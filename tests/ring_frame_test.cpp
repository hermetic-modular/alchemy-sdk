/**
 * @file ring_frame_test.cpp
 * @brief Host tests for RingFrame composition, Field evaluation, and the
 *        legacy Draw* wrappers.
 *
 * Renders into a capture strip through a synthetic 13-LED single-ring
 * layout (matching the Alchemy Lab V1 arc) and asserts on the captured
 * pixels.  Panel brightness is set to 1.0 so expected values are exact.
 */

#include "alchemy/led/panel.h"
#include "alchemy/led/ring_frame.h"
#include "alchemy/led/signal.h"
#include "alchemy/led/animations.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

constexpr uint8_t kLeds = 13u;

struct CaptureStrip : alchemy::ILedStrip
{
    uint8_t r[kLeds] = {}, g[kLeds] = {}, b[kLeds] = {};

    void SetPixel(uint16_t idx, uint8_t rr, uint8_t gg, uint8_t bb) override
    {
        if (idx >= kLeds) return;
        r[idx] = rr; g[idx] = gg; b[idx] = bb;
    }
    void Clear() override
    {
        for (uint8_t i = 0; i < kLeds; i++) { r[i] = g[i] = b[i] = 0u; }
    }
    void     Show() override {}
    bool     Busy() const override { return false; }
    uint16_t NumLeds() const override { return kLeds; }
};

/* One ring whose offset 0 sits at the arc start and advances one LED per
 * step — ring offset == arc step, so captured pixel k is arc LED k. */
constexpr alchemy::LedRingLayout kRing[] = {
    {0u, kLeds, 7.5f, 0.75f},
};
constexpr alchemy::HardwareLayout kLayout = {
    1u,       /* pots */
    0u,       /* cv   */
    0u,       /* btns */
    kLeds,    /* led_total */
    kRing,
    nullptr,  /* buttons */
};
constexpr alchemy::ArcGeometry kGeo{7.5f, 0.75f, kLeds};

struct Rig
{
    CaptureStrip      strip;
    alchemy::LedPanel panel;

    Rig()
    {
        panel.Init(strip, kLayout);
        panel.SetBrightness(1.0f);
    }
};

int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                    \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

bool Near(uint8_t v, int want, int tol = 2)
{
    return v >= want - tol && v <= want + tol;
}

/* ── Edge fill ──────────────────────────────────────────────────────────── */

void TestEdgeFill()
{
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.color = {200u, 100u, 50u};
    f.Base(d, 0.5f);            /* 6.5 LEDs: 6 full + half tip */
    f.Emit(rig.panel, 0u);

    for (uint8_t k = 0; k < 6u; k++) CHECK(rig.strip.r[k] == 200u);
    CHECK(Near(rig.strip.r[6], 100));   /* fractional tip */
    for (uint8_t k = 7; k < kLeds; k++) CHECK(rig.strip.r[k] == 0u);
}

void TestEdgeFillCcw()
{
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.color     = {200u, 0u, 0u};
    d.direction = alchemy::FillDirection::Ccw;
    f.Base(d, 0.5f);
    f.Emit(rig.panel, 0u);

    for (uint8_t k = 7; k < kLeds; k++) CHECK(rig.strip.r[k] == 200u);
    CHECK(Near(rig.strip.r[6], 100));
    for (uint8_t k = 0; k < 6u; k++) CHECK(rig.strip.r[k] == 0u);
}

/* ── Center fill ────────────────────────────────────────────────────────── */

void TestCenterFill()
{
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.mode         = alchemy::FillMode::Center;
    d.color        = {0u, 200u, 0u};
    d.neg_color    = {200u, 0u, 0u};
    d.center_color = {50u, 50u, 50u};
    f.Base(d, 1.0f);            /* full CW arm from midpoint pivot */
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.g[6] == 50u);                        /* pivot */
    for (uint8_t k = 7; k < kLeds; k++) CHECK(rig.strip.g[k] == 200u);
    for (uint8_t k = 0; k < 6u; k++) CHECK(rig.strip.g[k] == 0u);
}

void TestCenterFillOffsetPivot()
{
    /* Pivot at 2/3 of the throw: pivot LED = round(0.667 · 12) = 8; each
     * arm normalized to its own side. */
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.mode         = alchemy::FillMode::Center;
    d.color        = {0u, 0u, 200u};
    d.neg_color    = {200u, 0u, 0u};
    d.center_color = {50u, 50u, 50u};
    d.pivot01      = 0.667f;
    f.Base(d, 0.0f);            /* full CCW arm */
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[8] == 50u);                        /* pivot LED */
    for (uint8_t k = 0; k < 8u; k++) CHECK(rig.strip.r[k] == 200u);
    for (uint8_t k = 9; k < kLeds; k++) CHECK(rig.strip.r[k] == 0u);
}

