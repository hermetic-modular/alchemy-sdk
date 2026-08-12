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

/* Bare literal 0 is ctor-ambiguous by design; boards use kButtonB1. */
constexpr uint8_t kBtn0 = 0, kBtn1 = 1, kBtn2 = 2;

/* ── Roster, identity keying, serialization ────────────────────────── */

void TestRosterAndSerialization()
{
    VirtualButton a = VirtualButton(kBtn1, "Filter")
                          .Ident("flt.mode").Selector(kModes).Default(1);
    VirtualButton b = VirtualButton(kBtn2, "Sub").Ident("osc.sub").Toggle();
    VirtualButton c = VirtualButton(kBtn0, "Shared").Ident("shared").Selector(4);
    VirtualButton g = VirtualButton(kBtn2, "Bypass").Ident("bypass").Toggle();
    VirtualButton m = VirtualButton(kBtn1, "Trigger").Ident("trig");

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
        VirtualButton b = VirtualButton(kBtn0, "B").Ident(ident).Selector(zones);
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
        VirtualButton(kBtn0, "Mode").Selector(kModes).Bind(&target);
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
    VirtualButton a = VirtualButton(kBtn0, "Mode")
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
        VirtualButton(kBtn1, "H").Toggle().Hold(400,
                                                VirtualButton::Action::Toggle);
    pg.Buttons(h);
    rig.Tap(bank, 1);
    BCHECK_EQ(h.Zone(), uint8_t{0});
}

