/**
 * @file tests/host/button_tests.cpp
 * @brief VirtualButton / ButtonBank test battery.
 *
 * Covers the button surface end to end: roster discovery and identity
 * keying (dedupe, page attribution, byte order), serialization and the
 * schema hash, derived and declared gestures (tap / hold / set, the
 * press-time page latch), the pager consume handshake, the
 * Pressed()-only contract (RisingEdge is never consumed), deferred
 * change notification, declaration-error latching, and the descriptor's
 * "buttons" component emission.
 */

#include "button_tests.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "alchemy/host_link/describe.h"
#include "alchemy/hw/i_button.h"
#include "alchemy/surface/button_bank.h"
#include "alchemy/surface/knob_storage.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/presets.h"

using namespace alchemy;
using namespace alchemy::hostlink;

static daisy::QSPIHandle s_dummy_qspi;   /* stubbed on host */

/* ── Local check harness (totals returned to main) ─────────────────── */

static int s_checks   = 0;
static int s_failures = 0;

#define BCHECK(cond)                                                       \
    do {                                                                   \
        s_checks++;                                                        \
        if (!(cond)) {                                                     \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define BCHECK_EQ(a, b)                                                    \
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

namespace {

/* ── Fakes and drivers ─────────────────────────────────────────────── */

/** Debounced-button stub.  Counts the consume-on-read edge calls so the
 *  bank's Pressed()-only contract is enforceable, not aspirational. */
struct TestBtn : IButton
{
    bool p            = false;
    int  edge_reads   = 0;

    bool  Pressed() const override { return p; }
    bool  RisingEdge() override  { edge_reads++; return false; }
    bool  FallingEdge() override { edge_reads++; return false; }
    float TimeHeldMs() const override { return 0.f; }
};

struct Rig
{
    TestBtn  btn[3];
    IButton* refs[3] = {&btn[0], &btn[1], &btn[2]};
    uint32_t t       = 0;

    void Wire(ButtonBank& bank) { bank.Attach(refs, 3); }

    /** Advance @p ms one-millisecond polls at the given active page. */
    void Poll(ButtonBank& bank, uint32_t ms, uint8_t page = 0,
              bool gated = false)
    {
        for (uint32_t i = 0; i < ms; i++)
            bank.PollButtons(++t, gated, page);
    }

    void Tap(ButtonBank& bank, uint8_t hw, uint8_t page = 0)
    {
        btn[hw].p = true;
        Poll(bank, 2, page);
        btn[hw].p = false;
        Poll(bank, 2, page);
    }
};

struct FakeStorage : KnobStorage
{
    const IButton* page_btn = nullptr;
    int            consumed = 0;

    float   Stored(uint8_t, uint8_t) const override { return 0.f; }
    uint8_t ActivePage() const override { return 0; }
    uint8_t NumPots()    const override { return 6; }
    uint8_t NumPages()   const override { return 2; }
    void    Update(const float*, uint32_t) override {}

    const IButton* PageButton() const override { return page_btn; }
    void           ConsumeButton() override { consumed++; }
};

static const char* kModes[3] = {"LP", "BP", "HP"};
static const char* kOnOff[2] = {"Off", "On"};

/* ── Roster, identity keying, serialization ────────────────────────── */

void TestRosterAndSerialization()
{
    VirtualButton a = VirtualButton("flt.mode", "Filter")
                          .Hw(1).Selector(kModes).Default(1);
    VirtualButton b = VirtualButton("osc.sub", "Sub").Hw(2).Toggle();
    VirtualButton c = VirtualButton("shared", "Shared").Hw(0).Selector(4);
    VirtualButton g = VirtualButton("bypass", "Bypass").Hw(2).Toggle();
    VirtualButton m = VirtualButton("trig", "Trigger").Hw(1);

    Page pg0 = Page(0);
    pg0.Buttons(a, c, m);
    Page pg1 = Page(1);
    pg1.Buttons(b, c);   /* c on both pages → one cell, page omitted */

    Rig        rig;
    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg0, pg1).Global(g);

    BCHECK_EQ(bank.SerializedSize(), size_t{4});
    BCHECK(bank.Ok());

    /* Roster order: pg0 declaration order, then pg1, then globals. */
    BCHECK(bank.CellAt(0) == &a);
    BCHECK(bank.CellAt(1) == &c);
    BCHECK(bank.CellAt(2) == &b);
    BCHECK(bank.CellAt(3) == &g);

    /* Attribution: single page emits it, shared and global omit. */
    BCHECK_EQ(bank.CellPage(0), int16_t{0});
    BCHECK_EQ(bank.CellPage(1), int16_t{-1});
    BCHECK_EQ(bank.CellPage(2), int16_t{1});
    BCHECK_EQ(bank.CellPage(3), int16_t{-1});

    /* Momentary: modal roster, zero bytes, live Zone reads defaults. */
    BCHECK_EQ(bank.NumModal(), uint8_t{1});
    BCHECK(bank.ModalAt(0) == &m);
    BCHECK_EQ(a.Zone(), uint8_t{1});

    uint8_t blob[4] = {};
    bank.Serialize(blob);
    BCHECK_EQ(blob[0], uint8_t{1});   /* a's Default(1) */
    BCHECK_EQ(blob[1], uint8_t{0});

    /* Round-trip + clamp: out-of-range bytes land on the default. */
    const uint8_t in[4] = {2, 200, 1, 1};
    BCHECK(bank.Deserialize(in));
    BCHECK_EQ(a.Zone(), uint8_t{2});
    BCHECK_EQ(c.Zone(), uint8_t{0});   /* 200 → Default(0) */
    BCHECK_EQ(b.Zone(), uint8_t{1});
    BCHECK_EQ(g.Zone(), uint8_t{1});
}

/* ── Schema hash ───────────────────────────────────────────────────── */

void TestSchemaHash()
{
    auto hash_of = [](const char* ident, uint8_t zones)
    {
        VirtualButton b = VirtualButton(ident, "B").Hw(0).Selector(zones);
        Page          pg(0);
        pg.Buttons(b);
        ButtonBank bank;
        bank.Pages(pg);
        return bank.SchemaHash();
    };

    BCHECK_EQ(hash_of("a", 3), hash_of("a", 3));   /* stable */
    BCHECK(hash_of("a", 3) != hash_of("a", 4));    /* zones reshape */
    BCHECK(hash_of("a", 3) != hash_of("b", 3));    /* ident rename */
}

/* ── Gestures ──────────────────────────────────────────────────────── */

void TestDerivedTapCycle()
{
    uint8_t       target = 0;
    VirtualButton a =
        VirtualButton("m", "Mode").Hw(0).Selector(kModes).Bind(&target);
    Page pg(0);
    pg.Buttons(a);

    Rig        rig;
    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg);

    rig.Tap(bank, 0);
    BCHECK_EQ(a.Zone(), uint8_t{1});
    BCHECK_EQ(target, uint8_t{1});
    rig.Tap(bank, 0);
    rig.Tap(bank, 0);
    BCHECK_EQ(a.Zone(), uint8_t{0});   /* wraps mod 3 */
    BCHECK_EQ(target, uint8_t{0});
}

void TestHoldAndTapArbitration()
{
    int           taps = 0;
    VirtualButton a = VirtualButton("m", "Mode")
                          .Hw(0)
                          .Selector(kModes)
                          .Default(2)
                          .Tap(VirtualButton::Action::Cycle)
                          .HoldSet(500, 0, "Reset")
                          .OnChange([](uint8_t, void* ctx)
                                    { ++*static_cast<int*>(ctx); },
                                    &taps);
    Page pg(0);
    pg.Buttons(a);

    Rig        rig;
    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg);

    /* Short press: tap cycles 2 → 0. */
    rig.Tap(bank, 0);
    BCHECK_EQ(a.Zone(), uint8_t{0});
    BCHECK_EQ(taps, 1);

    /* Long press: hold fires Set(0) at threshold; the release that
     * follows must not also tap-cycle. */
    bank.SetZone(a, 2);
    bank.Update(rig.t);
    rig.btn[0].p = true;
    rig.Poll(bank, 600);
    BCHECK_EQ(a.Zone(), uint8_t{0});   /* HoldSet fired */
    rig.btn[0].p = false;
    rig.Poll(bank, 2);
    BCHECK_EQ(a.Zone(), uint8_t{0});   /* no trailing tap */

    /* A hold-only button gets no implicit tap. */
    VirtualButton h =
        VirtualButton("h", "H").Hw(1).Toggle().Hold(400,
                                                    VirtualButton::Action::Toggle);
    pg.Buttons(h);
    rig.Tap(bank, 1);
    BCHECK_EQ(h.Zone(), uint8_t{0});
}

