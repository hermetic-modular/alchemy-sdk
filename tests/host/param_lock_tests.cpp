/**
 * @file tests/host/param_lock_tests.cpp
 * @brief ParamLock / ParamLockManager test battery.
 *
 * Covers the configurable-length automation surface end to end: the
 * record gesture, the millisecond sample clock (the part that makes
 * "20 seconds" mean twenty seconds at any frame rate), decimation and
 * playback interpolation, the serialized byte form, externally-placed
 * motion arenas, and the preset budget.
 */

#include "param_lock_tests.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "alchemy/control/lock_snap.h"
#include "alchemy/control/musical_clock.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/param_lock.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"

using namespace alchemy;

static daisy::QSPIHandle s_dummy_qspi;   /* stubbed on host */

/* ── Local check harness (totals returned to main) ─────────────────── */

static int s_checks   = 0;
static int s_failures = 0;

#define PCHECK(cond)                                                       \
    do {                                                                   \
        s_checks++;                                                        \
        if (!(cond)) {                                                     \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define PCHECK_EQ(a, b)                                                    \
    do {                                                                   \
        s_checks++;                                                        \
        const auto va = (a);                                               \
        const auto vb = (b);                                               \
        if (!(va == vb)) {                                                 \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s == %s  (%lld != %lld)\n",          \
                        __FILE__, __LINE__, #a, #b,                        \
                        (long long)va, (long long)vb);                     \
        }                                                                  \
    } while (0)

#define PCHECK_NEAR(a, b, tol)                                             \
    do {                                                                   \
        s_checks++;                                                        \
        const float va = (a);                                              \
        const float vb = (b);                                              \
        if (!(std::fabs(va - vb) <= (tol))) {                              \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s ~= %s  (%.6f vs %.6f, tol %g)\n",  \
                        __FILE__, __LINE__, #a, #b, va, vb, (double)(tol));\
        }                                                                  \
    } while (0)

namespace {

/* ── Fakes and drivers ─────────────────────────────────────────────── */

struct FakeButton : IButton
{
    bool pressed = false;

    bool  Pressed() const override { return pressed; }
    bool  RisingEdge() override { return false; }
    bool  FallingEdge() override { return false; }
    float TimeHeldMs() const override { return 0.0f; }
};

/** One control frame, in ControlLoop's order. */
template<class Locks>
void Frame(Locks& locks, const float* phys, uint32_t frame_ms)
{
    locks.SetFrameMs(frame_ms);
    locks.PollButtons(0u, false);
    locks.Advance();
    locks.Update(phys, 0u);
}

/** Pot position every gesture starts from — the gesture engine's baseline. */
constexpr float kRest = 0.5f;

/**
 * Hold the trigger and sweep one pot through `values`, then release.
 *
 * The first frame stays at kRest because OnButtonDown snapshots the
 * baseline there — a gesture already displaced on the press frame arms
 * nothing.  The lock arms on values[0], and record_base is the PRE-nudge
 * baseline (kRest): the loop's reference — and a Return exit's home — is
 * where the hand started, not where the arm threshold tripped.  The
 * recorded samples are `values`.
 */
template<class Locks, uint8_t kPots>
void RecordGesture(Locks& locks, FakeButton& button, uint8_t pot,
                   const std::vector<float>& values, uint32_t frame_ms)
{
    float phys[kPots] = {};
    for (uint8_t i = 0; i < kPots; i++) phys[i] = kRest;

    button.pressed = true;
    Frame(locks, phys, frame_ms);          /* baseline snapshot */

    for (float v : values)
    {
        phys[pot] = v;
        Frame(locks, phys, frame_ms);
    }

    button.pressed = false;
    Frame(locks, phys, frame_ms);
}

/**
 * Frames a playing lock takes to go once round its loop.
 *
 * The caller seeds a rising ramp and names an interior level; the ramp
 * crosses it once per loop.  Crossings rather than "Delta() dropped"
 * because playback interpolates the wrap over several frames, and
 * crossing-to-crossing rather than from frame zero because a lock that
 * auto-armed mid-hold is already playing when the caller looks.
 */
template<class Locks>
uint32_t FramesPerLoop(Locks& locks, uint8_t pot, uint32_t frame_ms,
                       float level, uint32_t limit = 100000u)
{
    const float phys[8] = {};
    float    prev  = locks.Delta(pot);
    bool     armed = false;      /* seen the first crossing yet? */
    uint32_t n     = 0u;

    for (uint32_t i = 0; i < limit; i++)
    {
        Frame(locks, phys, frame_ms);
        n++;
        const float now = locks.Delta(pot);
        const bool crossed = (prev < level && now >= level);
        prev = now;

        if (!crossed) continue;
        if (!armed) { armed = true; n = 0u; continue; }   /* start counting */
        return n;
    }
    return 0u;   /* never completed a loop */
}

/** Build a raw saved image for one slot (header + samples, zero tail). */
template<class Locks>
void PokeSlot(std::vector<uint8_t>& image, uint8_t slot, bool active,
              float record_base, const std::vector<float>& samples,
              uint16_t musical_ticks = 0u)
{
    uint8_t* p = image.data() + static_cast<size_t>(slot) * Locks::kBytesPerSavedSlot;

    ParamLockSavedHeader h;
    h.active        = active ? 1u : 0u;
    h.length        = static_cast<uint16_t>(samples.size());
    h.record_base   = LockQuant(record_base);
    h.musical_ticks = musical_ticks;
    std::memcpy(p, &h, sizeof h);

    for (size_t i = 0; i < samples.size(); i++)
    {
        const uint16_t q = LockQuant(samples[i]);
        std::memcpy(p + sizeof h + i * sizeof(uint16_t), &q, sizeof q);
    }
}

/* ── 1. Sample quantization ────────────────────────────────────────── */

void TestQuantization()
{
    /* Under half a step, so the stored gesture is finer than the ADC. */
    for (int i = 0; i <= 1000; i++)
    {
        const float v = static_cast<float>(i) / 1000.0f;
        PCHECK(std::fabs(LockDequant(LockQuant(v)) - v) <= 1.0f / 131070.0f);
    }

    PCHECK_EQ(LockQuant(0.0f), 0u);
    PCHECK_EQ(LockQuant(1.0f), 65535u);
    PCHECK(LockDequant(0u) == 0.0f);
    PCHECK(LockDequant(65535u) == 1.0f);

    /* Clamp, not wrap — a wrap turns a clipped gesture into a jump. */
    PCHECK_EQ(LockQuant(-0.5f), 0u);
    PCHECK_EQ(LockQuant(2.0f), 65535u);
    PCHECK_EQ(LockQuant(std::nanf("")), 0u);
}

/* ── 2. Configuration constants ────────────────────────────────────── */

void TestConfigConstants()
{
    using Default = ParamLock<6>;
    using Long    = ParamLock<6, LockLength<20>>;
    using Paged   = ParamLock<12, LockLength<30, 20>>;

    /* The default spends the old format's RAM (960 B/slot vs 1,024 B)
     * on 16 s of automation instead of 4. */
    PCHECK_EQ(Default::kMaxSeconds, 16u);
    PCHECK_EQ(Default::kSampleRateHz, 30u);
    PCHECK_EQ(Default::kFramesPerSlot, 480u);
    PCHECK(Default::kFramesPerSlot * sizeof(uint16_t) <= 1024u);

    /* A plain ParamLock<16> must keep compiling without a LockArena —
     * that is the upgrade story.  Pins DefaultLockLength's ceiling. */
    PCHECK(ParamLock<16>::kInlineArena);
    PCHECK(ParamLock<16>::kPresetBytes <= kPresetBlobCapacity);

    /* seconds x rate is the whole cost model. */
    PCHECK_EQ(Long::kFramesPerSlot, 600u);
    PCHECK_EQ(Paged::kFramesPerSlot, 600u);

    /* 7-byte header (PLK2: + musical_ticks) + 2 B per sample, per slot. */
    PCHECK_EQ(Long::kBytesPerSavedSlot, 7u + 600u * 2u);
    PCHECK_EQ(Long::kSavedBytes, 6u * (7u + 1200u));
    PCHECK_EQ(Long::kPresetBytes, Long::kSavedBytes);

    /* Arena is 2 B per sample and nothing else. */
    PCHECK_EQ(Long::kArenaSamples, 6u * 600u);
    PCHECK_EQ(Long::kArenaBytes, 6u * 600u * 2u);

    /* The headline configurations fit a preset slot. */
    PCHECK(Long::kPresetBytes  <= kPresetBlobCapacity);
    PCHECK(Paged::kPresetBytes <= kPresetBlobCapacity);
    PCHECK(Paged::kPresetBytes  == 12u * (7u + 1200u));

    /* RamOnly contributes nothing to a preset but keeps its byte form. */
    using RamOnly = ParamLock<6, LockLength<20, 30, LockStore::RamOnly>>;
    PCHECK_EQ(RamOnly::kPresetBytes, 0u);
    PCHECK_EQ(RamOnly::kSavedBytes, Long::kSavedBytes);

    /* Inline-vs-external is purely a size decision against the limit. */
    PCHECK(Default::kInlineArena);
    PCHECK(Long::kArenaBytes <= ALCHEMY_LOCK_INLINE_RAM_LIMIT);
    PCHECK(Long::kInlineArena);

    /* RamBytes() accounts for the object too, so it is strictly larger
     * than the arena it reports. */
    PCHECK(Long::RamBytes() > Long::kArenaBytes);
}

/* ── 3. Record → playback round trip ───────────────────────────────── */

/* Rate 50 Hz at 20 ms frames is exactly one sample per frame, which
 * isolates the record/playback path from the sample clock (tested
 * separately below). */
using ExactLocks = ParamLock<4, LockLength<4, 50>>;
constexpr uint32_t kExactFrameMs = 20u;

void TestRecordPlaybackRoundTrip()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    /* The first swept value arms the lock; deltas reference the
     * PRE-nudge baseline (kRest), the gesture's true origin. */
    const std::vector<float> sweep = {0.6f, 0.7f, 0.8f, 0.9f};
    RecordGesture<ExactLocks, 4>(locks, button, 1, sweep, kExactFrameMs);

    PCHECK(locks.IsActive(1));
    PCHECK(!locks.IsRecording(1));
    PCHECK(!locks.IsActive(0));      /* untouched pots stay clear */

    /* Playback reproduces the recorded deltas in order, and loops. */
    const float phys[4] = {};
    for (int loop = 0; loop < 2; loop++)
    {
        for (float v : sweep)
        {
            PCHECK_NEAR(locks.Delta(1), v - kRest, 1e-3f);
            Frame(locks, phys, kExactFrameMs);
        }
    }
}

void TestDisarmClearsSlot()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    RecordGesture<ExactLocks, 4>(locks, button, 2, {0.6f, 0.7f, 0.8f},
                                 kExactFrameMs);
    PCHECK(locks.IsActive(2));

    /* A second hold-and-nudge on an active slot clears it. */
    RecordGesture<ExactLocks, 4>(locks, button, 2, {0.6f}, kExactFrameMs);
    PCHECK(!locks.IsActive(2));
    PCHECK(!locks.IsRecording(2));
    PCHECK(locks.Delta(2) == 0.0f);
}

void TestBufferFullAutoArms()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    /* Hold past the configured length: recording must stop at exactly
     * kFramesPerSlot and arm playback, not overrun the arena. */
    float phys[4] = {kRest, kRest, kRest, kRest};
    button.pressed = true;
    Frame(locks, phys, kExactFrameMs);            /* baseline snapshot */

    /* Monotonically rising across the whole buffer, so FramesPerLoop's
     * "Delta went down" wrap detector sees exactly one drop per loop. */
    const uint32_t held = ExactLocks::kFramesPerSlot + 50u;
    for (uint32_t i = 0; i < held; i++)
    {
        phys[0] = 0.3f + 0.002f * static_cast<float>(i);
        Frame(locks, phys, kExactFrameMs);
        if (i == ExactLocks::kFramesPerSlot + 10u)
        {
            /* Already flipped to playback while the button is still down. */
            PCHECK(locks.IsActive(0));
            PCHECK(!locks.IsRecording(0));
        }
    }
    button.pressed = false;
    Frame(locks, phys, kExactFrameMs);

    PCHECK(locks.IsActive(0));

    /* kFramesPerSlot samples, one frame each at this cadence. */
    /* Deltas reference kRest: they span -0.2 .. 0.198; 0.1 is interior. */
    PCHECK_EQ(FramesPerLoop(locks, 0, kExactFrameMs, 0.1f),
              uint32_t{ExactLocks::kFramesPerSlot});
}

/* ── 4. The sample clock: seconds mean seconds ─────────────────────── */

void TestWallClockIndependentOfFrameRate()
{
    /* Before the buffer was clocked in milliseconds, "4 seconds" meant
     * "256 frames" — changing FrameMs rescaled every recording. */
    using Locks = ParamLock<4, LockLength<4, 50>>;
    FakeButton button;

    /* A 20-sample ramp: 20 samples at 50 Hz is 400 ms of automation, no
     * matter what the control loop is doing. */
    std::vector<float> ramp;
    for (int i = 0; i < 20; i++)
        ramp.push_back(0.1f + 0.04f * static_cast<float>(i));

    struct Case { uint32_t frame_ms; };
    const Case cases[] = {{10u}, {16u}, {20u}, {40u}};

    for (const Case& c : cases)
    {
        Locks locks(button);
        locks.Init();

        std::vector<uint8_t> image(Locks::kSavedBytes, 0u);
        PokeSlot<Locks>(image, 0, true, 0.0f, ramp);
        locks.Restore(image.data());

        /* Ramp deltas span 0.1 .. 0.86; 0.4 is an interior level. */
        const uint32_t frames = FramesPerLoop(locks, 0, c.frame_ms, 0.4f);
        PCHECK(frames > 0u);

        const uint32_t loop_ms = frames * c.frame_ms;

        /* 20 samples at 50 Hz = 400 ms.  Tolerance is one frame: the
         * wrap can only be observed on a frame boundary. */
        const bool ok = loop_ms + c.frame_ms >= 400u
                     && loop_ms <= 400u + c.frame_ms;
        if (!ok)
            std::printf("FAIL wall clock: frame_ms=%u -> %u frames = %u ms "
                        "(expected ~400 ms)\n",
                        c.frame_ms, frames, loop_ms);
        s_checks++;
        if (!ok) s_failures++;
    }
}

void TestPlaybackInterpolates()
{
    /* 0.4 samples per frame, so most frames land between stored
     * samples.  Holding instead of interpolating steps audibly. */
    using Locks = ParamLock<2, LockLength<4, 20>>;
    FakeButton button;
    Locks locks(button);
    locks.Init();

    std::vector<uint8_t> image(Locks::kSavedBytes, 0u);
    PokeSlot<Locks>(image, 0, true, 0.0f, {0.0f, 1.0f});
    locks.Restore(image.data());

    const float phys[2] = {};
    PCHECK_NEAR(locks.Delta(0), 0.0f, 1e-4f);

    /* 0.4, 0.8 — both strictly between the two stored samples. */
    Frame(locks, phys, 20u);
    PCHECK_NEAR(locks.Delta(0), 0.4f, 1e-3f);
    Frame(locks, phys, 20u);
    PCHECK_NEAR(locks.Delta(0), 0.8f, 1e-3f);

    /* Exactly zero, not 1e-7 — else a motionless lock detunes its knob. */
    Locks flat(button);
    flat.Init();
    std::vector<uint8_t> flat_image(Locks::kSavedBytes, 0u);
    PokeSlot<Locks>(flat_image, 0, true, 0.375f, {0.375f, 0.375f, 0.375f});
    flat.Restore(flat_image.data());
    for (int i = 0; i < 12; i++)
    {
        PCHECK(flat.Delta(0) == 0.0f);
        Frame(flat, phys, 20u);
    }
}

void TestRecordingBoxAverages()
{
    /* Several frames share one sample.  Point-sampling would make a
     * one-frame flick vanish or hit full amplitude depending on phase. */
    using Locks = ParamLock<2, LockLength<4, 10>>;   /* 10 Hz stored */
    FakeButton button;
    Locks locks(button);
    locks.Init();

    /* 20 ms frames, 10 Hz stored: 5 frames per stored sample.  Arm on
     * frame 0, then a single-frame excursion inside the first sample. */
    std::vector<float> gesture = {0.9f, 0.5f, 0.5f, 0.5f, 0.5f,
                                  0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    RecordGesture<Locks, 2>(locks, button, 0, gesture, 20u);

    PCHECK(locks.IsActive(0));

    /* The first stored sample averages the 0.9 arm frame with the 0.5
     * frames sharing its period.  Relative to record_base (kRest, the
     * pre-nudge baseline) that lands inside the raw +0.4 excursion. */
    const float first = locks.Delta(0);
    PCHECK(first < 0.4f);
    PCHECK(first > 0.0f);
    PCHECK_NEAR(first, (0.9f + 4.0f * 0.5f) / 5.0f - kRest, 3e-2f);
}

void TestSlowFrameHoldsLastSample()
{
    /* Control frame slower than the stored rate: 6 samples per frame,
     * one average and five zero-order holds.  The holds must repeat the
     * last sample — record_base would interleave the arm position into
     * every gap, a buzz at half the sample rate on playback. */
    using Locks = ParamLock<2, LockLength<4, 60>>;
    FakeButton button;
    Locks locks(button);
    locks.Init();

    /* Arm at 0.6 (nudge from rest), then hold two frames at 0.9. */
    RecordGesture<Locks, 2>(locks, button, 0, {0.6f, 0.9f, 0.9f}, 100u);
    PCHECK(locks.IsActive(0));

    std::vector<uint8_t> out(Locks::kSavedBytes, 0u);
    locks.Save(out.data());

    ParamLockSavedHeader h;
    std::memcpy(&h, out.data(), sizeof h);
    PCHECK_EQ(h.record_base, LockQuant(kRest));   /* pre-nudge baseline */
    PCHECK(h.length >= 12u);                 /* ~3 frames x 6 samples */

    /* Every sample from the second frame on is 0.9, averages and holds
     * alike.  The bug put quant(0.6) in the gaps. */
    for (uint16_t j = 6u; j < h.length; j++)
    {
        uint16_t q;
        std::memcpy(&q, out.data() + sizeof h + j * sizeof q, sizeof q);
        PCHECK_NEAR(LockDequant(q), 0.9f, 1e-3f);
    }
}

void TestShortGestureStillRecords()
{
    /* Shorter than one sample period (100 ms at 10 Hz).  Without the
     * flush in Finalise the slot ends up neither recording nor active. */
    using Locks = ParamLock<2, LockLength<4, 10>>;
    FakeButton button;
    Locks locks(button);
    locks.Init();

    RecordGesture<Locks, 2>(locks, button, 0, {0.9f, 0.9f}, 20u);

    PCHECK(locks.IsActive(0));
    PCHECK(!locks.IsRecording(0));
}

/* ── 5. Serialized byte form ───────────────────────────────────────── */

void TestSerializeRoundTrip()
{
    /* The byte stream is a pure function of musical content: same
     * gesture, same bytes, same slot CRC. */
    using Locks = ParamLock<4, LockLength<4, 60>>;
    FakeButton button;
    Locks locks(button);
    locks.Init();

    PCHECK_EQ(locks.SerializedSize(), Locks::kSavedBytes);

    std::vector<uint8_t> image(locks.SerializedSize(), 0u);
    PokeSlot<Locks>(image, 0, true, 0.25f,
                    {0.01f, 0.02f, 0.03f, 0.04f, 0.05f});

    /* Slot 2 uses the full motion buffer. */
    std::vector<float> full;
    full.reserve(Locks::kFramesPerSlot);
    for (uint32_t j = 0; j < Locks::kFramesPerSlot; j++)
        full.push_back((j & 1u) ? 0.75f : 0.25f);
    PokeSlot<Locks>(image, 2, true, 0.5f, full);

    PCHECK(locks.Deserialize(image.data()));

    std::vector<uint8_t> out(locks.SerializedSize(), 0xAAu);
    locks.Serialize(out.data());
    PCHECK(out == image);

    /* Inactive slots come back zeroed, not carrying arena residue. */
    const size_t off = Locks::kBytesPerSavedSlot;
    for (size_t i = 0; i < Locks::kBytesPerSavedSlot; i++)
        PCHECK(out[off + i] == 0u);
}

void TestSerializeRejectsMalformed()
{
    using Locks = ParamLock<2, LockLength<4, 60>>;
    FakeButton button;
    Locks locks(button);
    locks.Init();

    std::vector<uint8_t> image(Locks::kSavedBytes, 0u);

    /* A length past the stride — what a longer-configured build would
     * write, if the schema hash failed to gate it. */
    ParamLockSavedHeader h;
    h.active      = 1u;
    h.length      = static_cast<uint16_t>(Locks::kFramesPerSlot + 1u);
    h.record_base = LockQuant(0.5f);
    std::memcpy(image.data(), &h, sizeof h);

    PCHECK(locks.Deserialize(image.data()));
    PCHECK(!locks.IsActive(0));
    PCHECK(locks.Delta(0) == 0.0f);

    /* An active-but-empty entry is equally inert. */
    h.length = 0u;
    std::memcpy(image.data(), &h, sizeof h);
    PCHECK(locks.Deserialize(image.data()));
    PCHECK(!locks.IsActive(0));
}

void TestAdvanceSplit()
{
    /* Playback lives in Advance(), not Update(), so ControlLoop keeps
     * automation running while Settings suspends the gesture path. */
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    std::vector<uint8_t> image(ExactLocks::kSavedBytes, 0u);
    PokeSlot<ExactLocks>(image, 0, true, 0.5f, {0.6f, 0.7f, 0.9f});
    PCHECK(locks.Deserialize(image.data()));

    const float phys[4] = {};
    PCHECK_NEAR(locks.Delta(0), 0.1f, 1e-3f);

    /* Update() with no pending edges must not move the play head. */
    locks.Update(phys, 0u);
    locks.Update(phys, 1u);
    PCHECK_NEAR(locks.Delta(0), 0.1f, 1e-3f);

    /* Advance() steps the loop — including through the LockSource base,
     * which is how ControlLoop drives it. */
    locks.SetFrameMs(kExactFrameMs);
    locks.Advance();
    PCHECK_NEAR(locks.Delta(0), 0.2f, 1e-3f);
    static_cast<LockSource&>(locks).Advance();
    PCHECK_NEAR(locks.Delta(0), 0.4f, 1e-3f);
    locks.Advance();
    PCHECK_NEAR(locks.Delta(0), 0.1f, 1e-3f);   /* wrapped */

    /* Inactive slots are unaffected either way. */
    PCHECK(locks.Delta(1) == 0.0f);
}

void TestSchemaHashSeparatesConfigurations()
{
    FakeButton button;

    ParamLock<4, LockLength<4, 60>>                     a(button);
    ParamLock<4, LockLength<4, 60>>                     a2(button);
    ParamLock<4, LockLength<20, 60>>                    longer(button);
    ParamLock<4, LockLength<4, 30>>                     slower(button);
    ParamLock<6, LockLength<4, 60>>                     wider(button);
    ParamLock<4, LockLength<4, 60, LockStore::RamOnly>> ram(button);

    /* Same shape, same hash. */
    PCHECK_EQ(a.SchemaHash(), a2.SchemaHash());

    /* Anything that changes the bytes, or their meaning, changes it. */
    PCHECK(a.SchemaHash() != longer.SchemaHash());
    PCHECK(a.SchemaHash() != slower.SchemaHash());
    PCHECK(a.SchemaHash() != wider.SchemaHash());
    PCHECK(a.SchemaHash() != ram.SchemaHash());
    PCHECK(longer.SchemaHash() != slower.SchemaHash());
}

void TestRamOnlyStaysOutOfPresets()
{
    using Ram = ParamLock<4, LockLength<30, 60, LockStore::RamOnly>>;
    FakeButton button;
    Ram locks(button);
    locks.Init();

    /* Contributes nothing to a preset slot... */
    PCHECK_EQ(locks.SerializedSize(), 0u);

    std::vector<uint8_t> guard(16u, 0x5Au);
    locks.Serialize(guard.data());
    for (uint8_t b : guard) PCHECK(b == 0x5Au);   /* wrote nothing */

    /* ...but the byte form still works by hand. */
    std::vector<uint8_t> image(Ram::kSavedBytes, 0u);
    PokeSlot<Ram>(image, 1, true, 0.5f, {0.6f, 0.7f});
    locks.Restore(image.data());
    PCHECK(locks.IsActive(1));
    PCHECK_NEAR(locks.Delta(1), 0.1f, 1e-3f);

    std::vector<uint8_t> out(Ram::kSavedBytes, 0xAAu);
    locks.Save(out.data());
    PCHECK(out == image);
}

/* ── 6. Externally-placed motion arenas ────────────────────────────── */

/* Past ALCHEMY_LOCK_INLINE_RAM_LIMIT the arena must be supplied by the
 * application — on target this is where ALCHEMY_SDRAM_BSS goes. */
using BigLocks = ParamLock<8, LockLength<60, 60, LockStore::RamOnly>>;
static uint16_t s_big_arena[BigLocks::kArenaSamples];

void TestExternalArena()
{
    PCHECK(!BigLocks::kInlineArena);
    PCHECK(BigLocks::kArenaBytes > ALCHEMY_LOCK_INLINE_RAM_LIMIT);

    /* Only the arena is out of line; the object stays small. */
    PCHECK(sizeof(BigLocks) < 1024u);
    PCHECK_EQ(BigLocks::RamBytes(), sizeof(BigLocks) + BigLocks::kArenaBytes);

    FakeButton button;
    BigLocks locks(button, LockArena{s_big_arena, sizeof s_big_arena});
    locks.Init();
    PCHECK(locks.IsReady());

    std::vector<uint8_t> image(BigLocks::kSavedBytes, 0u);
    PokeSlot<BigLocks>(image, 7, true, 0.2f, {0.3f, 0.4f, 0.5f});
    locks.Restore(image.data());

    PCHECK(locks.IsActive(7));
    PCHECK_NEAR(locks.Delta(7), 0.1f, 1e-3f);

    /* Writes landed in the caller's arena, at the right slot's stride. */
    PCHECK_EQ(s_big_arena[7u * BigLocks::kFramesPerSlot], LockQuant(0.3f));
}

void TestShortArenaIsRefused()
{
    /* A short arena makes the surface inert rather than running off the
     * end.  Runtime, because the length is the user's number. */
    static uint16_t small[16];
    FakeButton button;
    BigLocks locks(button, LockArena{small, sizeof small});

    PCHECK(!locks.IsReady());

    locks.Init();                       /* must not touch `small` */
    PCHECK(locks.Delta(0) == 0.0f);
    PCHECK(!locks.IsActive(0));

    std::vector<uint8_t> image(BigLocks::kSavedBytes, 0u);
    PokeSlot<BigLocks>(image, 0, true, 0.2f, {0.3f, 0.4f});
    locks.Restore(image.data());        /* refused, not written */
    PCHECK(!locks.IsActive(0));
    for (uint16_t v : small) PCHECK(v == 0u);

    /* An inert surface still owes a full image: headers only would
     * leave gaps uninitialised, and that buffer is a preset slot. */
    std::vector<uint8_t> out(BigLocks::kSavedBytes, 0xC3u);
    locks.Save(out.data());
    PCHECK(out == std::vector<uint8_t>(BigLocks::kSavedBytes, 0u));
}

/* ── 7. Paged addressing ───────────────────────────────────────────── */

void TestPagedSlotAddressing()
{
    using Locks = ParamLock<12, LockLength<4, 60>>;   /* 2 pages x 6 pots */
    FakeButton button;
    Pager pager(button, 2, 6);
    Locks locks(button, pager);
    locks.Init();

    /* Distinct content on page 1, pot 2 — flat index 1*6 + 2 = 8. */
    std::vector<uint8_t> image(Locks::kSavedBytes, 0u);
    PokeSlot<Locks>(image, 8, true, 0.5f, {0.7f, 0.7f});
    locks.Restore(image.data());

    PCHECK(locks.IsActiveAtPage(1, 2));
    PCHECK_NEAR(locks.DeltaAtPage(1, 2), 0.2f, 1e-3f);

    /* Not visible from page 0 — the pager starts there. */
    PCHECK_EQ(pager.Page(), 0u);
    PCHECK(!locks.IsActive(2));
    PCHECK(locks.Delta(2) == 0.0f);
    PCHECK(!locks.AnyActive());

    /* Inactive-page reads are still available by explicit index, which
     * is what a composition site uses for off-page parameters. */
    PCHECK(!locks.IsActiveAtPage(0, 2));
}

/* ── 8. Preset budgeting ───────────────────────────────────────────── */

struct DummySerializable : Serializable
{
    size_t   SerializedSize() const override { return 64u; }
    void     Serialize(uint8_t* out) const override { std::memset(out, 0, 64u); }
    bool     Deserialize(const uint8_t*) override { return true; }
    uint32_t SchemaHash() const override { return 0x1234u; }
};

void TestPresetBudget()
{
    Presets presets{s_dummy_qspi};

    FakeButton button;
    ParamLock<12, LockLength<30, 20>> locks(button);
    DummySerializable extra;

    PCHECK_EQ(Presets::Capacity(), kPresetBlobCapacity);
    PCHECK_EQ(presets.PayloadBytes(), 0u);
    PCHECK(presets.FitsInSlot());

    presets.Manage(locks);
    presets.Manage(extra);

    /* Readable at boot, before the first Save returns false. */
    PCHECK_EQ(presets.PayloadBytes(),
              decltype(locks)::kPresetBytes + 64u);
    PCHECK(presets.FitsInSlot());
    PCHECK(presets.PayloadBytes() <= Presets::Capacity());

    /* Fits, with room left for other components. */
    PCHECK(decltype(locks)::kPresetBytesFree > 5000u);
}

/* ── 9. Gesture thresholds ─────────────────────────────────────────── */

void TestArmAndClearThresholds()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    /* Below the arm threshold (default 0.005): nothing arms. */
    RecordGesture<ExactLocks, 4>(locks, button, 0, {0.502f, 0.502f},
                                 kExactFrameMs);
    PCHECK(!locks.IsActive(0));
    PCHECK(!locks.IsRecording(0));

    /* A 0.02 nudge arms — under the OLD single threshold (0.03) this
     * gesture was silently ignored, which is the timing complaint the
     * arm/clear split exists to fix. */
    RecordGesture<ExactLocks, 4>(locks, button, 0, {0.52f, 0.53f},
                                 kExactFrameMs);
    PCHECK(locks.IsActive(0));

    /* The same 0.02 nudge on the now-active slot does NOT clear it:
     * clearing needs the larger clear threshold (default 0.03), so a
     * brush against a playing pot mid-hold is survivable. */
    RecordGesture<ExactLocks, 4>(locks, button, 0, {0.52f}, kExactFrameMs);
    PCHECK(locks.IsActive(0));

    /* A firmer 0.04 nudge clears. */
    RecordGesture<ExactLocks, 4>(locks, button, 0, {0.54f}, kExactFrameMs);
    PCHECK(!locks.IsActive(0));

    /* Both thresholds are configurable. */
    locks.ArmThreshold(0.10f);
    RecordGesture<ExactLocks, 4>(locks, button, 1, {0.55f}, kExactFrameMs);
    PCHECK(!locks.IsActive(1));                    /* 0.05 < 0.10 */
    RecordGesture<ExactLocks, 4>(locks, button, 1, {0.65f, 0.7f},
                                 kExactFrameMs);
    PCHECK(locks.IsActive(1));                     /* 0.15 ≥ 0.10 */
}

/* ── 10. Clocked mode ──────────────────────────────────────────────── */

/* 120 BPM at 96 PPQN: 192 ticks/s — a half note is 192 ticks = 1000 ms.
 * Rate 50 Hz at 20 ms frames keeps 1 sample per frame. */
constexpr uint32_t kTestPpqn = 96u;

/** One control frame with the clock ticking, in ControlLoop's order. */
template<class Locks>
void ClockedFrame(Locks& locks, MusicalClock& clk, uint32_t& t_us,
                  const float* phys, uint32_t frame_ms)
{
    t_us += frame_ms * 1000u;
    clk.Tick(t_us);
    locks.SetFrameMs(frame_ms);
    locks.PollButtons(0u, false);
    locks.Advance();
    locks.Update(phys, 0u);
}

template<class Locks, uint8_t kPots>
void ClockedRecordGesture(Locks& locks, MusicalClock& clk, uint32_t& t_us,
                          FakeButton& button, uint8_t pot,
                          const std::vector<float>& values, uint32_t frame_ms)
{
    float phys[kPots];
    for (uint8_t i = 0; i < kPots; i++) phys[i] = kRest;

    button.pressed = true;
    ClockedFrame(locks, clk, t_us, phys, frame_ms);   /* baseline */
    for (float v : values)
    {
        phys[pot] = v;
        ClockedFrame(locks, clk, t_us, phys, frame_ms);
    }
    button.pressed = false;
    ClockedFrame(locks, clk, t_us, phys, frame_ms);
}

/** FramesPerLoop with the clock running — crossing-to-crossing. */
template<class Locks>
uint32_t ClockedFramesPerLoop(Locks& locks, MusicalClock& clk, uint32_t& t_us,
                              uint8_t pot, uint32_t frame_ms, float level,
                              uint32_t limit = 100000u)
{
    const float phys[8] = {};
    float    prev  = locks.Delta(pot);
    bool     armed = false;
    uint32_t n     = 0u;

    for (uint32_t i = 0; i < limit; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, frame_ms);
        n++;
        const float now = locks.Delta(pot);
        const bool crossed = (prev < level && now >= level);
        prev = now;
        if (!crossed) continue;
        if (!armed) { armed = true; n = 0u; continue; }
        return n;
    }
    return 0u;
}

/** A rising n-frame ramp in 0.006 steps: sample k plays back as delta
 *  0.006·(k+1).  Every step clears the arm threshold, so all n frames
 *  record.  Default 51 frames = 1020 ms at the 20 ms cadence. */
std::vector<float> ClockedTestRamp(int n = 51)
{
    std::vector<float> ramp;
    for (int i = 1; i <= n; i++)
        ramp.push_back(kRest + 0.006f * static_cast<float>(i));
    return ramp;
}

/** Drive frames until Delta drops by more than 0.1 (the loop restart),
 *  then return.  The NEXT ClockedFrame is cycle-frame 1 (sample 1). */
template<class Locks>
bool DriveToRestart(Locks& locks, MusicalClock& clk, uint32_t& t_us,
                    uint8_t pot, uint32_t frame_ms, uint32_t limit = 1000u)
{
    const float phys[8] = {};
    float prev = locks.Delta(pot);
    for (uint32_t i = 0; i < limit; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, frame_ms);
        const float now = locks.Delta(pot);
        if (now < prev - 0.1f) return true;
        prev = now;
    }
    return false;
}

uint16_t SavedTicks(const std::vector<uint8_t>& image, size_t slot,
                    size_t bytes_per_slot)
{
    ParamLockSavedHeader h;
    std::memcpy(&h, image.data() + slot * bytes_per_slot, sizeof h);
    return h.musical_ticks;
}

void TestClockedLengthSnapsToGrid()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);

    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);   /* Clocked */

    /* The motivating case: a 1020 ms take against a 1000 ms half note
     * loops as exactly the half note — the boundary snaps; the last
     * ~20 ms of motion is simply never reached. */
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(), kExactFrameMs);
    PCHECK(locks.IsActive(0));

    std::vector<uint8_t> image(ExactLocks::kSavedBytes, 0u);
    locks.Save(image.data());
    PCHECK_EQ(SavedTicks(image, 0, ExactLocks::kBytesPerSavedSlot), 192u);

    /* 192 ticks at 120 BPM = 1000 ms = 50 frames (51 were recorded). */
    const uint32_t frames =
        ClockedFramesPerLoop(locks, clk, t_us, 0, kExactFrameMs, 0.15f);
    PCHECK(frames >= 49u && frames <= 51u);
}