void TestPressPageLatch()
{
    /* Same hw on two pages: explicit idents are required (derived
     * "b0" ids would collide). */
    VirtualButton p0 = VirtualButton(kBtn0, "P0").Ident("p0").Selector(kModes);
    VirtualButton p1 = VirtualButton(kBtn0, "P1").Ident("p1").Selector(kModes);
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
    VirtualButton a = VirtualButton(kBtn0, "Mode").Selector(kModes);
    VirtualButton other = VirtualButton(kBtn1, "Other").Toggle();
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
    VirtualButton a = VirtualButton(kBtn0, "Mode").Selector(kModes);
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
    VirtualButton a = VirtualButton(kBtn0, "Mode")
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
            VirtualButton(kBtn0, "Mode").Selector(3).Labels(kOnOff);
        Page pg(0);
        pg.Buttons(a);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* Duplicate ident across two buttons. */
        VirtualButton a = VirtualButton(kBtn0, "A").Ident("dup").Toggle();
        VirtualButton b = VirtualButton(kBtn1, "B").Ident("dup").Toggle();
        Page pg(0);
        pg.Buttons(a, b);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* Two ident-less buttons on one hw index: derived "b0" ids
         * collide. */
        VirtualButton a = VirtualButton(kBtn0, "A").Toggle();
        VirtualButton b = VirtualButton(kBtn0, "B").Toggle();
        Page pg(0);
        pg.Buttons(a, b);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* Stateful with neither ident nor hw: no derivable id. */
        VirtualButton a = VirtualButton(nullptr, "X").Selector(3);
        Page pg(0);
        pg.Buttons(a);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* Default outside the zone range. */
        VirtualButton a =
            VirtualButton(kBtn0, "Mode").Selector(3).Default(3);
        Page pg(0);
        pg.Buttons(a);
        ButtonBank bank;
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(!bank.Ok());
    }
    {
        /* A stateful button first seen after freeze: inert + latched. */
        VirtualButton a = VirtualButton(kBtn0, "A").Toggle();
        Page pg(0);
        pg.Buttons(a);
        Rig        rig;
        ButtonBank bank;
        rig.Wire(bank);
        bank.Pages(pg);
        (void)bank.SerializedSize();
        BCHECK(bank.Ok());

        VirtualButton late = VirtualButton(kBtn1, "Late").Toggle();
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

    VirtualButton flt = VirtualButton(kBtn2, "Filter")
                            .Ident("flt.mode")
                            .Selector(kModes)
                            .Default(1)
                            .Colors(kCols)
                            .Anchor("cfg.slot");
    VirtualButton sub  = VirtualButton(kBtn1, "Sub").Ident("osc.sub").Toggle();
    VirtualButton noid = VirtualButton(kBtn1, "NoId").Toggle();
    VirtualButton host = VirtualButton("cfg.slot", "Slot").Selector(4);
    VirtualButton trig = VirtualButton(kBtn0, "Trigger")
                             .Ident("trig")
                             .Tap(+[](void*) {}, "Fire the kick");

    Page pg0(0), pg1(1);
    pg0.Buttons(flt, trig, noid);
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
    BCHECK(json.find("\"size\":4,\"off\":0") != std::string::npos);

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
    BCHECK(json.find("\"anchor\":\"cfg.slot\"") != std::string::npos);
    BCHECK(json.find("\"btn\":{\"index\":2,\"action\":\"cycle\",")
           != std::string::npos);
    BCHECK(json.find("\"label\":\"Cycle LP / BP / HP\"")
           != std::string::npos);

    /* Two-zone implicit tap reads as toggle. */
    BCHECK(json.find("\"btn\":{\"index\":1,\"action\":\"toggle\",")
           != std::string::npos);

    /* Ident-less button: positional "b<hw>" id, dense offset. */
    BCHECK(json.find("\"id\":\"b1\",\"name\":\"NoId\",\"off\":1,"
                     "\"page\":0")
           != std::string::npos);

    /* Host-only state: field present, no btn object. */
    const auto slot = json.find("\"id\":\"cfg.slot\"");
    BCHECK(slot != std::string::npos);
    const auto slot_end = json.find("}]}", slot);
    BCHECK(json.find("\"btn\":", slot) > slot_end);

    /* A latched bank fails the whole build — no descriptor beats a
     * wrong one. */
    VirtualButton bad =
        VirtualButton(kBtn0, "Bad").Selector(3).Labels(kOnOff);
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

/* ── Anchor / controls ref validation ──────────────────────────────── */

/** Minimal fielded component: one f32 "flt.cutoff" at offset 0. */
struct CutoffComp : Serializable
{
    size_t   SerializedSize() const override { return 4u; }
    uint32_t SchemaHash()     const override { return 0xC07C07u; }
    void     Serialize(uint8_t* out) const override { std::memset(out, 0, 4u); }
    bool     Deserialize(const uint8_t*) override { return true; }
    bool     Describe(hostlink::ComponentWriter& w) const override
    {
        return w.Field("flt.cutoff", "Cutoff", 0u, FieldType::F32, 0.5f);
    }
};

void TestAnchorValidation()
{
    static const DescriptorBuilder::ModuleInfo kVInfo =
        {"btnbank", "Bank", "0.0.1", "gitbtn", "0.1.0", "v1"};

    {
        /* Dangling target: no descriptor, and the reason names it. */
        VirtualButton flt = VirtualButton(kBtn2, "Filter")
                                .Ident("flt.mode")
                                .Selector(kModes)
                                .Anchor("ftl.cutoff");
        Page pg(0);
        pg.Buttons(flt);
        ButtonBank bank;
        bank.Pages(pg);
        Presets presets{s_dummy_qspi};
        presets.Manage(bank);
        (void)bank.SerializedSize();

        static char buf[4096];
        DescriptorBuilder db(buf, sizeof buf, presets);
        db.Begin(kVInfo);
        db.BeginButtons("buttons", bank);
        for (uint8_t i = 0; i < bank.NumCells(); i++)
            db.ButtonsField(*bank.CellAt(i), i, bank.CellPage(i));
        db.EndButtons();
        BCHECK_EQ(db.Finish(), 0u);
        BCHECK(db.LastError() != nullptr
               && std::strstr(db.LastError(), "ftl.cutoff") != nullptr);
    }
    {
        /* Cross-component target, ref managed before the target. */
        VirtualButton flt = VirtualButton(kBtn2, "Filter")
                                .Ident("flt.mode")
                                .Selector(kModes)
                                .Anchor("flt.cutoff");
        Page pg(0);
        pg.Buttons(flt);
        Rig        rig;
        ButtonBank bank;
        rig.Wire(bank);
        bank.Pages(pg);
        CutoffComp cutoff;
        Presets    presets{s_dummy_qspi};
        presets.Manage(bank);
        presets.Manage(cutoff);
        static char buf[8192];
        const uint32_t len = RenderDescriptor(buf, sizeof buf, kVInfo,
                                              presets, nullptr, nullptr, 0);
        BCHECK(len > 0u);
        BCHECK(std::string(buf, len).find("\"anchor\":\"flt.cutoff\"")
               != std::string::npos);
    }
    {
        /* Same target, target managed first. */
        VirtualButton flt = VirtualButton(kBtn2, "Filter")
                                .Ident("flt.mode")
                                .Selector(kModes)
                                .Anchor("flt.cutoff");
        Page pg(0);
        pg.Buttons(flt);
        Rig        rig;
        ButtonBank bank;
        rig.Wire(bank);
        bank.Pages(pg);
        CutoffComp cutoff;
        Presets    presets{s_dummy_qspi};
        presets.Manage(cutoff);
        presets.Manage(bank);
        static char buf[8192];
        BCHECK(RenderDescriptor(buf, sizeof buf, kVInfo, presets, nullptr,
                                nullptr, 0)
               > 0u);
    }
    {
        /* Positional "b<hw>" ids are anchorable. */
        VirtualButton noid = VirtualButton(kBtn1, "NoId").Toggle();
        VirtualButton flt  = VirtualButton(kBtn2, "Filter")
                                .Ident("flt.mode")
                                .Selector(kModes)
                                .Anchor("b1");
        Page pg(0);
        pg.Buttons(noid, flt);
        Rig        rig;
        ButtonBank bank;
        rig.Wire(bank);
        bank.Pages(pg);
        Presets presets{s_dummy_qspi};
        presets.Manage(bank);
        static char buf[8192];
        BCHECK(RenderDescriptor(buf, sizeof buf, kVInfo, presets, nullptr,
                                nullptr, 0)
               > 0u);
    }
    {
        /* A modal id is not a field. */
        VirtualButton trig = VirtualButton(kBtn0, "Trigger")
                                 .Ident("trig")
                                 .Tap(+[](void*) {}, "Fire");
        VirtualButton flt = VirtualButton(kBtn2, "Filter")
                                .Ident("flt.mode")
                                .Selector(kModes)
                                .Anchor("trig");
        Page pg(0);
        pg.Buttons(trig, flt);
        Rig        rig;
        ButtonBank bank;
        rig.Wire(bank);
        bank.Pages(pg);
        Presets presets{s_dummy_qspi};
        presets.Manage(bank);
        static char buf[8192];
        BCHECK_EQ(RenderDescriptor(buf, sizeof buf, kVInfo, presets, nullptr,
                                   nullptr, 0),
                  0u);
    }
    {
        /* Self-anchor. */
        VirtualButton flt = VirtualButton(kBtn2, "Filter")
                                .Ident("flt.mode")
                                .Selector(kModes)
                                .Anchor("flt.mode");
        Page pg(0);
        pg.Buttons(flt);
        Rig        rig;
        ButtonBank bank;
        rig.Wire(bank);
        bank.Pages(pg);
        Presets presets{s_dummy_qspi};
        presets.Manage(bank);
        static char buf[8192];
        BCHECK_EQ(RenderDescriptor(buf, sizeof buf, kVInfo, presets, nullptr,
                                   nullptr, 0),
                  0u);
    }
    {
        /* Legacy root-array Controls: a resolving ref passes, a dangling
         * one fails the same way. */
        VirtualButton good =
            VirtualButton("b2", "Osc Source")
                .Role(VirtualButton::Role::State)
                .Controls("flt.cutoff", VirtualButton::Action::Cycle);
        CutoffComp cutoff;
        Presets    presets{s_dummy_qspi};
        presets.Manage(cutoff);
        static char buf[8192];
        BCHECK(RenderDescriptor(buf, sizeof buf, kVInfo, presets, nullptr,
                                nullptr, 0, &good, 1)
               > 0u);

        VirtualButton bad =
            VirtualButton("b2", "Osc Source")
                .Role(VirtualButton::Role::State)
                .Controls("nope", VirtualButton::Action::Cycle);
        CutoffComp cutoff2;
        Presets    p2{s_dummy_qspi};
        p2.Manage(cutoff2);
        static char buf2[8192];
        BCHECK_EQ(RenderDescriptor(buf2, sizeof buf2, kVInfo, p2, nullptr,
                                   nullptr, 0, &bad, 1),
                  0u);
    }
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
    TestAnchorValidation();

    checks   += s_checks;
    failures += s_failures;
    return s_failures;
}
