/**
 * @file tests/host/pager_nav_tests.cpp
 * @brief Pager navigation-binding test battery: cycle, Shift layers,
 *        Latch pairs, poisoning, the clean-release contract, the view
 *        latch, and the ButtonBank / ParamLock integrations.
 */

#include "pager_nav_tests.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "alchemy/surface/button_bank.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/param_lock.h"

using namespace alchemy;

/* ── Local check harness (totals returned to main) ─────────────────── */

static int s_checks   = 0;
static int s_failures = 0;

#define NCHECK(cond)                                                       \
    do {                                                                   \
        s_checks++;                                                        \
        if (!(cond)) {                                                     \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define NCHECK_EQ(a, b)                                                    \
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

#define NCHECK_NEAR(a, b, tol)                                             \
    do {                                                                   \
        s_checks++;                                                        \
        const float va = (a);                                              \
        const float vb = (b);                                              \
        if (!(std::fabs(va - vb) <= (tol))) {                              \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s ~= %s  (%.6f vs %.6f)\n",          \
                        __FILE__, __LINE__, #a, #b, va, vb);               \
        }                                                                  \
    } while (0)

namespace {

/* ── Fakes and drivers ─────────────────────────────────────────────── */

constexpr uint8_t kPots = 6;

struct NavBtn : IButton
{
    bool p = false;

    bool  Pressed() const override { return p; }
    bool  RisingEdge() override  { return false; }
    bool  FallingEdge() override { return false; }
    float TimeHeldMs() const override { return 0.f; }
};

struct NavRig
{
    NavBtn   btn[3];
    float    phys[kPots];
    uint32_t t = 0;

    NavRig() { for (uint8_t i = 0; i < kPots; i++) phys[i] = 0.5f; }

    /** One control frame: 1 ms polls, then Update — which the loop
     *  skips while gated, as ControlLoop does. */
    void Frame(Pager& pager, bool gated = false, uint8_t polls = 4)
    {
        for (uint8_t i = 0; i < polls; i++) pager.PollButtons(++t, gated);
        if (!gated) pager.Update(phys, t);
    }

    void Tap(Pager& pager, uint8_t b)
    {
        btn[b].p = true;
        Frame(pager);
        btn[b].p = false;
        Frame(pager);
    }
};

bool SameRgb(LedPanel::Rgb a, LedPanel::Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/* ── Classic cycle: the pre-binding contract, unchanged ────────────── */

void TestClassicCycle()
{
    NavRig rig;
    Pager  pager(rig.btn[0], 3, kPots);

    NCHECK_EQ(pager.ActivePage(), 0u);
    NCHECK(pager.PageButton() == &rig.btn[0]);
    NCHECK(pager.ConsumesRelease(&rig.btn[0]));
    NCHECK(!pager.ConsumesRelease(&rig.btn[1]));

    /* The press alone does nothing; the clean release advances. */
    rig.btn[0].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);
    rig.btn[0].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);

    /* Wraps around the full pool. */
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 2u);
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* An advance re-arms catch on the destination page. */
    pager.SetStored(1, 0, 0.9f, rig.phys);
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 1u);
    NCHECK(!pager.Caught(0));
    NCHECK_NEAR(pager.Value(0), 0.9f, 1e-6f);   /* uncaught: holds stored */
    rig.phys[0] = 0.95f;                        /* crosses 0.9 */
    rig.Frame(pager);
    NCHECK(pager.Caught(0));
    NCHECK_NEAR(pager.Value(0), 0.95f, 1e-6f);  /* caught: follows the pot */
    rig.phys[0] = 0.5f;
    rig.Frame(pager);

    /* Same-frame consume suppresses exactly this release's advance. */
    rig.btn[0].p = true;
    rig.Frame(pager);
    rig.btn[0].p = false;
    pager.PollButtons(++rig.t, false);
    pager.ConsumeButton();                      /* consumer ran this frame */
    pager.Update(rig.phys, rig.t);
    NCHECK_EQ(pager.ActivePage(), 1u);

    /* A stray claim (no release this frame) evaporates. */
    pager.ConsumeButton();
    rig.Frame(pager);
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 2u);
}

/* ── Gate poisoning: presses under or spanning the gate stay inert ─── */

void TestGatePoisoning()
{
    NavRig rig;
    Pager  pager(rig.btn[0], 3, kPots);

    /* Pressed under the gate, released after it: no advance.  (This is
     * the documented fix: the pre-binding pager advanced here.) */
    rig.btn[0].p = true;
    rig.Frame(pager, /*gated=*/true);
    rig.Frame(pager, false);                    /* gate lifted, still held */
    rig.btn[0].p = false;
    rig.Frame(pager, false);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* Pressed before the gate, released under it: dropped. */
    rig.btn[0].p = true;
    rig.Frame(pager, false);
    rig.btn[0].p = false;
    rig.Frame(pager, true);
    rig.Frame(pager, false);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* A clean press after the window still works. */
    rig.Frame(pager, false);
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 1u);

    /* A release latched just before the gate rises is dropped —
     * Settings closing must not fire a stale advance. */
    rig.btn[0].p = true;
    rig.Frame(pager, false);
    rig.btn[0].p = false;
    pager.PollButtons(++rig.t, false);          /* release latched...   */
    rig.Frame(pager, true);                     /* ...then gate, no Update */
    rig.Frame(pager, true);
    rig.Frame(pager, false);
    NCHECK_EQ(pager.ActivePage(), 1u);
}