void TestClockedTrimsLongTakes()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);

    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);

    /* 55 samples (1100 ms, deltas up to 0.33) snap to the 1000 ms half
     * note.  The motion must play 1:1 and get RIGHT-TRIMMED: the head
     * never reaches samples 50..54, so the peak delta is sample 49's
     * 0.300 — a time-stretch would compress the full 0.33 ramp into the
     * loop and hit 0.33. */
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(55), kExactFrameMs);

    const float phys[4] = {};
    float peak = 0.0f;
    for (int i = 0; i < 120; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        if (locks.Delta(0) > peak) peak = locks.Delta(0);
    }
    PCHECK(peak > 0.29f);
    PCHECK(peak < 0.31f);

    const uint32_t frames =
        ClockedFramesPerLoop(locks, clk, t_us, 0, kExactFrameMs, 0.15f);
    PCHECK(frames >= 49u && frames <= 51u);
}

void TestClockedHoldsShortTakes()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);

    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);

    /* 45 samples (900 ms) snap to the 1000 ms half note: the motion
     * plays 1:1 for 45 frames, HOLDS its final value (0.27) for ~5
     * frames, and restarts on the grid. */
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(45), kExactFrameMs);
    PCHECK(DriveToRestart(locks, clk, t_us, 0, kExactFrameMs));

    const float phys[4] = {};
    float at[54];
    for (int k = 1; k <= 53; k++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        at[k] = locks.Delta(0);
    }

    /* 1:1 through the motion: cycle-frame k is sample k, NOT the
     * 0.9×-stretched sample a fit-to-span playback would give. */
    PCHECK_NEAR(at[20], 0.006f * 21.0f, 5e-3f);
    PCHECK_NEAR(at[40], 0.006f * 41.0f, 5e-3f);

    /* The hold tail: parked exactly on the final sample. */
    PCHECK_NEAR(at[46], 0.006f * 45.0f, 2e-3f);
    PCHECK(at[47] == at[46]);
    PCHECK(at[48] == at[46]);

    /* And the restart lands on the 50-frame grid (±1 frame of clock
     * integration rounding). */
    int restart = -1;
    for (int k = 49; k <= 52 && restart < 0; k++)
        if (at[k] < 0.05f) restart = k;
    PCHECK(restart >= 49 && restart <= 51);
}