void TestPressPageLatch()
{
    VirtualButton p0 = VirtualButton("p0", "P0").Hw(0).Selector(kModes);
    VirtualButton p1 = VirtualButton("p1", "P1").Hw(0).Selector(kModes);
    Page pg0(0), pg1(1);
    pg0.Buttons(p0);
    pg1.Buttons(p1);

    Rig        rig;
    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg0, pg1);

    /* Press begins on page 0; the active page changes mid-hold; the
     * release still resolves against page 0. */
    rig.btn[0].p = true;
    rig.Poll(bank, 2, /*page=*/0);
    rig.btn[0].p = false;
    rig.Poll(bank, 2, /*page=*/1);
    BCHECK_EQ(p0.Zone(), uint8_t{1});
    BCHECK_EQ(p1.Zone(), uint8_t{0});

    /* Same physical button, different page: dispatches to p1. */
    rig.Tap(bank, 0, /*page=*/1);
    BCHECK_EQ(p1.Zone(), uint8_t{1});
    BCHECK_EQ(p0.Zone(), uint8_t{1});
}

void TestConsumeHandshakeAndEdgeContract()
{
    VirtualButton a = VirtualButton("m", "Mode").Hw(0).Selector(kModes);
    VirtualButton other = VirtualButton("o", "Other").Hw(1).Toggle();
    Page pg(0);
    pg.Buttons(a, other);

    Rig         rig;
    FakeStorage storage;
    storage.page_btn = rig.refs[0];

    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg);
    bank.AttachStorage(&storage);

    rig.Tap(bank, 0);
    BCHECK_EQ(storage.consumed, 1);   /* shadows page-advance */
    rig.Tap(bank, 1);
    BCHECK_EQ(storage.consumed, 1);   /* other buttons don't */

    /* The bank derives edges from Pressed() only. */
    BCHECK_EQ(rig.btn[0].edge_reads, 0);
    BCHECK_EQ(rig.btn[1].edge_reads, 0);
    BCHECK_EQ(rig.btn[2].edge_reads, 0);
}