/* ── Shift: lifecycle, catch both ways, the no-smear invariant ─────── */

void TestShiftLifecycle()
{
    NavRig rig;
    Pager  pager(3, kPots);
    pager.Cycle(rig.btn[0], 0, 1).Shift(rig.btn[1], 2);

    NCHECK(pager.PageButton() == &rig.btn[0]);
    NCHECK(!pager.ConsumesRelease(&rig.btn[1]));  /* shift is hold-driven */

    /* Base pot 0 caught at 0.30; the layer's stored value sits at 0.80. */
    rig.phys[0] = 0.30f;
    pager.SetStored(0, 0, 0.30f, rig.phys);
    pager.SetStored(2, 0, 0.80f, rig.phys);
    rig.Frame(pager);
    NCHECK(pager.Caught(0));
    const float base_before = pager.Stored(0, 0);

    /* Engage on press: the layer shows, catch re-armed. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 2u);
    NCHECK(!pager.Caught(0));
    NCHECK(!pager.HoldClaimed(&rig.btn[1]));

    /* Approaching without crossing edits nothing. */
    rig.phys[0] = 0.60f;
    rig.Frame(pager);
    NCHECK(!pager.Caught(0));
    NCHECK_NEAR(pager.Stored(2, 0), 0.80f, 1e-6f);
    NCHECK(!pager.HoldClaimed(&rig.btn[1]));

    /* Crossing takes hold; the stored value follows; the hold is used. */
    rig.phys[0] = 0.85f;
    rig.Frame(pager);
    NCHECK(pager.Caught(0));
    rig.phys[0] = 0.90f;
    rig.Frame(pager);
    NCHECK_NEAR(pager.Stored(2, 0), 0.90f, 1e-6f);
    NCHECK(pager.HoldClaimed(&rig.btn[1]));
    NCHECK(pager.HoldUsed(rig.btn[1]));

    /* The base page never smeared: bit-identical through the hold. */
    NCHECK(pager.Stored(0, 0) == base_before);

    /* Release: return page, catch re-armed against the parked pot, no
     * value leap; HoldUsed still readable across the release frame. */
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);
    NCHECK(!pager.Caught(0));
    NCHECK(pager.Stored(0, 0) == base_before);
    NCHECK(pager.HoldUsed(rig.btn[1]));

    /* Sweep back through the base value to re-own the knob. */
    rig.phys[0] = 0.25f;
    rig.Frame(pager);
    NCHECK(pager.Caught(0));

    /* The layer kept the edit. */
    NCHECK_NEAR(pager.Stored(2, 0), 0.90f, 1e-6f);
}