void TestClockedSnapSurvivesFrameTimingLie()
{
    /* The hardware repro: a lock at the stock 30 Hz stored rate on a
     * control loop that DECLARES 16 ms frames but actually runs ~32 ms
     * (control_loop.cpp's poll loop adds its work to the nominal delay,
     * so the real frame period always exceeds frame_ms).  Four beats of
     * motion at 120 BPM must snap to the whole bar and play back in
     * full.  Deriving the duration from sample count ÷ rate measured
     * this take at HALF length — snapping to the half note and trimming
     * two of the four performed beats. */
    using RigLocks = ParamLock<4>;               /* LockLength<16, 30> */
    FakeButton button;
    RigLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);

    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);

    constexpr uint32_t kDeclaredMs = 16u;
    constexpr uint32_t kRealMs     = 32u;

    float phys[4] = {kRest, kRest, kRest, kRest};
    auto lie_frame = [&]()
    {
        t_us += kRealMs * 1000u;                 /* reality */
        clk.Tick(t_us);
        locks.SetFrameMs(kDeclaredMs);           /* the loop's belief */
        locks.PollButtons(0u, false);
        locks.Advance();
        locks.Update(phys, 0u);
    };

    /* 63 real frames × 32 ms ≈ 2016 ms ≈ four beats of motion. */
    button.pressed = true;
    lie_frame();
    for (int i = 1; i <= 63; i++)
    {
        phys[0] = kRest + 0.006f * static_cast<float>(i);
        lie_frame();
    }
    button.pressed = false;
    lie_frame();
    PCHECK(locks.IsActive(0));

    /* Snapped to the whole bar (384 ticks), not the half note. */
    std::vector<uint8_t> image(RigLocks::kSavedBytes, 0u);
    locks.Save(image.data());
    PCHECK_EQ(SavedTicks(image, 0, RigLocks::kBytesPerSavedSlot), 384u);

    /* Restart period spans the full take: 384 ticks = 2000 ms real
     * = ~62.5 real frames — the whole four beats replay each bar. */
    float    prev   = locks.Delta(0);
    bool     armed  = false;
    uint32_t n      = 0u;
    uint32_t frames = 0u;
    for (uint32_t i = 0; i < 4000u && frames == 0u; i++)
    {
        lie_frame();
        n++;
        const float now_d   = locks.Delta(0);
        const bool  crossed = (prev < 0.15f && now_d >= 0.15f);
        prev = now_d;
        if (!crossed) continue;
        if (!armed) { armed = true; n = 0u; }
        else        frames = n;
    }
    PCHECK(frames >= 61u && frames <= 64u);
}

