/**
 * @file clip_indicator_test.cpp
 * @brief Host tests for ClipIndicator detection, suppression, hold, and
 *        pip drawing.
 */

#include "alchemy/led/anims/clip_indicator.h"
#include "alchemy/led/panel.h"

#include <cmath>
#include <cstdio>

namespace {

/* ── Capture rig: 2 rings × 4 LEDs + 2 buttons (chain 8..11) ─────────── */

constexpr uint16_t kLeds = 12u;

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
        for (uint16_t i = 0; i < kLeds; i++) { r[i] = g[i] = b[i] = 0u; }
    }
    void     Show() override {}
    bool     Busy() const override { return false; }
    uint16_t NumLeds() const override { return kLeds; }
};

/* Rings advance 3 hours per LED from noon: offsets 0..3 sit at hours
 * 0, 3, 6, 9 — so the default 6-o'clock pip is ring offset 2. */
constexpr alchemy::LedRingLayout kRings[] = {
    {0u, 4u, 0.0f, 3.0f},
    {4u, 4u, 0.0f, 3.0f},
};
constexpr alchemy::LedButtonLayout kButtons[] = {
    {8u, 9u},    /* button 0: bottom, top */
    {10u, 11u},  /* button 1: bottom, top */
};
constexpr alchemy::HardwareLayout kLayout = {
    2u,        /* pots */
    0u,        /* cv   */
    2u,        /* btns */
    kLeds,
    kRings,
    kButtons,
};

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

/* Feed one block containing @p clipped full-scale samples padded with
 * silence, then advance one 1 ms tick. */
void FeedAndTick(alchemy::ClipIndicator& ci, uint32_t clipped)
{
    float block[64];
    for (uint32_t i = 0; i < 64u; i++)
        block[i] = (i < clipped) ? 1.0f : 0.0f;
    ci.Process(block, 64u);
    ci.Tick(1.0f);
}

void TickSilence(alchemy::ClipIndicator& ci, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) ci.Tick(1.0f);
}

/* ── Detection ───────────────────────────────────────────────────────── */

void TestSingleSampleSuppressed()
{
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 1u);
    CHECK(!ci.Lit());
    TickSilence(ci, 100u);
    CHECK(!ci.Lit());
}

void TestBurstFires()
{
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 8u);   /* default min_clipped_samples in one block */
    CHECK(ci.Lit());
}

void TestThreshold()
{
    alchemy::ClipIndicator ci;

    /* 0.97 sits below the default 0.98 threshold — never counted. */
    float low[64];
    for (uint32_t i = 0; i < 64u; i++) low[i] = (i & 1u) ? -0.97f : 0.97f;
    for (uint32_t t = 0; t < 20u; t++) { ci.Process(low, 64u); ci.Tick(1.0f); }
    CHECK(!ci.Lit());

    /* 0.985 counts, negative samples included. */
    float hi[8];
    for (uint32_t i = 0; i < 8u; i++) hi[i] = (i & 1u) ? -0.985f : 0.985f;
    ci.Process(hi, 8u);
    ci.Tick(1.0f);
    CHECK(ci.Lit());
}

void TestSparseAccumulation()
{
    /* 2 clipped samples per ms beats the default leak (8 per 50 ms):
     * sustained crest-clipping accumulates to the trigger in a few ms. */
    alchemy::ClipIndicator ci;
    for (uint32_t t = 0; t < 10u; t++) FeedAndTick(ci, 2u);
    CHECK(ci.Lit());
}

void TestWindowDecay()
{
    /* Two 4-sample grazes further apart than the window never sum to
     * the trigger. */
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 4u);
    CHECK(!ci.Lit());
    TickSilence(ci, 60u);
    FeedAndTick(ci, 4u);
    CHECK(!ci.Lit());
}

void TestHoldDuration()
{
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 8u);
    CHECK(ci.Lit());

    /* Default hold: lit through 49 ms of silence, dark at 50. */
    TickSilence(ci, 49u);
    CHECK(ci.Lit());
    TickSilence(ci, 1u);
    CHECK(!ci.Lit());
}

void TestRetriggerExtends()
{
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 8u);
    TickSilence(ci, 40u);
    FeedAndTick(ci, 8u);   /* re-fire near the end of the hold */
    TickSilence(ci, 40u);
    CHECK(ci.Lit());       /* still inside the refreshed hold */
}

void TestSustainedStaysLit()
{
    alchemy::ClipIndicator ci;
    for (uint32_t t = 0; t < 200u; t++)
    {
        FeedAndTick(ci, 16u);
        if (t > 10u) CHECK(ci.Lit());
    }
}

