/**
 * @file tests/host/midi_tests.cpp
 * @brief Descriptor golden + codec / tracker / UsbMidi service tests
 *        (service runs against the fake alchemy_usb layer below).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "alchemy/midi/midi_event.h"
#include "alchemy/midi/mono_note_tracker.h"
#include "alchemy/midi/poly_note_allocator.h"
#include "alchemy/midi/usb_midi.h"
#include "alchemy/usb/usb_descriptors.h"

using namespace alchemy::midi;

static int g_checks   = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        g_checks++;                                                        \
        const auto va = (a);                                               \
        const auto vb = (b);                                               \
        if (!(va == vb)) {                                                 \
            g_failures++;                                                  \
            std::printf("FAIL %s:%d  %s == %s  (%llu != %llu)\n",          \
                        __FILE__, __LINE__, #a, #b,                        \
                        (unsigned long long)va, (unsigned long long)vb);   \
        }                                                                  \
    } while (0)

/* ── Fake alchemy_usb layer ────────────────────────────────────────── */

static AlchemyUsbMidiRxFn fake_rx_fn  = nullptr;
static void*              fake_rx_ctx = nullptr;
static bool               fake_configured = true;
static bool               fake_tx_ready   = true;
static std::vector<std::vector<uint8_t>> fake_tx_bursts;

extern "C" {
void alchemy_usb_start(AlchemyUsbPeriph periph, const char* product_name)
{
    (void)periph;
    (void)product_name;
}
bool alchemy_usb_configured(void) { return fake_configured; }
void alchemy_usb_midi_set_rx(AlchemyUsbMidiRxFn fn, void* ctx)
{
    fake_rx_fn  = fn;
    fake_rx_ctx = ctx;
}
bool alchemy_usb_midi_tx_ready(void) { return fake_configured && fake_tx_ready; }
uint16_t alchemy_usb_midi_write(const uint8_t* packets, uint16_t len)
{
    if (!fake_configured || !fake_tx_ready)
        return 0u;
    fake_tx_bursts.emplace_back(packets, packets + len);
    return len;
}
}

static void Inject(const uint8_t* pkt) { fake_rx_fn(pkt, fake_rx_ctx); }
static void Inject4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t p[4] = {a, b, c, d};
    Inject(p);
}

/* ── Descriptor tests ──────────────────────────────────────────────── */

static uint32_t Fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Byte-exact pins — changing them is a host-visible USB shape change:
 * bump ALCHEMY_USB_BCD_DEVICE alongside. */
static constexpr uint32_t kDevDescPin = 0xF314E77Du;
static constexpr uint32_t kCfgDescPin = 0xC511C565u;