void TestClockedTracksTempoChange()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);

    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);

    /* 45 samples (900 ms), snapped to the 192-tick half note. */
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(45), kExactFrameMs);

    /* Halve the tempo: the BOUNDARY moves (192 ticks now spans 2000 ms
     * = 100 frames) but the MOTION does not — it still plays its 45
     * frames at recorded speed, then holds until the restart. */
    clk.SetBpm(60.0f);
    const uint32_t slow =
        ClockedFramesPerLoop(locks, clk, t_us, 0, kExactFrameMs, 0.15f);
    PCHECK(slow >= 99u && slow <= 101u);

    PCHECK(DriveToRestart(locks, clk, t_us, 0, kExactFrameMs));
    const float phys[4] = {};
    float at20 = 0.f;
    for (int k = 1; k <= 20; k++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        at20 = locks.Delta(0);
    }
    /* Still sample 20 at frame 20 — a stretch-to-span would be at half
     * that position (~0.063) in the doubled loop. */
    PCHECK_NEAR(at20, 0.006f * 21.0f, 5e-3f);

    /* Double the tempo past the motion length: 192 ticks = 500 ms = 25
     * frames — the take is truncated to the first 25 samples each pass. */
    clk.SetBpm(240.0f);
    const uint32_t fast =
        ClockedFramesPerLoop(locks, clk, t_us, 0, kExactFrameMs, 0.10f);
    PCHECK(fast >= 24u && fast <= 26u);
}