/* ── Clean releases, and the pre-caught pot guard ──────────────────── */

void TestCleanHoldAndPreCaught()
{
    NavRig rig;
    Pager  pager(2, kPots);
    pager.Shift(rig.btn[1], 1);

    /* All defaults sit under their pots: pre-caught at engage.  Touch
     * nothing — the hold is clean. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    rig.Frame(pager);
    NCHECK(!pager.HoldClaimed(&rig.btn[1]));
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK(!pager.HoldUsed(rig.btn[1]));
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* A pre-caught pot that MOVES counts as used. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    rig.phys[2] = 0.62f;
    rig.Frame(pager);
    NCHECK(pager.HoldClaimed(&rig.btn[1]));
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK(pager.HoldUsed(rig.btn[1]));
    rig.phys[2] = 0.5f;
}

/* ── Sub-frame tap: press+release between two Updates ──────────────── */

void TestSubFrameTap()
{
    NavRig rig;
    Pager  pager(2, kPots);
    pager.Shift(rig.btn[1], 1);

    rig.btn[1].p = true;
    pager.PollButtons(++rig.t, false);
    rig.btn[1].p = false;
    pager.PollButtons(++rig.t, false);
    pager.Update(rig.phys, rig.t);
    NCHECK_EQ(pager.ActivePage(), 0u);          /* the layer never shows */
    NCHECK(!pager.HoldClaimed(&rig.btn[1]));    /* and the tap is clean  */

    /* A normal hold still engages afterwards. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);
    rig.btn[1].p = false;
    rig.Frame(pager);
}

/* ── Chord reach: two nav buttons down → navigation stands down ────── */

void TestChordReach()
{
    NavRig rig;
    Pager  pager(3, kPots);
    pager.Shift(rig.btn[1], 1).Shift(rig.btn[2], 2);

    /* Co-press while a layer is held: abort, both presses poisoned. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);
    rig.btn[2].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);

    rig.btn[2].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* Fresh presses work again. */
    rig.btn[2].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 2u);
    rig.btn[2].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);
}

/* ── Gate rise mid-hold: the layer aborts, the press poisons ───────── */

void TestGateAbortsLayer()
{
    NavRig rig;
    Pager  pager(2, kPots);
    pager.Shift(rig.btn[1], 1);

    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);

    /* Settings opens: the layer aborts immediately (poll context);
     * the catch re-arm lands on the first post-gate Update. */
    rig.Frame(pager, /*gated=*/true);
    NCHECK_EQ(pager.ActivePage(), 0u);
    rig.Frame(pager, true);
    rig.Frame(pager, false);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* The surviving press stays poisoned until released. */
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);
    rig.btn[1].p = false;
    rig.Frame(pager);

    /* And a clean press engages again. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);
    rig.btn[1].p = false;
    rig.Frame(pager);
}

/* ── Latch pairs + the cycle-slot resolution (the issue-#16 UI) ────── */

void TestLatchAndCycleResolution()
{
    enum : uint8_t { kVar = 0, kRnd = 1, kRev = 2 };

    NavRig rig;
    Pager  pager(3, kPots);
    pager.Cycle(rig.btn[0], kVar, kRev).Latch(rig.btn[2], kVar, kRnd);

    NCHECK(pager.ConsumesRelease(&rig.btn[2]));

    /* The pair toggles while a member is showing. */
    rig.Tap(pager, 2);
    NCHECK_EQ(pager.ActivePage(), kRnd);
    rig.Tap(pager, 2);
    NCHECK_EQ(pager.ActivePage(), kVar);
    rig.Tap(pager, 2);
    NCHECK_EQ(pager.ActivePage(), kRnd);

    /* The pair occupies one cycle slot: from RND, B1 leaves for Reverb. */
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), kRev);

    /* Off the pair the latch button is inert. */
    rig.Tap(pager, 2);
    NCHECK_EQ(pager.ActivePage(), kRev);

    /* Cycling back lands on the REMEMBERED member. */
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), kRnd);
}