static void TestDescriptors()
{
    CHECK_EQ(alchemy_usb_dev_desc[0], 18u);
    CHECK_EQ(alchemy_usb_dev_desc[1], 0x01u);
    CHECK_EQ(alchemy_usb_dev_desc[4], 0xEFu);
    CHECK_EQ(alchemy_usb_dev_desc[5], 0x02u);
    CHECK_EQ(alchemy_usb_dev_desc[6], 0x01u);
    const uint16_t bcd = static_cast<uint16_t>(alchemy_usb_dev_desc[12]
                                               | (alchemy_usb_dev_desc[13]
                                                  << 8));
    CHECK_EQ(bcd, ALCHEMY_USB_BCD_DEVICE);

    const uint8_t* d = alchemy_usb_cfg_desc;
    const uint16_t total = static_cast<uint16_t>(d[2] | (d[3] << 8));
    CHECK_EQ(total, ALCHEMY_USB_CFG_DESC_SIZE);
    CHECK_EQ(d[4], ALCHEMY_USB_NUM_INTERFACES);
    CHECK_EQ(d[7], 0xC0u); /* self-powered */

    /* Walk: every bLength consumes exactly the blob. */
    bool     saw_if[4]  = {};
    uint8_t  if_class[4] = {};
    int      num_iads   = 0;
    uint8_t  iad_cover  = 0u;
    std::vector<uint8_t> eps;
    uint16_t ms_cs_off = 0u, ms_cs_total = 0u;

    uint16_t off = d[0];
    while (off < total)
    {
        const uint8_t len  = d[off];
        const uint8_t type = d[off + 1u];
        CHECK(len >= 2u);
        CHECK(off + len <= total);

        if (type == 0x0Bu)
        {
            num_iads++;
            for (uint8_t i = 0; i < d[off + 3u]; i++)
                iad_cover |= static_cast<uint8_t>(1u << (d[off + 2u] + i));
        }
        else if (type == 0x04u)
        {
            const uint8_t num = d[off + 2u];
            if (num < 4u)
            {
                saw_if[num]   = true;
                if_class[num] = d[off + 5u];
            }
        }
        else if (type == 0x05u)
        {
            eps.push_back(d[off + 2u]);
        }
        else if (type == 0x24u && d[off + 2u] == 0x01u && len == 7u)
        {
            ms_cs_off   = off;
            ms_cs_total = static_cast<uint16_t>(d[off + 5u]
                                                | (d[off + 6u] << 8));
        }
        off = static_cast<uint16_t>(off + len);
    }
    CHECK_EQ(off, total);

    CHECK(saw_if[0] && saw_if[1] && saw_if[2] && saw_if[3]);
    CHECK_EQ(if_class[0], 0x02u);
    CHECK_EQ(if_class[1], 0x0Au);
    CHECK_EQ(if_class[2], 0x01u);
    CHECK_EQ(if_class[3], 0x01u);

    CHECK_EQ(num_iads, 2);
    CHECK_EQ(iad_cover, 0x0Fu);

    CHECK_EQ(eps.size(), 5u);
    const uint8_t want_eps[5] = {ALCHEMY_CDC_CMD_EP, ALCHEMY_CDC_OUT_EP,
                                 ALCHEMY_CDC_IN_EP, ALCHEMY_MIDI_OUT_EP,
                                 ALCHEMY_MIDI_IN_EP};
    for (size_t i = 0; i < eps.size() && i < 5u; i++)
        CHECK_EQ(eps[i], want_eps[i]);

    /* MS class-specific total spans from its header to the blob end. */
    CHECK(ms_cs_off != 0u);
    CHECK_EQ(ms_cs_total, static_cast<uint16_t>(total - ms_cs_off));

    const uint32_t dev_h = Fnv1a(alchemy_usb_dev_desc,
                                 ALCHEMY_USB_DEV_DESC_SIZE);
    const uint32_t cfg_h = Fnv1a(alchemy_usb_cfg_desc,
                                 ALCHEMY_USB_CFG_DESC_SIZE);
    if (dev_h != kDevDescPin || cfg_h != kCfgDescPin)
    {
        std::printf("descriptor pins: dev 0x%08X cfg 0x%08X\n",
                    (unsigned)dev_h, (unsigned)cfg_h);
    }
    CHECK_EQ(dev_h, kDevDescPin);
    CHECK_EQ(cfg_h, kCfgDescPin);
}

/* ── Codec tests ───────────────────────────────────────────────────── */

static void TestCodec()
{
    MidiEvent ev;

    uint8_t pkt[4];
    const MidiEvent note_on = MidiEvent::Make(MidiMsg::NoteOn, 4u, 60u, 100u);
    CHECK(EventToUsbPacket(note_on, 0u, pkt));
    CHECK_EQ(pkt[0], 0x09u);
    CHECK_EQ(pkt[1], 0x94u);
    CHECK(EventFromUsbPacket(pkt, ev));
    CHECK_EQ(ev.status, 0x94u);
    CHECK_EQ(ev.d1, 60u);
    CHECK_EQ(ev.d2, 100u);
    CHECK(ev.IsChannelVoice());
    CHECK_EQ(ev.Channel(), 4u);
    CHECK(ev.Type() == MidiMsg::NoteOn);

    const MidiEvent prog = MidiEvent::Make(MidiMsg::ProgramChange, 2u, 7u);
    CHECK(EventToUsbPacket(prog, 0u, pkt));
    CHECK_EQ(pkt[0], 0x0Cu);
    CHECK(EventFromUsbPacket(pkt, ev));
    CHECK_EQ(ev.status, 0xC2u);
    CHECK_EQ(ev.d1, 7u);

    MidiEvent clk;
    clk.status = 0xF8u;
    CHECK(EventToUsbPacket(clk, 0u, pkt));
    CHECK_EQ(pkt[0], 0x0Fu);
    CHECK(EventFromUsbPacket(pkt, ev));
    CHECK_EQ(ev.status, 0xF8u);

    MidiEvent spp;
    spp.status = 0xF2u;
    spp.d1     = 0x10u;
    spp.d2     = 0x02u;
    CHECK(EventToUsbPacket(spp, 0u, pkt));
    CHECK_EQ(pkt[0], 0x03u);
    CHECK(EventFromUsbPacket(pkt, ev));
    CHECK_EQ(ev.status, 0xF2u);
    CHECK_EQ(ev.d1, 0x10u);
    CHECK_EQ(ev.d2, 0x02u);

    /* Bend helper */
    MidiEvent bend = MidiEvent::Make(MidiMsg::PitchBend, 0u, 0x00u, 0x40u);
    CHECK_EQ(bend.Bend14(), 0x2000u);

    /* SysEx spans decode to nothing (v1 policy: tolerated, discarded) */
    const uint8_t syx_start[4] = {0x04u, 0xF0u, 0x01u, 0x02u};
    const uint8_t syx_end[4]   = {0x07u, 0x03u, 0x04u, 0xF7u};
    CHECK(!EventFromUsbPacket(syx_start, ev));
    CHECK(!EventFromUsbPacket(syx_end, ev));

    /* CIN 5 is tune request only when the byte says so */
    const uint8_t tune[4]    = {0x05u, 0xF6u, 0u, 0u};
    const uint8_t syx1[4]    = {0x05u, 0xF7u, 0u, 0u};
    CHECK(EventFromUsbPacket(tune, ev));
    CHECK_EQ(ev.status, 0xF6u);
    CHECK(!EventFromUsbPacket(syx1, ev));

    /* Reserved CINs */
    const uint8_t resv[4] = {0x00u, 0x90u, 60u, 100u};
    CHECK(!EventFromUsbPacket(resv, ev));
}