void TestClockedFollowsTransportStop()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);

    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);

    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(), kExactFrameMs);

    const float phys[4] = {};
    for (int i = 0; i < 10; i++) ClockedFrame(locks, clk, t_us, phys,
                                              kExactFrameMs);

    /* Transport stop halts MasterTick — the loop freezes in place. */
    clk.Stop();
    ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);   /* applies stop */
    const float held = locks.Delta(0);
    for (int i = 0; i < 25; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        PCHECK(locks.Delta(0) == held);
    }

    /* Start resumes advancement. */
    clk.Start();
    bool moved = false;
    for (int i = 0; i < 5; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        if (locks.Delta(0) != held) moved = true;
    }
    PCHECK(moved);
}

void TestClockResetReAnchors()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);

    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);

    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(), kExactFrameMs);

    /* Reset() zeroes MasterTick — the loop must re-anchor, not compute
     * a negative phase.  Loop length is preserved. */
    clk.Reset();
    const uint32_t frames =
        ClockedFramesPerLoop(locks, clk, t_us, 0, kExactFrameMs, 0.15f);
    PCHECK(frames >= 49u && frames <= 51u);
}

void TestClockedFallsBackWithoutRunningClock()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    /* Clocked selected, clock wired but never started: the mature
     * fallback records the slot free (musical_ticks 0) and it replays
     * at its recorded wall-clock length. */
    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);           /* rate set, but transport never runs */
    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);

    RecordGesture<ExactLocks, 4>(locks, button, 0, ClockedTestRamp(),
                                 kExactFrameMs);
    PCHECK(locks.IsActive(0));

    std::vector<uint8_t> image(ExactLocks::kSavedBytes, 0u);
    locks.Save(image.data());
    PCHECK_EQ(SavedTicks(image, 0, ExactLocks::kBytesPerSavedSlot), 0u);

    /* All 51 samples replay in 51 frames — wall-clock, no snap. */
    PCHECK_EQ(FramesPerLoop(locks, 0, kExactFrameMs, 0.15f), 51u);
}