/* ── Latch pair inside an IMPLICIT cycle list must not trap the cycle ── */

void TestLatchInImplicitCycle()
{
    NavRig rig;
    Pager  pager(rig.btn[0], 4, kPots);          /* classic ctor: all pages */
    pager.Latch(rig.btn[2], 2, 3);

    /* The pair appears as two implicit entries; cycling must step over
     * the sibling member instead of parking forever on the pair. */
    rig.Tap(pager, 0);
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 2u);           /* pair, remembered = 2 */
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 0u);           /* NOT stuck on 2/3 */

    /* The remembered member survives the loop. */
    rig.Tap(pager, 2);                           /* inert off the pair */
    rig.Tap(pager, 0);
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 2u);
}

/* ── Shift .From() scoping ─────────────────────────────────────────── */

void TestShiftFromScope()
{
    NavRig rig;
    Pager  pager(3, kPots);
    pager.Cycle(rig.btn[0], 0, 1).Shift(rig.btn[1], 2).From(0);

    /* In scope: engages. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 2u);
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* Out of scope: inert, and the hold stays clean. */
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 1u);
    rig.btn[1].p = true;
    rig.Frame(pager);
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);
    NCHECK(!pager.HoldClaimed(&rig.btn[1]));
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK(!pager.HoldUsed(rig.btn[1]));

    /* A used hold must not leave HoldUsed() sticking to a later press
     * that never engages. */
    rig.Tap(pager, 0);
    NCHECK_EQ(pager.ActivePage(), 0u);
    rig.btn[1].p = true;
    rig.Frame(pager);
    rig.phys[0] = 0.9f;                          /* pre-caught pot moves */
    rig.Frame(pager);
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK(pager.HoldUsed(rig.btn[1]));
    rig.phys[0] = 0.5f;
    rig.Tap(pager, 0);                           /* to page 1: out of scope */
    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK(!pager.HoldUsed(rig.btn[1]));         /* fresh, never engaged */
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK(!pager.HoldUsed(rig.btn[1]));
}

/* ── GoToPage outranks a held layer ────────────────────────────────── */

void TestGoToPageCancelsLayer()
{
    NavRig rig;
    Pager  pager(3, kPots);
    pager.Shift(rig.btn[1], 1);

    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);

    pager.GoToPage(2, rig.phys);
    NCHECK_EQ(pager.ActivePage(), 2u);

    /* The cancelled hold's release has no return leg. */
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 2u);
}

/* ── A preset landing mid-hold re-arms; the layer stays engaged ────── */

void TestDeserializeMidHold()
{
    NavRig rig;

    /* Source image: known values on both pages. */
    Pager src(2, kPots);
    src.SetStored(0, 0, 0.20f, rig.phys);
    src.SetStored(1, 0, 0.70f, rig.phys);
    std::vector<uint8_t> blob(src.SerializedSize());
    src.Serialize(blob.data());

    Pager pager(2, kPots);
    pager.Shift(rig.btn[1], 1);

    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);

    NCHECK(pager.Deserialize(blob.data()));
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);          /* still held, still showing */
    NCHECK_NEAR(pager.Stored(1, 0), 0.70f, 1e-6f);
    NCHECK_NEAR(pager.Stored(0, 0), 0.20f, 1e-6f);
    NCHECK(!pager.Caught(0));                   /* re-armed against the pot */

    /* The load rewrote the values but the USER edited nothing: clean. */
    NCHECK(!pager.HoldClaimed(&rig.btn[1]));

    /* Editing after the load counts as usual. */
    rig.phys[0] = 0.72f;                        /* crosses the loaded 0.70 */
    rig.Frame(pager);
    NCHECK(pager.HoldClaimed(&rig.btn[1]));
    rig.phys[0] = 0.5f;

    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);
}

