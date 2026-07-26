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

#include "alchemy/surface/pager.h"
#include "alchemy/surface/param_lock.h"
#include "alchemy/surface/presets.h"

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
 * nothing.  So the lock arms on values[0]: record_base == values[0], and
 * the recorded samples are `values`.
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
              float record_base, const std::vector<float>& samples)
{
    uint8_t* p = image.data() + static_cast<size_t>(slot) * Locks::kBytesPerSavedSlot;

    ParamLockSavedHeader h;
    h.active      = active ? 1u : 0u;
    h.length      = static_cast<uint16_t>(samples.size());
    h.record_base = LockQuant(record_base);
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

    /* 5-byte header + 2 B per sample, per slot. */
    PCHECK_EQ(Long::kBytesPerSavedSlot, 5u + 600u * 2u);
    PCHECK_EQ(Long::kSavedBytes, 6u * (5u + 1200u));
    PCHECK_EQ(Long::kPresetBytes, Long::kSavedBytes);

    /* Arena is 2 B per sample and nothing else. */
    PCHECK_EQ(Long::kArenaSamples, 6u * 600u);
    PCHECK_EQ(Long::kArenaBytes, 6u * 600u * 2u);

    /* The headline configurations fit a preset slot. */
    PCHECK(Long::kPresetBytes  <= kPresetBlobCapacity);
    PCHECK(Paged::kPresetBytes <= kPresetBlobCapacity);
    PCHECK(Paged::kPresetBytes  == 12u * (5u + 1200u));

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

    /* First swept value arms the lock, so it becomes record_base. */
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
            PCHECK_NEAR(locks.Delta(1), v - sweep.front(), 1e-3f);
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
    /* Recorded deltas span 0 .. 0.398; 0.2 is an interior level. */
    PCHECK_EQ(FramesPerLoop(locks, 0, kExactFrameMs, 0.2f),
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
     * frames sharing its period, landing inside the raw -0.4 excursion. */
    const float first = locks.Delta(0);
    PCHECK(first > -0.4f);
    PCHECK(first < 0.0f);
    PCHECK_NEAR(first, (0.9f + 4.0f * 0.5f) / 5.0f - 0.9f, 3e-2f);
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
    PCHECK_EQ(h.record_base, LockQuant(0.6f));
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

    checks   += s_checks;
    failures += s_failures;
    return s_failures;
}