/* ── Regions ────────────────────────────────────────────────────────────── */

void TestRegions()
{
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.color = {100u, 100u, 100u};
    f.Base(d, 0.5f);            /* Active = LEDs 0..6, PastTip = 7..12 */

    /* Field over Active only: zero it out entirely. */
    alchemy::FieldDesc off;      /* Uniform, phase-driven to m = 1 */
    f.Field(alchemy::Region::Active, off, 1.0f, 0u, 0.5f);
    f.Emit(rig.panel, 0u);

    for (uint8_t k = 0; k < 7u; k++) CHECK(rig.strip.r[k] == 0u);
}

/* ── Pip compose / motion / trail ───────────────────────────────────────── */

void TestPipAddAndCarve()
{
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.color = {100u, 0u, 0u};
    f.Base(d, 1.0f);            /* all LEDs at r=100 */

    alchemy::PipDesc add;
    add.compose = alchemy::PipCompose::Add;
    add.color   = {100u, 0u, 0u};
    f.Pip(alchemy::Region::Full, add, 0.0f);   /* LED 0 → 200 */

    alchemy::PipDesc carve;
    carve.compose = alchemy::PipCompose::Carve;
    f.Pip(alchemy::Region::Full, carve, 1.0f); /* LED 12 → 0 */

    f.Emit(rig.panel, 0u);
    CHECK(rig.strip.r[0] == 200u);
    CHECK(rig.strip.r[6] == 100u);
    CHECK(rig.strip.r[12] == 0u);
}

void TestPipMotionAndSmooth()
{
    Rig rig;
    alchemy::RingFrame f;
    f.BeginOverlay(kGeo);

    alchemy::PipDesc p;
    p.color  = {240u, 0u, 0u};
    p.smooth = true;
    p.motion = alchemy::PipMotion::Wrap;
    f.Pip(alchemy::Region::Full, p, 1.25f);    /* wraps to 0.25 → LED 3 */
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[3] == 240u);              /* 0.25 · 12 = 3 exactly */
    CHECK(rig.strip.r[4] == 0u);

    /* Bounce: phase 0.75 folds to 0.5 → LED 6. */
    Rig rig2;
    alchemy::RingFrame f2;
    f2.BeginOverlay(kGeo);
    p.motion = alchemy::PipMotion::Bounce;
    f2.Pip(alchemy::Region::Full, p, 0.75f);
    f2.Emit(rig2.panel, 0u);
    CHECK(rig2.strip.r[6] == 240u);
}

void TestPipTrail()
{
    Rig rig;
    alchemy::RingFrame f;
    f.BeginOverlay(kGeo);

    alchemy::PipDesc p;
    p.color         = {200u, 0u, 0u};
    p.smooth        = true;
    p.trail_steps   = 2u;
    p.trail_falloff = 0.5f;
    p.trail_side    = alchemy::PipTrailSide::Ccw;
    f.Pip(alchemy::Region::Full, p, 0.5f);      /* core at LED 6 */
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[6] == 200u);
    CHECK(Near(rig.strip.r[5], 100));            /* falloff¹ */
    CHECK(Near(rig.strip.r[4], 50));             /* falloff² */
    CHECK(rig.strip.r[7] == 0u);                 /* CW side dark */
}

/* ── Overlay emit writes only touched LEDs ──────────────────────────────── */

void TestOverlayEmit()
{
    Rig rig;
    /* Pre-existing content on the ring. */
    rig.panel.SetRingByHour(0u, 7.5f, {9u, 9u, 9u});

    alchemy::RingFrame f;
    f.BeginOverlay(kGeo);
    alchemy::PipDesc p;
    p.color = {200u, 0u, 0u};
    f.Pip(alchemy::Region::Full, p, 1.0f);       /* LED 12 only */
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[0] == 9u);                 /* untouched survives */
    CHECK(rig.strip.r[12] == 200u);
}

/* ── Field patterns ─────────────────────────────────────────────────────── */