void TestGatedPressIsInert()
{
    VirtualButton a = VirtualButton("m", "Mode").Hw(0).Selector(kModes);
    Page pg(0);
    pg.Buttons(a);

    Rig        rig;
    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg);

    /* Press and release entirely under the gate: nothing fires. */
    rig.btn[0].p = true;
    rig.Poll(bank, 2, 0, /*gated=*/true);
    rig.btn[0].p = false;
    rig.Poll(bank, 2, 0, /*gated=*/true);
    BCHECK_EQ(a.Zone(), uint8_t{0});

    /* Press under the gate, release after: still inert (poisoned). */
    rig.btn[0].p = true;
    rig.Poll(bank, 2, 0, /*gated=*/true);
    rig.Poll(bank, 2, 0, /*gated=*/false);
    rig.btn[0].p = false;
    rig.Poll(bank, 2, 0, /*gated=*/false);
    BCHECK_EQ(a.Zone(), uint8_t{0});

    /* Fresh press after the gate works. */
    rig.Tap(bank, 0);
    BCHECK_EQ(a.Zone(), uint8_t{1});
}

/* ── Deferred change notification ──────────────────────────────────── */

void TestDeferredChangeFlush()
{
    int           fires  = 0;
    uint8_t       target = 0;
    VirtualButton a = VirtualButton("m", "Mode")
                          .Hw(0)
                          .Selector(kModes)
                          .Bind(&target)
                          .OnChange([](uint8_t, void* ctx)
                                    { ++*static_cast<int*>(ctx); },
                                    &fires);
    Page pg(0);
    pg.Buttons(a);

    Rig        rig;
    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg);

    const uint8_t in[1] = {2};
    BCHECK(bank.Deserialize(in));
    BCHECK_EQ(a.Zone(), uint8_t{2});   /* data applied immediately */
    BCHECK_EQ(fires, 0);               /* callbacks deferred */
    BCHECK_EQ(target, uint8_t{0});

    bank.Update(0);
    BCHECK_EQ(fires, 1);
    BCHECK_EQ(target, uint8_t{2});
    bank.Update(0);
    BCHECK_EQ(fires, 1);               /* flush is one-shot */

    /* Re-applying the same bytes is not a change. */
    BCHECK(bank.Deserialize(in));
    bank.Update(0);
    BCHECK_EQ(fires, 1);
}

/* ── Declaration-error latching ────────────────────────────────────── */

void TestOkLatch()
{
    {
        /* Label count mismatch. */
        VirtualButton a =
            VirtualButton("m", "Mode").Hw(0).Selector(3).Labels(kOnOff);
        Page pg(0);
        pg.Buttons(a);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* Duplicate ident across two buttons. */
        VirtualButton a = VirtualButton("dup", "A").Hw(0).Toggle();
        VirtualButton b = VirtualButton("dup", "B").Hw(1).Toggle();
        Page pg(0);
        pg.Buttons(a, b);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* Default outside the zone range. */
        VirtualButton a =
            VirtualButton("m", "Mode").Hw(0).Selector(3).Default(3);
        Page pg(0);
        pg.Buttons(a);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* A stateful button first seen after freeze: inert + latched. */
        VirtualButton a = VirtualButton("a", "A").Hw(0).Toggle();
        Page pg(0);
        pg.Buttons(a);
        Rig        rig;
        ButtonBank bank;
        rig.Wire(bank);
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(bank.Ok());

        VirtualButton late = VirtualButton("late", "Late").Hw(1).Toggle();
        pg.Buttons(late);
        rig.Tap(bank, 1);
        BCHECK_EQ(late.Zone(), uint8_t{0});
        BCHECK(!bank.Ok());
    }
}