/* ── Tracker tests ─────────────────────────────────────────────────── */

static void TestTracker()
{
    MonoNoteTracker t;

    CHECK(!t.GateHigh());
    t.NoteOn(60u, 100u);
    CHECK(t.GateHigh());
    CHECK_EQ(t.Note(), 60u);
    CHECK_EQ(t.Velocity(), 100u);
    CHECK(t.TakeRetrigger());
    CHECK(!t.TakeRetrigger());

    /* Last priority: new note wins, release returns legato */
    t.NoteOn(64u, 90u);
    CHECK_EQ(t.Note(), 64u);
    CHECK(t.TakeRetrigger());
    t.NoteOff(64u);
    CHECK(t.GateHigh());
    CHECK_EQ(t.Note(), 60u);
    CHECK_EQ(t.Velocity(), 100u);
    CHECK(!t.TakeRetrigger());
    t.NoteOff(60u);
    CHECK(!t.GateHigh());
    CHECK_EQ(t.Note(), 60u); /* pitch holds through release tail */

    /* Low priority: higher press doesn't steal, lower does */
    MonoNoteTracker low;
    low.SetPriority(MonoNoteTracker::Priority::Low);
    low.NoteOn(48u, 80u);
    (void)low.TakeRetrigger();
    low.NoteOn(72u, 90u);
    CHECK_EQ(low.Note(), 48u);
    CHECK(!low.TakeRetrigger());
    low.NoteOn(36u, 70u);
    CHECK_EQ(low.Note(), 36u);
    CHECK(low.TakeRetrigger());
    low.NoteOff(36u);
    CHECK_EQ(low.Note(), 48u);

    /* High priority */
    MonoNoteTracker high;
    high.SetPriority(MonoNoteTracker::Priority::High);
    high.NoteOn(60u, 80u);
    high.NoteOn(72u, 90u);
    CHECK_EQ(high.Note(), 72u);
    high.NoteOn(65u, 85u);
    CHECK_EQ(high.Note(), 72u);
    high.NoteOff(72u);
    CHECK_EQ(high.Note(), 65u);

    /* Retrig on return, opt-in */
    MonoNoteTracker rt;
    rt.RetrigOnReturn(true);
    rt.NoteOn(60u, 100u);
    (void)rt.TakeRetrigger();
    rt.NoteOn(64u, 100u);
    (void)rt.TakeRetrigger();
    rt.NoteOff(64u);
    CHECK(rt.TakeRetrigger());

    /* Same-note restrike retrigs */
    MonoNoteTracker rs;
    rs.NoteOn(60u, 100u);
    (void)rs.TakeRetrigger();
    rs.NoteOn(60u, 110u);
    CHECK(rs.TakeRetrigger());
    CHECK_EQ(rs.Velocity(), 110u);
    CHECK_EQ(rs.HeldCount(), 1u);

    /* Overflow drops oldest */
    MonoNoteTracker of;
    for (uint8_t i = 0u; i < MonoNoteTracker::kCapacity + 4u; i++)
        of.NoteOn(static_cast<uint8_t>(20u + i), 100u);
    CHECK_EQ(of.HeldCount(), MonoNoteTracker::kCapacity);
    of.Clear();
    CHECK(!of.GateHigh());
}