void TestFieldDeterminism()
{
    const alchemy::FieldDesc shimmer = alchemy::FieldShimmer(70u);
    const float a = alchemy::FieldEval(shimmer, 3u, 1000u, -1.0f);
    const float b = alchemy::FieldEval(shimmer, 3u, 1000u, -1.0f);
    const float c = alchemy::FieldEval(shimmer, 3u, 1000u + 70u, -1.0f);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a >= 0.0f && a < 1.0f);

    /* Held: constant within a bucket. */
    const alchemy::FieldDesc stutter = alchemy::FieldStutter(120u);
    CHECK(alchemy::FieldEval(stutter, 0u, 240u, -1.0f)
          == alchemy::FieldEval(stutter, 0u, 300u, -1.0f));
}

void TestFieldWavePhaseDrive()
{
    alchemy::FieldDesc wave = alchemy::FieldRipple(0u, 4.0f);
    /* Explicit phase: the crest moves with it. */
    const float m0 = alchemy::FieldEval(wave, 0u, 0u, 0.0f);
    const float m1 = alchemy::FieldEval(wave, 0u, 0u, 0.25f);
    CHECK(std::fabs(m0 - m1) > 0.1f);
}

/* ── Signals ────────────────────────────────────────────────────────────── */

void TestSignals()
{
    CHECK(alchemy::Phase01(1500u, 1000u) == 0.5f);
    CHECK(alchemy::Tri01(0.25f) == 0.5f);
    CHECK(alchemy::Tri01(0.75f) == 0.5f);

    CHECK(alchemy::NormTapered(0.005f, 0.01f, 60.0f) < 0.0f);   /* floor  */
    CHECK(alchemy::NormTapered(90.0f, 0.01f, 60.0f) == 1.0f);   /* pegged */
    const float mid = alchemy::NormTapered(15.0f, 0.01f, 60.0f);
    CHECK(mid > 0.49f && mid < 0.51f);                          /* sqrt   */
}

/* ── Legacy wrappers ────────────────────────────────────────────────────── */

void TestLegacyDrawFill()
{
    Rig rig;
    alchemy::FillDesc d;
    d.mode         = alchemy::FillMode::Center;
    d.color        = {0u, 200u, 0u};
    d.center_color = {50u, 50u, 50u};
    /* Legacy Center convention: value −1..+1 around the midpoint. */
    alchemy::DrawFill(rig.panel, 0u, kGeo, 1.0f, d);

    CHECK(rig.strip.g[6] == 50u);
    for (uint8_t k = 7; k < kLeds; k++) CHECK(rig.strip.g[k] == 200u);
}

void TestLegacyDrawSelector()
{
    Rig rig;
    alchemy::SelectorDesc sel;
    sel.num_zones      = 6u;
    sel.active_color   = {200u, 0u, 0u};
    sel.inactive_color = {100u, 0u, 0u};
    sel.inactive_dim   = 0.5f;
    alchemy::DrawSelector(rig.panel, 0u, kGeo, 0.0f, sel);

    CHECK(rig.strip.r[0] == 200u);                /* zone 0 selected */
    CHECK(Near(rig.strip.r[12], 50));             /* zone 5 dimmed   */
    CHECK(rig.strip.r[1] == 0u);                  /* between zones   */
}

void TestLegacyDrawPip()
{
    Rig rig;
    rig.panel.SetRingByHour(0u, 7.5f, {9u, 9u, 9u});

    alchemy::PipDesc p;
    p.color  = {200u, 0u, 0u};
    p.smooth = true;
    alchemy::DrawPip(rig.panel, 0u, kGeo, 0.5f, p);

    CHECK(rig.strip.r[6] == 200u);
    CHECK(rig.strip.r[0] == 9u);                  /* overlay: untouched */
}

/* ── Regression coverage ────────────────────────────────────────────────── */

void TestCenterFillDefaultPivot()
{
    /* Value at the default midpoint pivot: only the pivot LED lights. */
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.mode         = alchemy::FillMode::Center;
    d.color        = {0u, 200u, 0u};
    d.center_color = {50u, 50u, 50u};
    f.Base(d, 0.5f);
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.g[6] == 50u);
    for (uint8_t k = 0; k < kLeds; k++)
        if (k != 6u) CHECK(rig.strip.g[k] == 0u);
}

void TestEmitSaturation()
{
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::FillDesc d;
    d.color = {200u, 0u, 0u};
    f.Base(d, 1.0f);

    alchemy::PipDesc add;
    add.compose = alchemy::PipCompose::Add;
    add.color   = {200u, 0u, 0u};
    f.Pip(alchemy::Region::Full, add, 0.0f);   /* LED 0 → 400 pre-quantize */
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[0] == 255u);              /* saturates, no wrap */
}