/* ── Descriptor emission ───────────────────────────────────────────── */

void TestDescriptorEmission()
{
    static const LedPanel::Rgb kCols[3] = {
        {0x00, 0xFF, 0xFF}, {0xFF, 0xA0, 0x00}, {0xFF, 0x00, 0xFF}};

    VirtualButton flt = VirtualButton("flt.mode", "Filter")
                            .Hw(2)
                            .Selector(kModes)
                            .Default(1)
                            .Colors(kCols)
                            .Near("flt.cutoff");
    VirtualButton sub  = VirtualButton("osc.sub", "Sub").Hw(1).Toggle();
    VirtualButton host = VirtualButton("cfg.slot", "Slot").Selector(4);
    VirtualButton trig = VirtualButton("trig", "Trigger")
                             .Hw(0)
                             .Tap(+[](void*) {}, "Fire the kick");

    Page pg0(0), pg1(1);
    pg0.Buttons(flt, trig);
    pg1.Buttons(sub, host);

    Rig        rig;
    ButtonBank bank;
    rig.Wire(bank);
    bank.Pages(pg0, pg1);

    Presets presets{s_dummy_qspi};
    presets.Manage(bank);

    static char buf[8192];
    const uint32_t len = RenderDescriptor(
        buf, sizeof buf,
        {"btnbank", "Bank", "0.0.1", "gitbtn", "0.1.0", "v1"},
        presets, nullptr, nullptr, 0);
    BCHECK(len > 0u);
    const std::string json(buf, len);

    /* Component header: kind, size = one byte per stateful button. */
    BCHECK(json.find("\"kind\":\"buttons\",") != std::string::npos);
    BCHECK(json.find("\"size\":3,\"off\":0") != std::string::npos);

    /* Modal array precedes fields; the momentary is in it, with its
     * page and explicit label. */
    const auto modal  = json.find("\"modal\":[");
    const auto fields = json.find("\"fields\":[");
    BCHECK(modal != std::string::npos && fields != std::string::npos);
    BCHECK(modal < fields);
    BCHECK(json.find("\"id\":\"trig\",\"name\":\"Trigger\",\"index\":0,"
                     "\"page\":0")
           != std::string::npos);
    BCHECK(json.find("\"label\":\"Fire the kick\"") != std::string::npos);

    /* Stateful buttons: ordinary enum fields with a btn object. */
    BCHECK(json.find("\"id\":\"flt.mode\",\"name\":\"Filter\",\"off\":0,"
                     "\"page\":0,\"type\":\"enum\",\"zones\":3,\"def\":1")
           != std::string::npos);
    BCHECK(json.find("\"disp\":{\"kind\":\"enum\","
                     "\"labels\":[\"LP\",\"BP\",\"HP\"]}")
           != std::string::npos);
    BCHECK(json.find("\"near\":\"flt.cutoff\"") != std::string::npos);
    BCHECK(json.find("\"btn\":{\"index\":2,\"action\":\"cycle\",")
           != std::string::npos);
    BCHECK(json.find("\"label\":\"Cycle LP / BP / HP\"")
           != std::string::npos);

    /* Two-zone implicit tap reads as toggle. */
    BCHECK(json.find("\"btn\":{\"index\":1,\"action\":\"toggle\",")
           != std::string::npos);

    /* Host-only state: field present, no btn object. */
    const auto slot = json.find("\"id\":\"cfg.slot\"");
    BCHECK(slot != std::string::npos);
    const auto slot_end = json.find("}]}", slot);
    BCHECK(json.find("\"btn\":", slot) > slot_end);

    /* A latched bank fails the whole build — no descriptor beats a
     * wrong one. */
    VirtualButton bad =
        VirtualButton("bad", "Bad").Hw(0).Selector(3).Labels(kOnOff);
    Page pgb(0);
    pgb.Buttons(bad);
    ButtonBank broken;
    broken.Pages(pgb);
    Presets p2{s_dummy_qspi};
    p2.Manage(broken);
    static char buf2[8192];
    BCHECK_EQ(RenderDescriptor(buf2, sizeof buf2,
                               {"btnbank", "Bank", "0.0.1", "gitbtn",
                                "0.1.0", "v1"},
                               p2, nullptr, nullptr, 0),
              0u);
}

} // namespace

int RunButtonTests(int& checks, int& failures)
{
    TestRosterAndSerialization();
    TestSchemaHash();
    TestDerivedTapCycle();
    TestHoldAndTapArbitration();
    TestPressPageLatch();
    TestConsumeHandshakeAndEdgeContract();
    TestGatedPressIsInert();
    TestDeferredChangeFlush();
    TestOkLatch();
    TestDescriptorEmission();

    checks   += s_checks;
    failures += s_failures;
    return s_failures;
}