/* ── Service tests ─────────────────────────────────────────────────── */

static void TestService()
{
    static UsbMidi midi;
    static MonoNoteTracker tracker;
    static std::vector<MidiEvent> seen;
    static uint32_t clock_pulses = 0u;

    seen.clear();
    fake_tx_bursts.clear();
    fake_configured = true;
    fake_tx_ready   = true;

    midi.Init();
    CHECK(fake_rx_fn != nullptr);

    midi.UseTracker(tracker)
        .SetChannel(4u)
        .OnEvent([](const MidiEvent& ev, void*) { seen.push_back(ev); },
                 nullptr)
        .OnClockPulse([](uint32_t, void*) { clock_pulses++; }, nullptr);

    /* Channel filter */
    Inject4(0x09u, 0x95u, 60u, 100u); /* ch 5: dropped */
    Inject4(0x09u, 0x94u, 60u, 100u); /* ch 4: kept    */
    midi.Poll(0u);
    CHECK_EQ(seen.size(), 1u);
    CHECK(tracker.GateHigh());
    CHECK_EQ(tracker.Note(), 60u);

    /* NoteOn vel 0 → NoteOff */
    Inject4(0x09u, 0x94u, 60u, 0u);
    midi.Poll(1u);
    CHECK_EQ(seen.size(), 2u);
    CHECK(seen.back().Type() == MidiMsg::NoteOff);
    CHECK(!tracker.GateHigh());

    /* CC 123 clears */
    Inject4(0x09u, 0x94u, 62u, 100u);
    midi.Poll(2u);
    CHECK(tracker.GateHigh());
    Inject4(0x0Bu, 0xB4u, 123u, 0u);
    midi.Poll(3u);
    CHECK(!tracker.GateHigh());

    /* Bend cache + helper */
    Inject4(0x0Eu, 0xE4u, 0x00u, 0x60u);
    midi.Poll(4u);
    CHECK_EQ(midi.PitchBend14(), 0x3000u);
    CHECK(midi.BendSemis(2.0f) > 0.99f && midi.BendSemis(2.0f) < 1.01f);

    /* Clock: IRQ path only, never queued */
    const uint32_t drops_before = midi.RxDropped();
    for (int i = 0; i < 1000; i++)
        Inject4(0x0Fu, 0xF8u, 0u, 0u);
    CHECK_EQ(clock_pulses, 1000u);
    CHECK_EQ(midi.ClockCount(), 1000u);
    CHECK_EQ(midi.RxDropped(), drops_before);
    midi.Poll(5u);
    const size_t seen_after_clock = seen.size();
    CHECK_EQ(seen_after_clock, 5u); /* no clock events dispatched */

    /* Start/Stop do queue */
    Inject4(0x0Fu, 0xFAu, 0u, 0u);
    Inject4(0x0Fu, 0xFCu, 0u, 0u);
    midi.Poll(6u);
    CHECK_EQ(seen.size(), 7u);
    CHECK(seen[5].Type() == MidiMsg::Start);
    CHECK(seen[6].Type() == MidiMsg::Stop);

    /* Active sensing ignored */
    Inject4(0x0Fu, 0xFEu, 0u, 0u);
    midi.Poll(7u);
    CHECK_EQ(seen.size(), 7u);

    /* Disconnect drops the gate */
    Inject4(0x09u, 0x94u, 65u, 100u);
    midi.Poll(8u);
    CHECK(tracker.GateHigh());
    fake_configured = false;
    midi.Poll(9u);
    CHECK(!tracker.GateHigh());
    CHECK_EQ(midi.PitchBend14(), 0x2000u);
    fake_configured = true;

    /* TX: three events → one 12-byte burst */
    midi.SendNoteOn(0u, 60u, 100u);
    midi.SendControlChange(0u, 1u, 64u);
    midi.SendNoteOff(0u, 60u);
    midi.Poll(10u);
    CHECK_EQ(fake_tx_bursts.size(), 1u);
    CHECK_EQ(fake_tx_bursts[0].size(), 12u);
    CHECK_EQ(fake_tx_bursts[0][0], 0x09u);
    CHECK_EQ(fake_tx_bursts[0][4], 0x0Bu);
    CHECK_EQ(fake_tx_bursts[0][8], 0x08u);

    /* TX busy: queued until ready */
    fake_tx_ready = false;
    midi.SendNoteOn(0u, 61u, 100u);
    midi.Poll(11u);
    CHECK_EQ(fake_tx_bursts.size(), 1u);
    fake_tx_ready = true;
    midi.Poll(12u);
    CHECK_EQ(fake_tx_bursts.size(), 2u);
    CHECK_EQ(fake_tx_bursts[1].size(), 4u);

    /* RX ring overflow is counted, then drains */
    for (int i = 0; i < 400; i++)
        Inject4(0x0Bu, 0xB4u, 1u, static_cast<uint8_t>(i & 0x7Fu));
    CHECK(midi.RxDropped() > 0u);
    midi.Poll(13u);
    CHECK(seen.size() > seen_after_clock);
}