void TestDerivedStrideProbing()
{
    /* Default Suppression(8) derives stride 4 — the probe grid is
     * indices 0, 4, 8; clipped samples that sit entirely off-grid are
     * never seen. */
    {
        alchemy::ClipIndicator ci;
        float block[12] = {};
        const uint32_t off_grid[8] = {1u, 2u, 3u, 5u, 6u, 7u, 9u, 10u};
        for (uint32_t i = 0; i < 8u; i++) block[off_grid[i]] = 1.0f;
        ci.Process(block, 12u);
        ci.Tick(1.0f);
        CHECK(!ci.Lit());
    }

    /* Suppression below 4 derives stride 1 — the scan is exact, and
     * off-grid positions no longer exist. */
    {
        alchemy::ClipIndicator ci = alchemy::ClipIndicator().Suppression(2u);
        float block[8] = {};
        block[1] = 1.0f;
        block[2] = -1.0f;
        ci.Process(block, 8u);
        ci.Tick(1.0f);
        CHECK(ci.Lit());
    }
}

void TestDerivedStrideRelaxed()
{
    /* Suppression(32) derives stride 16: two grid hits reach the
     * trigger, and a lone 8-sample burst (one hit = 16) cannot —
     * the lone-burst contract holds at every derived stride. */
    {
        alchemy::ClipIndicator ci = alchemy::ClipIndicator().Suppression(32u);
        float block[32] = {};
        block[0]  = 1.0f;
        block[16] = -1.0f;
        ci.Process(block, 32u);
        ci.Tick(1.0f);
        CHECK(ci.Lit());
    }
    {
        alchemy::ClipIndicator ci = alchemy::ClipIndicator().Suppression(32u);
        float block[32] = {};
        for (uint32_t i = 0; i < 8u; i++) block[i] = 1.0f;
        ci.Process(block, 32u);
        ci.Tick(1.0f);
        CHECK(!ci.Lit());
    }
}

void TestStrideScalesCounts()
{
    /* Each on-grid hit stands for `stride` samples, so the default
     * trigger (8) needs two hits at stride 4 — one is not enough. */
    {
        alchemy::ClipIndicator ci;
        float block[8] = {};
        block[0] = 1.0f;
        block[4] = -1.0f;
        ci.Process(block, 8u);
        ci.Tick(1.0f);
        CHECK(ci.Lit());
    }
    {
        alchemy::ClipIndicator ci;
        float block[8] = {};
        block[0] = 1.0f;
        ci.Process(block, 8u);
        ci.Tick(1.0f);
        CHECK(!ci.Lit());
    }
}

void TestNonFiniteCountsAsClipped()
{
    /* NaN and Inf order above every finite threshold in the bit-pattern
     * compare — a broken signal lights the alarm by design. */
    {
        alchemy::ClipIndicator ci;
        float inf[8];
        for (uint32_t i = 0; i < 8u; i++)
            inf[i] = (i & 1u) ? -INFINITY : INFINITY;
        ci.Process(inf, 8u);
        ci.Tick(1.0f);
        CHECK(ci.Lit());
    }
    {
        alchemy::ClipIndicator ci;
        float nan[8];
        for (uint32_t i = 0; i < 8u; i++) nan[i] = std::nanf("");
        ci.Process(nan, 8u);
        ci.Tick(1.0f);
        CHECK(ci.Lit());
    }
}