/* ── The view latch (RetainPage / ReleasePage) ─────────────────────── */

void TestViewLatch()
{
    NavRig rig;
    Pager  pager(2, kPots);
    pager.Shift(rig.btn[1], 1);

    /* Retain begins while the layer shows: its exit defers. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    pager.RetainPage();
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);          /* lingering */

    /* Catch keeps absorbing on the lingering layer. */
    rig.phys[1] = 0.8f;
    rig.Frame(pager);
    NCHECK(pager.HoldClaimed(&rig.btn[1]));     /* still claims its button */

    pager.ReleasePage();
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);
    NCHECK_NEAR(pager.Stored(1, 1), 0.8f, 1e-6f);
    rig.phys[1] = 0.5f;
    rig.Frame(pager);

    /* Re-pressed during the linger: the hold resumes. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    pager.RetainPage();
    rig.btn[1].p = false;
    rig.Frame(pager);
    rig.btn[1].p = true;
    rig.Frame(pager);
    pager.ReleasePage();
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);          /* held normally again */
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* A layer engaged AFTER the retain began is not covered. */
    pager.RetainPage();
    rig.btn[1].p = true;
    rig.Frame(pager);
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);          /* exits immediately */
    pager.ReleasePage();

    /* A sub-frame tap during the linger must re-linger, not leave a
     * zombie-active layer. */
    rig.btn[1].p = true;
    rig.Frame(pager);
    pager.RetainPage();
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 1u);          /* lingering */
    rig.btn[1].p = true;
    pager.PollButtons(++rig.t, false);
    rig.btn[1].p = false;
    pager.PollButtons(++rig.t, false);
    pager.Update(rig.phys, rig.t);
    NCHECK_EQ(pager.ActivePage(), 1u);          /* still lingering */
    pager.ReleasePage();
    rig.Frame(pager);
    NCHECK_EQ(pager.ActivePage(), 0u);          /* latch drop still exits */
}

/* ── ButtonBank hold gestures shadow release-driven navigation ─────── */

void TestBankHoldShadowsCycle()
{
    constexpr uint8_t kHw0 = 0;

    NavRig rig;
    Pager  pager(rig.btn[0], 2, kPots);

    static int  s_hold_fires = 0;
    static auto hold_fn      = +[](void*) { s_hold_fires++; };
    s_hold_fires = 0;

    VirtualButton act = VirtualButton(kHw0, "Action").Ident("act")
                            .Hold(8, hold_fn, "Do the thing");
    Page          pg  = Page(0);
    pg.Buttons(act);

    IButton*   refs[3] = {&rig.btn[0], &rig.btn[1], &rig.btn[2]};
    ButtonBank bank;
    bank.Attach(refs, 3).Pages(pg);
    bank.AttachStorage(&pager);

    auto frame = [&](uint8_t polls = 4) {
        for (uint8_t i = 0; i < polls; i++)
        {
            bank.PollButtons(++rig.t, false, pager.ActivePage());
            pager.PollButtons(rig.t, false);
        }
        pager.Update(rig.phys, rig.t);
        bank.Update(rig.t);
    };

    /* The hold fires once; the much-later release advances nothing. */
    rig.btn[0].p = true;
    for (int i = 0; i < 5; i++) frame();        /* ≥ 8 ms held */
    NCHECK_EQ(s_hold_fires, 1);
    rig.btn[0].p = false;
    frame();
    NCHECK_EQ(pager.ActivePage(), 0u);          /* shadowed */

    /* A plain tap (no gesture fired) still advances the page. */
    rig.btn[0].p = true;
    frame(2);
    rig.btn[0].p = false;
    frame();
    NCHECK_EQ(pager.ActivePage(), 1u);
}

/* ── Indicator colors per binding ──────────────────────────────────── */