/* ── Mono sustain ──────────────────────────────────────────────────── */

static void TestMonoSustain()
{
    MonoNoteTracker t;

    t.NoteOn(60u, 100u);
    t.SetSustain(true);
    t.NoteOff(60u);
    CHECK(t.GateHigh());
    CHECK_EQ(t.Note(), 60u);
    t.SetSustain(false);
    CHECK(!t.GateHigh());

    /* Re-press of a sustained note retriggers. */
    t.NoteOn(60u, 100u);
    (void)t.TakeRetrigger();
    t.SetSustain(true);
    t.NoteOff(60u);
    t.NoteOn(60u, 90u);
    CHECK(t.TakeRetrigger());
    CHECK_EQ(t.Velocity(), 90u);
    t.NoteOff(60u);
    CHECK(t.GateHigh()); /* sustained again */
    t.SetSustain(false);
    CHECK(!t.GateHigh());

    /* Held note survives pedal release; sustained ones drop. */
    t.NoteOn(48u, 80u);
    t.SetSustain(true);
    t.NoteOn(52u, 80u);
    t.NoteOff(52u);
    t.SetSustain(false);
    CHECK(t.GateHigh());
    CHECK_EQ(t.Note(), 48u);
    CHECK_EQ(t.HeldCount(), 1u);
    t.Clear();
}

/* ── Poly allocator ────────────────────────────────────────────────── */

static void TestPolyAllocator()
{
    PolyNoteAllocator a;
    a.SetVoiceCount(4u);

    a.NoteOn(0u, 60u, 100u);
    a.NoteOn(0u, 64u, 90u);
    a.NoteOn(0u, 67u, 80u);
    a.NoteOn(0u, 71u, 70u);
    CHECK_EQ(a.GatedCount(), 4u);
    bool notes_seen[4] = {};
    for (uint8_t i = 0u; i < 4u; i++)
    {
        CHECK(a.TakeRetrigger(i));
        switch (a.VoiceAt(i).note)
        {
            case 60u: notes_seen[0] = true; break;
            case 64u: notes_seen[1] = true; break;
            case 67u: notes_seen[2] = true; break;
            case 71u: notes_seen[3] = true; break;
        }
    }
    CHECK(notes_seen[0] && notes_seen[1] && notes_seen[2] && notes_seen[3]);

    /* Oldest steal (default): 5th note replaces note 60. */
    a.NoteOn(0u, 74u, 100u);
    CHECK_EQ(a.GatedCount(), 4u);
    bool has60 = false, has74 = false;
    for (uint8_t i = 0u; i < 4u; i++)
    {
        if (a.VoiceAt(i).note == 60u && a.VoiceAt(i).gate) has60 = true;
        if (a.VoiceAt(i).note == 74u && a.VoiceAt(i).gate) has74 = true;
    }
    CHECK(!has60);
    CHECK(has74);

    /* Lowest steal. */
    PolyNoteAllocator lo;
    lo.SetVoiceCount(2u).SetSteal(PolyNoteAllocator::Steal::Lowest);
    lo.NoteOn(0u, 40u, 100u);
    lo.NoteOn(0u, 50u, 100u);
    lo.NoteOn(0u, 60u, 100u);
    bool has40 = false;
    for (uint8_t i = 0u; i < 2u; i++)
        if (lo.VoiceAt(i).note == 40u && lo.VoiceAt(i).gate) has40 = true;
    CHECK(!has40);

    /* None: new note drops when full. */
    PolyNoteAllocator none;
    none.SetVoiceCount(2u).SetSteal(PolyNoteAllocator::Steal::None);
    none.NoteOn(0u, 40u, 100u);
    none.NoteOn(0u, 50u, 100u);
    none.NoteOn(0u, 60u, 100u);
    bool has60n = false;
    for (uint8_t i = 0u; i < 2u; i++)
        if (none.VoiceAt(i).note == 60u) has60n = true;
    CHECK(!has60n);

    /* Note-off frees; freed voice is reused least-recently-released
     * first. */
    PolyNoteAllocator f;
    f.SetVoiceCount(4u);
    f.NoteOn(0u, 60u, 100u);
    f.NoteOn(0u, 64u, 100u);
    f.NoteOff(0u, 60u);
    f.NoteOff(0u, 64u);
    f.NoteOn(0u, 72u, 100u);
    bool v72_in_60_slot = false;
    for (uint8_t i = 0u; i < 4u; i++)
        if (f.VoiceAt(i).note == 72u) v72_in_60_slot = true;
    CHECK(v72_in_60_slot);
    CHECK_EQ(f.GatedCount(), 1u);

    /* Same-note re-press reuses the voice and retriggers. */
    PolyNoteAllocator s;
    s.SetVoiceCount(4u);
    s.NoteOn(0u, 60u, 100u);
    for (uint8_t i = 0u; i < 4u; i++) (void)s.TakeRetrigger(i);
    s.NoteOn(0u, 60u, 90u);
    CHECK_EQ(s.GatedCount(), 1u);
    uint8_t v = 0xFFu;
    for (uint8_t i = 0u; i < 4u; i++)
        if (s.VoiceAt(i).gate && s.VoiceAt(i).note == 60u) v = i;
    CHECK(v != 0xFFu);
    CHECK(s.TakeRetrigger(v));
    CHECK_EQ(s.VoiceAt(v).velocity, 90u);

    /* Sustain: offs deferred, pedal-up releases; sustained re-press
     * reclaims. */
    PolyNoteAllocator su;
    su.SetVoiceCount(4u);
    su.SetSustain(true);
    su.NoteOn(0u, 60u, 100u);
    su.NoteOn(0u, 64u, 100u);
    su.NoteOff(0u, 60u);
    su.NoteOff(0u, 64u);
    CHECK_EQ(su.GatedCount(), 2u);
    su.NoteOn(0u, 60u, 80u);
    CHECK_EQ(su.GatedCount(), 2u);
    su.SetSustain(false);
    CHECK_EQ(su.GatedCount(), 1u); /* re-pressed 60 survives */
    su.Clear();
    CHECK_EQ(su.GatedCount(), 0u);

    /* Notes keyed per channel (MPE member channels). */
    PolyNoteAllocator m;
    m.SetVoiceCount(4u);
    m.NoteOn(1u, 60u, 100u);
    m.NoteOn(2u, 60u, 100u);
    CHECK_EQ(m.GatedCount(), 2u);
    m.NoteOff(1u, 60u);
    CHECK_EQ(m.GatedCount(), 1u);
}