void TestModeAffectsNewRecordingsOnly()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);
    locks.UseClock(clk);

    /* Recorded under Free: stays free forever. */
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(), kExactFrameMs);

    /* Flip to Clocked (as the Settings selector would); record another. */
    static_cast<LockSource&>(locks).SetSyncMode(1u);
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 1,
                                        ClockedTestRamp(), kExactFrameMs);

    std::vector<uint8_t> image(ExactLocks::kSavedBytes, 0u);
    locks.Save(image.data());
    PCHECK_EQ(SavedTicks(image, 0, ExactLocks::kBytesPerSavedSlot), 0u);
    PCHECK_EQ(SavedTicks(image, 1, ExactLocks::kBytesPerSavedSlot), 192u);

    /* Clocked mode must not touch SAMPLING in any way: the same gesture
     * recorded under Free and under Clocked stores identical sample
     * data (same count, same base, same bytes).  The clock names the
     * restart interval — musical_ticks above — and nothing else. */
    ParamLockSavedHeader h_free, h_clk;
    const uint8_t* s_free = image.data();
    const uint8_t* s_clk  = image.data() + ExactLocks::kBytesPerSavedSlot;
    std::memcpy(&h_free, s_free, sizeof h_free);
    std::memcpy(&h_clk,  s_clk,  sizeof h_clk);
    PCHECK_EQ(h_free.length, h_clk.length);
    PCHECK_EQ(h_free.record_base, h_clk.record_base);
    PCHECK_EQ(std::memcmp(s_free + sizeof h_free, s_clk + sizeof h_clk,
                          static_cast<size_t>(h_free.length)
                              * sizeof(uint16_t)),
              0);
}

void TestRestoredClockedLocksShareAlignment()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);
    locks.UseClock(clk);

    /* Two clocked slots restored from a preset anchor at the master
     * origin, so equal musical lengths come back mutually phase-locked
     * even though their sample counts differ. */
    std::vector<uint8_t> image(ExactLocks::kSavedBytes, 0u);
    PokeSlot<ExactLocks>(image, 0, true, 0.2f,
                         {0.2f, 0.4f, 0.6f, 0.8f}, 192u);
    PokeSlot<ExactLocks>(image, 2, true, 0.1f,
                         {0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 0.9f, 0.1f, 0.5f},
                         192u);
    locks.Restore(image.data());

    const float phys[4] = {};
    for (int i = 0; i < 37; i++)
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);

    PCHECK_NEAR(locks.PlayPhase(0), locks.PlayPhase(2), 1e-4f);
    PCHECK(locks.PlayPhase(0) > 0.0f);
}

/* ── 11. Freeze / ClearSlot / PlayPhase ────────────────────────────── */

void TestFreezeHoldsPlayheads()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);
    locks.UseClock(clk);

    /* One free slot, one clocked slot. */
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        {0.6f, 0.7f, 0.8f, 0.9f},
                                        kExactFrameMs);
    static_cast<LockSource&>(locks).SetSyncMode(1u);
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 1,
                                        ClockedTestRamp(), kExactFrameMs);

    const float phys[4] = {};
    for (int i = 0; i < 5; i++)
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);

    PCHECK(!locks.Frozen());
    locks.Freeze(true);
    PCHECK(locks.Frozen());

    const float d_free    = locks.Delta(0);
    const float p_clocked = locks.PlayPhase(1);
    for (int i = 0; i < 20; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        PCHECK(locks.Delta(0) == d_free);
        PCHECK(locks.PlayPhase(1) == p_clocked);
    }

    /* Resume continues from the held position — the clocked anchor is
     * slid by the frozen span, so there is no jump to "where the clock
     * got to".  One frame at 20 ms is 1/50th of the 1000 ms loop. */
    locks.Freeze(false);
    ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
    float expect = p_clocked + 0.02f;
    if (expect >= 1.0f) expect -= 1.0f;
    PCHECK_NEAR(locks.PlayPhase(1), expect, 5e-3f);
}

/* Recording is unaffected by Freeze — so a recording can also FINISH
 * while frozen.  Such a slot must not inherit the whole frozen span on
 * resume: its boundary starts at the resume point. */
void TestRecordingFinishedWhileFrozen()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    MusicalClock clk(kTestPpqn, 4u);
    clk.SetBpm(120.0f);
    clk.Start();
    uint32_t t_us = 1000u;
    clk.Tick(t_us);
    locks.UseClock(clk);
    static_cast<LockSource&>(locks).SetSyncMode(1u);   /* Clocked */

    locks.Freeze(true);

    /* The whole take happens under Freeze.  The span still snaps
     * normally — duration is measured against the clock, which Freeze
     * does not stop. */
    ClockedRecordGesture<ExactLocks, 4>(locks, clk, t_us, button, 0,
                                        ClockedTestRamp(), kExactFrameMs);
    PCHECK(locks.IsActive(0));

    std::vector<uint8_t> image(ExactLocks::kSavedBytes, 0u);
    locks.Save(image.data());
    PCHECK_EQ(SavedTicks(image, 0, ExactLocks::kBytesPerSavedSlot), 192u);

    /* Held at phase 0 while still frozen. */
    const float phys[4] = {};
    for (int i = 0; i < 5; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        PCHECK_NEAR(locks.PlayPhase(0), 0.0f, 1e-6f);
    }

    /* Resume: the boundary begins here, so the first restart lands one
     * full span later — 192 ticks = 1000 ms = 50 frames. */
    locks.Freeze(false);
    PCHECK_NEAR(locks.PlayPhase(0), 0.0f, 1e-3f);

    uint32_t frames  = 0u;
    float    prev_ph = locks.PlayPhase(0);
    for (uint32_t i = 0; i < 200u; i++)
    {
        ClockedFrame(locks, clk, t_us, phys, kExactFrameMs);
        frames++;
        const float ph = locks.PlayPhase(0);
        if (ph < prev_ph) break;   /* wrapped: first restart */
        prev_ph = ph;
    }
    PCHECK(frames >= 49u && frames <= 51u);
}

void TestClearSlotIsIsolated()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    RecordGesture<ExactLocks, 4>(locks, button, 0, {0.6f, 0.7f},
                                 kExactFrameMs);
    RecordGesture<ExactLocks, 4>(locks, button, 2, {0.7f, 0.8f},
                                 kExactFrameMs);
    PCHECK(locks.IsActive(0));
    PCHECK(locks.IsActive(2));

    locks.ClearSlot(2);
    PCHECK(!locks.IsActive(2));
    PCHECK(locks.Delta(2) == 0.0f);
    PCHECK(locks.IsActive(0));          /* untouched */
}

void TestPlayPhaseReportsHeadAndWriteFill()
{
    FakeButton button;
    ExactLocks locks(button);
    locks.Init();

    std::vector<uint8_t> image(ExactLocks::kSavedBytes, 0u);
    PokeSlot<ExactLocks>(image, 0, true, 0.5f, {0.5f, 0.6f, 0.7f, 0.8f});
    locks.Restore(image.data());

    const float phys[4] = {};
    PCHECK_NEAR(locks.PlayPhase(0), 0.00f, 1e-4f);
    Frame(locks, phys, kExactFrameMs);
    PCHECK_NEAR(locks.PlayPhase(0), 0.25f, 1e-3f);
    Frame(locks, phys, kExactFrameMs);
    PCHECK_NEAR(locks.PlayPhase(0), 0.50f, 1e-3f);

    /* While recording, PlayPhase is the write head against capacity. */
    float rec[4] = {kRest, kRest, kRest, kRest};
    button.pressed = true;
    Frame(locks, rec, kExactFrameMs);
    rec[1] = 0.7f;
    for (int i = 0; i < 10; i++) Frame(locks, rec, kExactFrameMs);
    PCHECK(locks.IsRecording(1));
    PCHECK_NEAR(locks.PlayPhase(1),
                10.0f / static_cast<float>(ExactLocks::kFramesPerSlot),
                2.0f / static_cast<float>(ExactLocks::kFramesPerSlot));
    button.pressed = false;
    Frame(locks, rec, kExactFrameMs);
}

/* ── 12. Exit modes ────────────────────────────────────────────────── */