void TestMultiChannelProcess()
{
    alchemy::ClipIndicator ci;
    float        l[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float        r[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    const float* ch[2] = {l, r};
    ci.Process(ch, 2u, 4u);   /* 4 + 4 = 8 across channels */
    ci.Tick(1.0f);
    CHECK(ci.Lit());
}

/* ── Fluent configuration ────────────────────────────────────────────── */

void TestSuppressionAndHoldSetters()
{
    alchemy::ClipIndicator ci = alchemy::ClipIndicator()
                                    .Suppression(1u)   /* no suppression */
                                    .Hold(10.0f);

    FeedAndTick(ci, 1u);
    CHECK(ci.Lit());
    TickSilence(ci, 9u);
    CHECK(ci.Lit());
    TickSilence(ci, 1u);
    CHECK(!ci.Lit());
}

void TestSetterClamps()
{
    /* Suppression(0, 0) clamps to 1 sample / 1 ms; Hold(-5) clamps to 0
     * — asserted behaviorally: any clipped sample fires, and the light
     * is lit only on the trigger tick itself. */
    alchemy::ClipIndicator ci = alchemy::ClipIndicator()
                                    .Suppression(0u, 0.0f)
                                    .Hold(-5.0f);

    FeedAndTick(ci, 1u);
    CHECK(ci.Lit());
    TickSilence(ci, 1u);
    CHECK(!ci.Lit());
}

void TestChainInitCopies()
{
    /* VirtualKnob-style static-init chain: the copy at the end of the
     * chain must carry every setting across. */
    Rig rig;
    alchemy::ClipIndicator ci = alchemy::ClipIndicator()
                                    .Threshold(0.5f)
                                    .Suppression(4u)
                                    .Hold(20.0f)
                                    .Pots(1u)
                                    .Color({0x12, 0x34, 0x56});

    float block[4] = {0.6f, -0.6f, 0.6f, -0.6f};   /* clips only at 0.5 */
    ci.Process(block, 4u);
    ci.Tick(1.0f);
    CHECK(ci.Lit());

    ci.Draw(rig.panel);                  /* configured color, ring 1 only */
    CHECK(rig.strip.r[6] == 0x12u);
    CHECK(rig.strip.g[6] == 0x34u);
    CHECK(rig.strip.b[6] == 0x56u);
    CHECK(rig.strip.r[2] == 0x00u);      /* ring 0 untouched */

    TickSilence(ci, 19u);
    CHECK(ci.Lit());
    TickSilence(ci, 1u);
    CHECK(!ci.Lit());
}

void TestDisable()
{
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 8u);
    CHECK(ci.Lit());

    ci.SetEnabled(false);
    CHECK(!ci.Lit());          /* disable drops the hold immediately */
    FeedAndTick(ci, 32u);
    CHECK(!ci.Lit());          /* counting is off */

    ci.SetEnabled(true);
    CHECK(!ci.Lit());          /* re-enable starts clean */
    FeedAndTick(ci, 8u);
    CHECK(ci.Lit());
}

void TestReset()
{
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 8u);
    CHECK(ci.Lit());
    ci.Reset();
    CHECK(!ci.Lit());

    /* Samples fed before the Reset don't leak into the next Tick. */
    float block[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    ci.Process(block, 8u);
    ci.Reset();
    ci.Tick(1.0f);
    CHECK(!ci.Lit());
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

void TestDrawMasks()
{
    Rig rig;
    alchemy::ClipIndicator ci = alchemy::ClipIndicator()
                                    .Pots(0u)      /* ring 0 only   */
                                    .Buttons(1u);  /* button 1 only */

    FeedAndTick(ci, 8u);
    CHECK(ci.Lit());
    ci.Draw(rig.panel, {0xFF, 0x00, 0x00});

    CHECK(rig.strip.r[2]  == 0xFFu);  /* ring 0, 6-o'clock pip     */
    CHECK(rig.strip.g[2]  == 0x00u);
    CHECK(rig.strip.r[6]  == 0x00u);  /* ring 1 untouched          */
    CHECK(rig.strip.r[8]  == 0x00u);  /* button 0 untouched        */
    CHECK(rig.strip.r[9]  == 0x00u);
    CHECK(rig.strip.r[10] == 0xFFu);  /* button 1 pair lit         */
    CHECK(rig.strip.r[11] == 0xFFu);
}

void TestDrawButtonAccentSelection()
{
    Rig rig;
    alchemy::ClipIndicator ci = alchemy::ClipIndicator()
                                    .Pots()        /* no pot pips */
                                    .Buttons(0u)
                                    .Accent(alchemy::ClipIndicator::ButtonAccent::Bottom);

    FeedAndTick(ci, 8u);
    ci.Draw(rig.panel, {0x00, 0xFF, 0x00});

    CHECK(rig.strip.g[8] == 0xFFu);   /* bottom accent only */
    CHECK(rig.strip.g[9] == 0x00u);
    for (uint16_t i = 0; i < 8u; i++)
        CHECK(rig.strip.g[i] == 0x00u);   /* Pots() emptied the rings */
}

void TestDrawDefaults()
{
    /* Stock instance: every ring's pip, default red, no buttons. */
    Rig rig;
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 8u);
    ci.Draw(rig.panel);

    CHECK(rig.strip.r[2]  == 0xFFu);
    CHECK(rig.strip.r[6]  == 0xFFu);
    CHECK(rig.strip.g[2]  == 0x00u);
    CHECK(rig.strip.r[8]  == 0x00u);
    CHECK(rig.strip.r[10] == 0x00u);
}

void TestDrawUnlitIsNoOp()
{
    Rig rig;
    alchemy::ClipIndicator ci;
    ci.Draw(rig.panel, {0xFF, 0x00, 0x00});
    for (uint16_t i = 0; i < kLeds; i++)
        CHECK(rig.strip.r[i] == 0x00u);
}

void TestDrawScalesWithBrightness()
{
    Rig rig;
    rig.panel.SetBrightness(0.5f);
    alchemy::ClipIndicator ci;
    FeedAndTick(ci, 8u);
    ci.Draw(rig.panel, {0xFF, 0x00, 0x00});
    CHECK(rig.strip.r[2] == 0x80u);   /* 255 × 0.5 + 0.5 rounding */
}

} // namespace

int main()
{
    TestSingleSampleSuppressed();
    TestBurstFires();
    TestThreshold();
    TestSparseAccumulation();
    TestWindowDecay();
    TestHoldDuration();
    TestRetriggerExtends();
    TestSustainedStaysLit();
    TestDerivedStrideProbing();
    TestDerivedStrideRelaxed();
    TestStrideScalesCounts();
    TestNonFiniteCountsAsClipped();
    TestMultiChannelProcess();
    TestSuppressionAndHoldSetters();
    TestSetterClamps();
    TestChainInitCopies();
    TestDisable();
    TestReset();
    TestDrawMasks();
    TestDrawButtonAccentSelection();
    TestDrawDefaults();
    TestDrawUnlitIsNoOp();
    TestDrawScalesWithBrightness();

    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all clip indicator tests passed\n");
    return 0;
}