void TestIndicatorColors()
{
    constexpr LedPanel::Rgb kRed   = {0xFF, 0x00, 0x00};
    constexpr LedPanel::Rgb kGreen = {0x00, 0xFF, 0x00};
    constexpr LedPanel::Rgb kBlue  = {0x00, 0x00, 0xFF};
    constexpr LedPanel::Rgb kOff   = {0x00, 0x00, 0x00};

    NavRig rig;
    Pager  pager(3, kPots);
    pager.Cycle(rig.btn[0], 0, 1).Shift(rig.btn[1], 2);
    pager.SetPageColor(0, kRed);
    pager.SetPageColor(1, kGreen);
    pager.SetPageColor(2, kBlue);

    NCHECK(SameRgb(pager.IndicatorColor(&rig.btn[0]), kRed));
    NCHECK(SameRgb(pager.IndicatorColor(&rig.btn[1]), kOff));
    NCHECK(SameRgb(pager.IndicatorColor(&rig.btn[2]), kOff));

    rig.Tap(pager, 0);
    NCHECK(SameRgb(pager.IndicatorColor(&rig.btn[0]), kGreen));

    rig.btn[1].p = true;
    rig.Frame(pager);
    NCHECK(SameRgb(pager.IndicatorColor(&rig.btn[1]), kBlue));
    rig.btn[1].p = false;
    rig.Frame(pager);
    NCHECK(SameRgb(pager.IndicatorColor(&rig.btn[1]), kOff));
}

/* ── ButtonBank composition: hold-to-shift + tap on one button ─────── */

void TestBankTapComposition()
{
    constexpr uint8_t kHw1 = 1;

    NavRig rig;
    Pager  pager(2, kPots);
    pager.Shift(rig.btn[1], 1);

    VirtualButton mute = VirtualButton(kHw1, "Mute").Ident("mute").Toggle();
    Page          pg   = Page(0);
    pg.Buttons(mute);

    IButton*   refs[3] = {&rig.btn[0], &rig.btn[1], &rig.btn[2]};
    ButtonBank bank;
    bank.Attach(refs, 3).Pages(pg);
    bank.AttachStorage(&pager);

    /* ControlLoop's order: bank polls before storage. */
    auto frame = [&](uint8_t polls = 4) {
        for (uint8_t i = 0; i < polls; i++)
        {
            bank.PollButtons(++rig.t, false, pager.ActivePage());
            pager.PollButtons(rig.t, false);
        }
        pager.Update(rig.phys, rig.t);
        bank.Update(rig.t);
    };

    NCHECK_EQ(bank.ZoneOf(mute), 0u);

    /* Clean hold: the layer shows, nothing edits, the tap fires. */
    rig.btn[1].p = true;
    frame();
    frame();
    NCHECK_EQ(pager.ActivePage(), 1u);
    rig.btn[1].p = false;
    frame();
    NCHECK_EQ(bank.ZoneOf(mute), 1u);
    NCHECK_EQ(pager.ActivePage(), 0u);

    /* Used hold: a pot edit claims the release; the tap stays quiet. */
    rig.btn[1].p = true;
    frame();
    rig.phys[0] = 0.9f;                         /* pre-caught pot moves */
    frame();
    rig.btn[1].p = false;
    frame();
    NCHECK_EQ(bank.ZoneOf(mute), 1u);           /* unchanged */
    NCHECK_EQ(pager.ActivePage(), 0u);
    rig.phys[0] = 0.5f;
}

/* ── ParamLock: press-edge bank latch + the consume handshake ──────── */