void TestOverlayFieldIsNoOp()
{
    /* A Field in an overlay frame modulates only what the frame drew —
     * over an empty overlay it must not black out the underlying render. */
    Rig rig;
    rig.panel.SetRingByHour(0u, 7.5f, {9u, 9u, 9u});

    alchemy::RingFrame f;
    f.BeginOverlay(kGeo);
    f.Field(alchemy::Region::Full, alchemy::FieldShimmer(70u), 1.0f, 35u);
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[0] == 9u);                /* survives */
}

void TestSelectorRegionFallbackZone()
{
    /* 8 Region zones cannot fit on 13 LEDs → Distributed fallback; zone
     * selection must use the Distributed rounding rule.  value 0.12 →
     * round(0.12·7) = zone 1 at LED round(1·12/7) = 2. */
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::SelectorDesc sel;
    sel.zone_geo     = alchemy::ZoneGeometry::Region;
    sel.num_zones    = 8u;
    sel.active_color = {200u, 0u, 0u};
    f.Base(sel, 0.12f);
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[2] == 200u);
    CHECK(rig.strip.r[0] == 0u);
}

void TestSelectorManyZones()
{
    /* Zone counts past avail_mask's 16 bits must render (zones ≥ 16
     * unavailable) without invoking an out-of-range shift. */
    Rig rig;
    alchemy::RingFrame f;
    f.Begin(kGeo);
    alchemy::SelectorDesc sel;
    sel.num_zones    = 40u;
    sel.active_color = {200u, 0u, 0u};
    sel.avail_mask   = 0x0001u;                 /* zone 0 only */
    f.Base(sel, 0.0f);
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[0] == 200u);              /* zone 0 still selected */
}

void TestPipZeroWidth()
{
    /* width = 0 clamps to a single-LED pip. */
    Rig rig;
    alchemy::RingFrame f;
    f.BeginOverlay(kGeo);
    alchemy::PipDesc p;
    p.color = {200u, 0u, 0u};
    p.width = 0u;
    f.Pip(alchemy::Region::Full, p, 0.5f);
    f.Emit(rig.panel, 0u);

    CHECK(rig.strip.r[6] == 200u);
}

void TestSignalEdgeCases()
{
    using alchemy::NormTapered;
    using alchemy::Taper;

    /* floor = 0 must hide, not produce NaN (Log divided by log(0)). */
    CHECK(NormTapered(5.0f, 0.0f, 60.0f, Taper::Log) < 0.0f);
    /* Degenerate range hides. */
    CHECK(NormTapered(5.0f, 10.0f, 10.0f) < 0.0f);
    /* Sqrt rises continuously from 0 at the floor. */
    CHECK(NormTapered(50.001f, 50.0f, 60.0f, Taper::Sqrt) < 0.05f);
    /* Negative phase is folded, not sign-mangled. */
    const float t = alchemy::Tri01(-0.75f);
    CHECK(t >= 0.0f && t <= 1.0f);
    CHECK(std::fabs(t - 0.5f) < 1e-4f);

    /* Uniform + Held is one level across the region, whatever the grain. */
    alchemy::FieldDesc u;
    u.pattern = alchemy::FieldPattern::Uniform;
    u.grain   = alchemy::FieldGrain::PerLed;
    u.step    = alchemy::FieldStep::Held;
    CHECK(alchemy::FieldEval(u, 0u, 5000u, -1.0f)
          == alchemy::FieldEval(u, 7u, 5000u, -1.0f));
}

} // namespace

int main()
{
    TestEdgeFill();
    TestEdgeFillCcw();
    TestCenterFill();
    TestCenterFillOffsetPivot();
    TestRegions();
    TestPipAddAndCarve();
    TestPipMotionAndSmooth();
    TestPipTrail();
    TestOverlayEmit();
    TestFieldDeterminism();
    TestFieldWavePhaseDrive();
    TestSignals();
    TestLegacyDrawFill();
    TestLegacyDrawSelector();
    TestLegacyDrawPip();
    TestCenterFillDefaultPivot();
    TestEmitSaturation();
    TestOverlayFieldIsNoOp();
    TestSelectorRegionFallbackZone();
    TestSelectorManyZones();
    TestPipZeroWidth();
    TestSignalEdgeCases();

    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all ring tests passed\n");
    return 0;
}