/* ── Service: sustain, expression, RPN, SysEx, TX ──────────────────── */

static void TestServiceComplete()
{
    static UsbMidi midi;
    static MonoNoteTracker tracker;
    static PolyNoteAllocator alloc;
    static std::vector<uint8_t> syx;
    static bool syx_done = false;
    static uint16_t last_rpn_param = 0xFFFFu;
    static uint16_t last_rpn_val = 0u;
    static bool last_rpn_nrpn = false;

    syx.clear();
    fake_tx_bursts.clear();
    fake_configured = true;
    fake_tx_ready   = true;

    midi.Init();
    alloc.SetVoiceCount(4u);
    midi.UseTracker(tracker).UseAllocator(alloc).SetOmni();
    midi.OnSysEx(
        [](const uint8_t* d, uint8_t n, bool term, void*) {
            syx.insert(syx.end(), d, d + n);
            if (term) syx_done = true;
        },
        nullptr);
    midi.OnRpn(
        [](uint8_t, uint16_t param, uint16_t val, bool nrpn, void*) {
            last_rpn_param = param;
            last_rpn_val   = val;
            last_rpn_nrpn  = nrpn;
        },
        nullptr);

    /* CC64 routes to both consumers. */
    Inject4(0x09u, 0x90u, 60u, 100u);
    Inject4(0x0Bu, 0xB0u, 64u, 127u);
    Inject4(0x08u, 0x80u, 60u, 0u);
    midi.Poll(0u);
    CHECK(tracker.GateHigh());
    CHECK_EQ(alloc.GatedCount(), 1u);
    Inject4(0x0Bu, 0xB0u, 64u, 0u);
    midi.Poll(1u);
    CHECK(!tracker.GateHigh());
    CHECK_EQ(alloc.GatedCount(), 0u);

    /* Per-channel expression caches. */
    Inject4(0x0Eu, 0xE2u, 0x00u, 0x60u); /* ch2 bend up   */
    Inject4(0x0Du, 0xD3u, 55u, 0u);      /* ch3 pressure  */
    Inject4(0x0Bu, 0xB4u, 74u, 99u);     /* ch4 cc74      */
    midi.Poll(2u);
    CHECK_EQ(midi.PitchBend14(2u), 0x3000u);
    CHECK_EQ(midi.PitchBend14(0u), 0x2000u);
    CHECK_EQ(midi.ChannelPressure(3u), 55u);
    CHECK_EQ(midi.Cc74(4u), 99u);

    /* RPN 0 sets bend range; setting reasserts only on change. */
    alchemy::SettingsSlot bend_slot;
    bend_slot.kind      = alchemy::SettingsKind::Selector;
    bend_slot.num_zones = UsbMidi::kBendRangeZones;
    bend_slot.value_idx = 2u; /* ±2 */
    alchemy::SelectorHandle bend(&bend_slot);
    midi.UseBendRangeSetting(bend);
    midi.Poll(3u);
    CHECK(midi.BendRangeSemis() == 2.0f);

    Inject4(0x0Bu, 0xB0u, 101u, 0u);
    Inject4(0x0Bu, 0xB0u, 100u, 0u);
    Inject4(0x0Bu, 0xB0u, 6u, 12u);
    midi.Poll(4u);
    CHECK(midi.BendRangeSemis() == 12.0f);
    CHECK_EQ(last_rpn_param, 0u);
    CHECK(!last_rpn_nrpn);
    midi.Poll(5u); /* zone unchanged: RPN value holds */
    CHECK(midi.BendRangeSemis() == 12.0f);
    Inject4(0x0Bu, 0xB0u, 38u, 50u);
    midi.Poll(6u);
    CHECK(midi.BendRangeSemis() > 12.49f && midi.BendRangeSemis() < 12.51f);
    bend_slot.value_idx = 1u; /* user moves the setting: ±1 wins */
    midi.Poll(7u);
    CHECK(midi.BendRangeSemis() == 1.0f);

    /* NRPN reported as such, no bend-range effect. */
    Inject4(0x0Bu, 0xB0u, 99u, 1u);
    Inject4(0x0Bu, 0xB0u, 98u, 2u);
    Inject4(0x0Bu, 0xB0u, 6u, 7u);
    midi.Poll(8u);
    CHECK(last_rpn_nrpn);
    CHECK_EQ(last_rpn_param, static_cast<uint16_t>((1u << 7) | 2u));
    CHECK(midi.BendRangeSemis() == 1.0f);

    /* Null RPN disarms data entry. */
    Inject4(0x0Bu, 0xB0u, 101u, 127u);
    Inject4(0x0Bu, 0xB0u, 100u, 127u);
    last_rpn_param = 0xFFFFu;
    Inject4(0x0Bu, 0xB0u, 6u, 3u);
    midi.Poll(9u);
    CHECK_EQ(last_rpn_param, 0xFFFFu);

    /* SysEx RX: chunks delivered raw, stream aligned after. */
    Inject4(0x04u, 0xF0u, 0x7Du, 0x01u);
    Inject4(0x07u, 0x02u, 0x03u, 0xF7u);
    midi.Poll(10u);
    CHECK(syx_done);
    CHECK_EQ(syx.size(), 6u);
    CHECK_EQ(syx[0], 0xF0u);
    CHECK_EQ(syx[5], 0xF7u);
    Inject4(0x09u, 0x90u, 62u, 100u);
    midi.Poll(11u);
    CHECK(tracker.GateHigh());
    Inject4(0x08u, 0x80u, 62u, 0u);
    midi.Poll(12u);

    /* TX senders produce exact packets. */
    fake_tx_bursts.clear();
    midi.SendPitchBend(0u, 0x2000u);
    midi.SendProgramChange(0u, 7u);
    midi.SendRealtime(MidiMsg::Clock);
    midi.Poll(13u);
    CHECK_EQ(fake_tx_bursts.size(), 1u);
    CHECK_EQ(fake_tx_bursts[0].size(), 12u);
    CHECK_EQ(fake_tx_bursts[0][0], 0x0Eu);
    CHECK_EQ(fake_tx_bursts[0][2], 0x00u);
    CHECK_EQ(fake_tx_bursts[0][3], 0x40u);
    CHECK_EQ(fake_tx_bursts[0][4], 0x0Cu);
    CHECK_EQ(fake_tx_bursts[0][8], 0x0Fu);
    CHECK_EQ(fake_tx_bursts[0][9], 0xF8u);

    /* SysEx TX packetization: F0 01 02 03 04 F7 → 0x04 + 0x07. */
    fake_tx_bursts.clear();
    const uint8_t payload[4] = {0x01u, 0x02u, 0x03u, 0x04u};
    CHECK(midi.SendSysEx(payload, 4u));
    midi.Poll(14u);
    CHECK_EQ(fake_tx_bursts.size(), 1u);
    CHECK_EQ(fake_tx_bursts[0].size(), 8u);
    CHECK_EQ(fake_tx_bursts[0][0], 0x04u);
    CHECK_EQ(fake_tx_bursts[0][1], 0xF0u);
    CHECK_EQ(fake_tx_bursts[0][4], 0x07u);
    CHECK_EQ(fake_tx_bursts[0][7], 0xF7u);
}