void TestLatchExitLeavesBaseAlone()
{
    using Locks = ParamLock<4, LockLength<4, 50>>;   /* 1 page × 4 pots */
    FakeButton lock_btn, page_btn;
    Pager pager(page_btn, 1, 4);
    Locks locks(lock_btn, pager);
    locks.Init();

    /* Catch the pager at the gesture's end position first. */
    float phys[4] = {kRest, kRest, kRest, kRest};
    pager.Update(phys, 0u);

    RecordGesture<Locks, 4>(locks, lock_btn, 1, {0.6f, 0.7f, 0.8f},
                            kExactFrameMs);
    phys[1] = 0.8f;
    pager.Update(phys, 0u);

    /* Latch (default): the pot's stored value is wherever the pager put
     * it — the lock exit wrote nothing. */
    PCHECK_NEAR(pager.Stored(0, 1), 0.8f, 1e-4f);
    PCHECK(pager.State(0, 1).caught);
}

void TestReturnExitRestoresOriginAndRearmsCatch()
{
    using Locks = ParamLock<4, LockLength<4, 50>>;
    FakeButton lock_btn, page_btn;
    Pager pager(page_btn, 1, 4);
    Locks locks(lock_btn, pager);
    locks.Init();
    locks.ExitMode(LockExit::Return);

    float phys[4] = {kRest, kRest, kRest, kRest};
    pager.Update(phys, 0u);

    RecordGesture<Locks, 4>(locks, lock_btn, 1, {0.6f, 0.7f, 0.8f},
                            kExactFrameMs);

    /* The base snapped back to the gesture origin (kRest) and catch
     * re-armed: the pot, still physically at 0.8, is dead until it
     * comes back through the stored value. */
    PCHECK_NEAR(pager.Stored(0, 1), kRest, 1e-4f);
    PCHECK(!pager.State(0, 1).caught);

    phys[1] = 0.8f;
    pager.Update(phys, 0u);
    PCHECK_NEAR(pager.Stored(0, 1), kRest, 1e-4f);   /* still parked */
    PCHECK(!pager.State(0, 1).caught);

    /* Crossing the origin catches, exactly like a page switch. */
    phys[1] = 0.45f;
    pager.Update(phys, 0u);
    PCHECK(pager.State(0, 1).caught);

    /* Playback reproduces the recording relative to the restored base:
     * total = stored + delta = recording, exactly as captured. */
    PCHECK(locks.IsActive(1));

    /* A cleared slot must NOT return-snap: clear pot 1 (a 0.04 nudge,
     * past the clear threshold) and check stored stays caught wherever
     * the pot is — the exit hook only touches slots that ended active. */
    phys[1] = 0.45f;
    RecordGesture<Locks, 4>(locks, lock_btn, 1, {0.54f}, kExactFrameMs);
    PCHECK(!locks.IsActive(1));
    pager.Update(phys, 0u);
    PCHECK(pager.State(0, 1).caught);
}

void TestExitModeLockSourceBinding()
{
    using Locks = ParamLock<4, LockLength<4, 50>>;

    /* Unpaged: Return has no Pager to write into. */
    FakeButton btn;
    Locks unpaged(btn);
    unpaged.Init();
    LockSource& u = unpaged;
    PCHECK(!u.CanReturn());
    PCHECK_EQ(u.ExitMode(), static_cast<uint8_t>(LockExit::Latch));

    /* Paged: the settings push lands on the surface's exit policy and
     * behaves exactly like the fluent ExitMode(Return). */
    FakeButton lock_btn, page_btn;
    Pager pager(page_btn, 1, 4);
    Locks locks(lock_btn, pager);
    locks.Init();
    LockSource& p = locks;
    PCHECK(p.CanReturn());
    p.SetExitMode(static_cast<uint8_t>(LockExit::Return));
    PCHECK_EQ(p.ExitMode(), static_cast<uint8_t>(LockExit::Return));

    float phys[4] = {kRest, kRest, kRest, kRest};
    pager.Update(phys, 0u);
    RecordGesture<Locks, 4>(locks, lock_btn, 1, {0.6f, 0.7f, 0.8f},
                            kExactFrameMs);
    PCHECK_NEAR(pager.Stored(0, 1), kRest, 1e-4f);
    PCHECK(!pager.State(0, 1).caught);

    p.SetExitMode(static_cast<uint8_t>(LockExit::Latch));
    PCHECK_EQ(p.ExitMode(), static_cast<uint8_t>(LockExit::Latch));
}

/* ── 13. Pure math: snap table and anim eval ───────────────────────── */

void TestLockSnapTicksTable()
{
    constexpr uint8_t kST  = LockGrid::Straight | LockGrid::Triplet;
    constexpr uint8_t kAll = kST | LockGrid::Dotted;

    /* 96 PPQN, 4/4: quarter 96, half 192, bar 384. */

    /* The motivating cases: near a half note snaps to the half note. */
    PCHECK_EQ(LockSnapTicks(195.84, 96, 4, kST), 192u);
    PCHECK_EQ(LockSnapTicks(188.20, 96, 4, kST), 192u);

    /* Near a triplet lands on the triplet (128 = half-note triplet). */
    PCHECK_EQ(LockSnapTicks(130.0, 96, 4, kST), 128u);

    /* 140 ticks: triplet 128 wins under the default grid… */
    PCHECK_EQ(LockSnapTicks(140.0, 96, 4, kST), 128u);
    /* …the dotted quarter (144) wins when dotted is enabled… */
    PCHECK_EQ(LockSnapTicks(140.0, 96, 4, kAll), 144u);
    /* …and straight-only has to reach to the half note (ratio beats
     * the quarter: 192/140 < 140/96). */
    PCHECK_EQ(LockSnapTicks(140.0, 96, 4, LockGrid::Straight), 192u);

    /* Above one bar, integer bar counts are always in the grid: a
     * 5.5-bar take goes to 6 bars, never stretched to 4 or 8. */
    PCHECK_EQ(LockSnapTicks(5.5 * 384.0, 96, 4, kST), 6u * 384u);
    PCHECK_EQ(LockSnapTicks(4.9 * 384.0, 96, 4, kST), 5u * 384u);

    /* Tiny blips snap up to the smallest enabled candidate. */
    PCHECK_EQ(LockSnapTicks(3.0, 96, 4, kST), 16u);          /* 16th-t  */
    PCHECK_EQ(LockSnapTicks(3.0, 96, 4, LockGrid::Straight), 24u);

    /* Degenerate inputs are refused, not guessed at. */
    PCHECK_EQ(LockSnapTicks(0.0, 96, 4, kST), 0u);
    PCHECK_EQ(LockSnapTicks(100.0, 0, 4, kST), 0u);
    PCHECK_EQ(LockSnapTicks(100.0, 96, 0, kST), 0u);

    /* Too long for the u16 saved form: refused, so the slot falls back
     * to free rather than trimming the take to a far-shorter grid. */
    PCHECK_EQ(LockSnapTicks(70000.0, 96, 4, kST), 0u);
    PCHECK_EQ(LockSnapTicks(65000.0, 96, 4, kST), 169u * 384u);

    /* 3/4 time: the bar is 288 ticks, and bar multiples track it. */
    PCHECK_EQ(LockSnapTicks(2.1 * 288.0, 96, 3, kST), 2u * 288u);

    /* PPQN-independent: nothing assumes 96 — the same musical take
     * scales with whatever resolution the clock declares. */
    PCHECK_EQ(LockSnapTicks(2.0 * 195.84, 192, 4, kST), 384u); /* half @192  */
    PCHECK_EQ(LockSnapTicks(48.9, 24, 4, kST), 48u);           /* half @24   */
    PCHECK_EQ(LockSnapTicks(2.1 * 72.0, 24, 3, kST), 2u * 72u);/* 2 bars 3/4 */
}