void TestLockBankLatchAndConsume()
{
    NavRig rig;
    Pager  pager(2, kPots);
    pager.Shift(rig.btn[1], 1);
    ParamLock<static_cast<uint8_t>(2 * kPots)> locks(rig.btn[0], pager);

    /* One frame in the canonical order: locks update before the pager. */
    auto frame = [&](bool gated = false) {
        locks.SetFrameMs(16u);
        for (uint8_t i = 0; i < 4; i++)
        {
            locks.PollButtons(++rig.t, gated);
            pager.PollButtons(rig.t, gated);
        }
        locks.Advance();
        if (!gated)
        {
            locks.Update(rig.phys, rig.t);
            pager.Update(rig.phys, rig.t);
        }
    };

    /* Engage the layer, then start a lock hold on it. */
    rig.btn[1].p = true;
    frame();
    NCHECK_EQ(pager.ActivePage(), 1u);
    rig.btn[0].p = true;
    frame();                                    /* bank latched to page 1 */

    /* Release the layer button mid-hold: the view latch keeps the layer
     * showing and its catch running. */
    rig.btn[1].p = false;
    frame();
    NCHECK_EQ(pager.ActivePage(), 1u);

    /* Nudge past the gesture threshold: the recording lands in the
     * press-time bank — the layer's — not the base page's. */
    rig.phys[0] = 0.70f;
    frame();
    frame();
    NCHECK(locks.IsRecordingAtPage(1, 0));
    NCHECK(!locks.IsRecordingAtPage(0, 0));

    /* Trigger up: the deferred exit completes on the same frame. */
    rig.btn[0].p = false;
    frame();
    NCHECK_EQ(pager.ActivePage(), 0u);
    rig.phys[0] = 0.5f;
    frame();

    /* An explicit jump mid-hold cannot retarget the recording either. */
    rig.btn[0].p = true;
    frame();                                    /* bank latched to page 0 */
    pager.GoToPage(1, rig.phys);
    frame();
    rig.phys[1] = 0.75f;
    frame();
    frame();
    NCHECK(locks.IsRecordingAtPage(0, 1));
    NCHECK(!locks.IsRecordingAtPage(1, 1));
    rig.btn[0].p = false;
    frame();
    rig.phys[1] = 0.5f;
    pager.GoToPage(0, rig.phys);
    frame();
}

void TestLockConsumeIdentity()
{
    NavRig rig;
    Pager  pager(2, kPots);
    pager.Latch(rig.btn[0], 0, 1);              /* release-driven, on B1 */
    ParamLock<static_cast<uint8_t>(2 * kPots)> locks(rig.btn[0], pager);

    auto frame = [&] {
        locks.SetFrameMs(16u);
        for (uint8_t i = 0; i < 4; i++)
        {
            locks.PollButtons(++rig.t, false);
            pager.PollButtons(rig.t, false);
        }
        locks.Advance();
        locks.Update(rig.phys, rig.t);
        pager.Update(rig.phys, rig.t);
    };

    /* A hold-and-nudge lock gesture consumes its release: the latch on
     * the same button must not also toggle. */
    rig.btn[0].p = true;
    frame();
    rig.phys[0] = 0.70f;
    frame();
    frame();
    rig.btn[0].p = false;
    frame();
    NCHECK_EQ(pager.ActivePage(), 0u);          /* consumed, no toggle */
    rig.phys[0] = 0.5f;
    frame();

    /* A clean tap on the same button is not consumed: the latch fires. */
    rig.btn[0].p = true;
    frame();
    rig.btn[0].p = false;
    frame();
    NCHECK_EQ(pager.ActivePage(), 1u);
}

} // namespace

int RunPagerNavTests(int& checks, int& failures)
{
    TestClassicCycle();
    TestGatePoisoning();
    TestShiftLifecycle();
    TestCleanHoldAndPreCaught();
    TestSubFrameTap();
    TestChordReach();
    TestGateAbortsLayer();
    TestLatchAndCycleResolution();
    TestLatchInImplicitCycle();
    TestShiftFromScope();
    TestBankHoldShadowsCycle();
    TestGoToPageCancelsLayer();
    TestDeserializeMidHold();
    TestViewLatch();
    TestIndicatorColors();
    TestBankTapComposition();
    TestLockBankLatchAndConsume();
    TestLockConsumeIdentity();

    std::printf("pager-nav: %d checks, %d failures\n", s_checks, s_failures);
    checks   += s_checks;
    failures += s_failures;
    return s_failures == 0 ? 0 : 1;
}