/* ── Settings-recipe binding tests ─────────────────────────────────── */

static void TestSettingsBindings()
{
    static UsbMidi midi;
    static MonoNoteTracker tracker;
    static std::vector<MidiEvent> seen;
    seen.clear();
    fake_configured = true;
    fake_tx_ready   = true;

    midi.Init();
    midi.UseTracker(tracker).OnEvent(
        [](const MidiEvent& ev, void*) { seen.push_back(ev); }, nullptr);

    alchemy::SettingsSlot ch_slot;
    ch_slot.kind      = alchemy::SettingsKind::Selector;
    ch_slot.num_zones = UsbMidi::kChannelZones;
    ch_slot.value_idx = 5u; /* channel 5 → filter ch index 4 */
    alchemy::SelectorHandle ch(&ch_slot);
    midi.UseChannelSetting(ch);

    CHECK(ch_slot.name != nullptr
          && std::strcmp(ch_slot.name, "MIDI Channel") == 0);
    CHECK(ch_slot.zone_labels == UsbMidi::kChannelLabels);

    Inject4(0x09u, 0x90u, 60u, 100u); /* ch 1: filtered out */
    Inject4(0x09u, 0x94u, 60u, 100u); /* ch 5: passes       */
    midi.Poll(0u);
    CHECK_EQ(seen.size(), 1u);
    CHECK_EQ(midi.Channel(), 4u);

    ch_slot.value_idx = 0u; /* Omni: everything passes */
    Inject4(0x09u, 0x91u, 62u, 100u);
    midi.Poll(1u);
    CHECK_EQ(seen.size(), 2u);
    CHECK_EQ(midi.Channel(), UsbMidi::kOmni);

    alchemy::SettingsSlot bend_slot;
    bend_slot.kind      = alchemy::SettingsKind::Selector;
    bend_slot.num_zones = UsbMidi::kBendRangeZones;
    bend_slot.value_idx = 4u; /* ±12 st */
    alchemy::SelectorHandle bend(&bend_slot);
    midi.UseBendRangeSetting(bend);
    CHECK(bend_slot.zone_labels == UsbMidi::kBendRangeLabels);

    Inject4(0x0Eu, 0xE0u, 0x7Fu, 0x7Fu); /* full-up bend */
    midi.Poll(2u);
    CHECK(midi.BendSemis() > 11.9f && midi.BendSemis() <= 12.0f);
    bend_slot.value_idx = 0u; /* Off */
    midi.Poll(3u);
    CHECK(midi.BendSemis() == 0.0f);

    alchemy::SettingsSlot prio_slot;
    prio_slot.kind      = alchemy::SettingsKind::Selector;
    prio_slot.num_zones = UsbMidi::kPriorityZones;
    prio_slot.value_idx = 1u; /* Low */
    alchemy::SelectorHandle prio(&prio_slot);
    midi.UsePrioritySetting(prio);

    Inject4(0x09u, 0x90u, 48u, 100u);
    Inject4(0x09u, 0x90u, 72u, 100u); /* higher press must not steal */
    midi.Poll(4u);
    CHECK_EQ(tracker.Note(), 48u);

    /* Labels attach only on a zone-count match. */
    alchemy::SettingsSlot bad_slot;
    bad_slot.kind      = alchemy::SettingsKind::Selector;
    bad_slot.num_zones = 4u;
    alchemy::SelectorHandle bad(&bad_slot);
    midi.UseBendRangeSetting(bad);
    CHECK(bad_slot.zone_labels == nullptr);
    CHECK(bad_slot.name != nullptr); /* still named */
}

int main()
{
    TestDescriptors();
    TestCodec();
    TestTracker();
    TestMonoSustain();
    TestPolyAllocator();
    TestService();
    TestServiceComplete();
    TestSettingsBindings();

    std::printf("midi_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