void TestLockAnimEval()
{
    /* Solid: always full, home pip. */
    PCHECK_NEAR(LockAnimEval(LockAnim::Solid, 123u, 0.7f, false).level,
                1.0f, 1e-6f);
    PCHECK(!LockAnimEval(LockAnim::Solid, 123u, 0.7f, false).sweep);

    /* Blink: 2 Hz square off absolute time. */
    PCHECK_NEAR(LockAnimEval(LockAnim::Blink, 100u, 0.f, false).level,
                1.0f, 1e-6f);
    PCHECK_NEAR(LockAnimEval(LockAnim::Blink, 300u, 0.f, false).level,
                0.0f, 1e-6f);
    PCHECK_NEAR(LockAnimEval(LockAnim::Blink, 600u, 0.f, false).level,
                1.0f, 1e-6f);

    /* Breathe: bounded, dim at the trough, full at the crest. */
    PCHECK_NEAR(LockAnimEval(LockAnim::Breathe, 0u, 0.f, false).level,
                0.25f, 1e-4f);
    PCHECK_NEAR(LockAnimEval(LockAnim::Breathe, 1000u, 0.f, false).level,
                1.0f, 1e-4f);

    /* LoopPulse: flash at the wrap, floor later; solid while recording. */
    PCHECK_NEAR(LockAnimEval(LockAnim::LoopPulse, 0u, 0.0f, false).level,
                1.0f, 1e-4f);
    PCHECK_NEAR(LockAnimEval(LockAnim::LoopPulse, 0u, 0.5f, false).level,
                0.2f, 1e-4f);
    PCHECK_NEAR(LockAnimEval(LockAnim::LoopPulse, 0u, 0.5f, true).level,
                1.0f, 1e-4f);

    /* Sweep: the pip rides the phase. */
    const LockAnimFrame s = LockAnimEval(LockAnim::Sweep, 0u, 0.3f, false);
    PCHECK(s.sweep);
    PCHECK_NEAR(s.sweep_pos01, 0.3f, 1e-6f);
    PCHECK_NEAR(s.level, 1.0f, 1e-6f);
}

/* ── 14. Settings::UseLocks binding ────────────────────────────────── */

struct FakeLockSource : LockSource
{
    uint8_t mode      = 0u;
    uint8_t exit      = 0u;
    bool    has_clock = true;

    float DeltaAtPage(uint8_t, uint8_t) const override { return 0.0f; }
    bool  IsRecordingAtPage(uint8_t, uint8_t) const override { return false; }
    bool  IsActiveAtPage(uint8_t, uint8_t) const override { return false; }
    void  Update(const float*, uint32_t) override {}

    void    SetSyncMode(uint8_t m) override { mode = m; }
    uint8_t SyncMode() const override { return mode; }
    bool    HasClock() const override { return has_clock; }

    void    SetExitMode(uint8_t m) override { exit = m; }
    uint8_t ExitMode() const override { return exit; }
};

void TestSettingsUseLocksBinding()
{
    FakeButton b2, b3;
    Settings settings(b2, b3);
    FakeLockSource lock;

    auto h = settings.UseLocks(lock);

    /* Two plain persisted Selectors at P5 (sync) and P6 (exit) — one
     * byte each, exactly like any selector, so they ride presets the
     * way brightness does.  P2 stays free for the module. */
    PCHECK(settings.KindAt(0, 1) == SettingsKind::None);
    PCHECK(settings.KindAt(0, 4) == SettingsKind::Selector);
    PCHECK(settings.KindAt(0, 5) == SettingsKind::Selector);
    PCHECK_EQ(settings.ZonesAt(0, 4), 2u);
    PCHECK_EQ(settings.ZonesAt(0, 5), 2u);
    PCHECK_EQ(settings.SerializedSize(), 2u);
    PCHECK_EQ(lock.mode, 0u);
    PCHECK(h.Value() == LockSync::Free);

    /* The exit selector defaults to Return (zone 0) and pushes it. */
    PCHECK_EQ(lock.exit, static_cast<uint8_t>(LockExit::Return));
    PCHECK(h.ExitValue() == LockExit::Return);
    PCHECK_EQ(settings.SelectorIdxAt(0, 5), 0u);

    /* Default() / DefaultExit() push immediately. */
    h.Default(LockSync::Clocked);
    PCHECK_EQ(lock.mode, 1u);
    PCHECK(h.Value() == LockSync::Clocked);
    h.DefaultExit(LockExit::Latch);
    PCHECK_EQ(lock.exit, static_cast<uint8_t>(LockExit::Latch));
    PCHECK(h.ExitValue() == LockExit::Latch);
    PCHECK_EQ(settings.SelectorIdxAt(0, 5), 1u);

    /* Serialize carries the zone bytes; Deserialize pushes into the
     * lock, so a preset load restores behavior with no menu visit. */
    uint8_t img[2] = {0xFFu, 0xFFu};
    settings.Serialize(img);
    PCHECK_EQ(img[0], 1u);
    PCHECK_EQ(img[1], 1u);

    lock.mode = 0u;
    img[0]    = 1u;   /* Clocked      */
    img[1]    = 0u;   /* zone 0 = Return */
    PCHECK(settings.Deserialize(img));
    PCHECK_EQ(lock.mode, 1u);
    PCHECK_EQ(lock.exit, static_cast<uint8_t>(LockExit::Return));
    PCHECK_EQ(settings.SelectorIdxAt(0, 4), 1u);
    PCHECK_EQ(settings.SelectorIdxAt(0, 5), 0u);

    /* An out-of-range byte clamps instead of poisoning the zone math. */
    img[0] = 9u;
    img[1] = 9u;
    PCHECK(settings.Deserialize(img));
    PCHECK_EQ(settings.SelectorIdxAt(0, 4), 1u);
    PCHECK_EQ(settings.SelectorIdxAt(0, 5), 1u);
    PCHECK_EQ(lock.exit, static_cast<uint8_t>(LockExit::Latch));

    /* At() / ExitAt() relocate the slots; the handle follows. */
    h.At(0, 1).ExitAt(0, 2);
    PCHECK(settings.KindAt(0, 4) == SettingsKind::None);
    PCHECK(settings.KindAt(0, 5) == SettingsKind::None);
    PCHECK(settings.KindAt(0, 1) == SettingsKind::Selector);
    PCHECK(settings.KindAt(0, 2) == SettingsKind::Selector);
    PCHECK_EQ(settings.SerializedSize(), 2u);
    h.Default(LockSync::Free);
    PCHECK_EQ(lock.mode, 0u);
    h.DefaultExit(LockExit::Return);
    PCHECK_EQ(lock.exit, static_cast<uint8_t>(LockExit::Return));
}

void TestBrightnessHandleAt()
{
    FakeButton b2, b3;
    Settings settings(b2, b3);

    /* The placement override UseBrightness's doc always promised:
     * relocate, then keep configuring through the moved handle. */
    auto h = settings.UseBrightness().At(1, 0).Default(0.5f);
    PCHECK(settings.KindAt(0, 0) == SettingsKind::None);
    PCHECK(settings.KindAt(1, 0) == SettingsKind::Brightness);
    PCHECK_EQ(settings.NumPages(), 2u);
    PCHECK_NEAR(h.Value(), 0.5f, 1e-5f);
    PCHECK_NEAR(settings.RangeLoAt(1, 0), 0.05f, 1e-6f);
    PCHECK_EQ(settings.SerializedSize(), sizeof(float));

    /* Out-of-range targets are refused; the handle stays put. */
    h.At(9, 0).Default(0.8f);
    PCHECK(settings.KindAt(1, 0) == SettingsKind::Brightness);
    PCHECK_NEAR(h.Value(), 0.8f, 1e-5f);

    /* Builder-returned handles know their owner too. */
    auto k = settings.Page(0).Pot(3).Brightness();
    k.At(2, 5);
    PCHECK(settings.KindAt(0, 3) == SettingsKind::None);
    PCHECK(settings.KindAt(2, 5) == SettingsKind::Brightness);
}

} // namespace

int RunParamLockTests(int& checks, int& failures)
{
    TestQuantization();
    TestConfigConstants();

    TestRecordPlaybackRoundTrip();
    TestDisarmClearsSlot();
    TestBufferFullAutoArms();

    TestWallClockIndependentOfFrameRate();
    TestPlaybackInterpolates();
    TestRecordingBoxAverages();
    TestSlowFrameHoldsLastSample();
    TestShortGestureStillRecords();

    TestSerializeRoundTrip();
    TestSerializeRejectsMalformed();
    TestAdvanceSplit();
    TestSchemaHashSeparatesConfigurations();
    TestRamOnlyStaysOutOfPresets();

    TestExternalArena();
    TestShortArenaIsRefused();
    TestPagedSlotAddressing();
    TestPresetBudget();

    TestArmAndClearThresholds();

    TestClockedLengthSnapsToGrid();
    TestClockedTrimsLongTakes();
    TestClockedHoldsShortTakes();
    TestClockedSnapSurvivesFrameTimingLie();
    TestClockedTracksTempoChange();
    TestClockedFollowsTransportStop();
    TestClockResetReAnchors();
    TestClockedFallsBackWithoutRunningClock();
    TestModeAffectsNewRecordingsOnly();
    TestRestoredClockedLocksShareAlignment();

    TestFreezeHoldsPlayheads();
    TestRecordingFinishedWhileFrozen();
    TestClearSlotIsIsolated();
    TestPlayPhaseReportsHeadAndWriteFill();

    TestLatchExitLeavesBaseAlone();
    TestReturnExitRestoresOriginAndRearmsCatch();
    TestExitModeLockSourceBinding();

    TestLockSnapTicksTable();
    TestLockAnimEval();

    TestSettingsUseLocksBinding();
    TestBrightnessHandleAt();

    checks   += s_checks;
    failures += s_failures;
    return s_failures;
}
