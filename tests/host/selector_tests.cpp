/** Regression tests for selector value, color, and LED agreement. */
#include "selector_tests.h"
#include "alchemy/led/perf_renderer.h"
#include "alchemy/led/ring_frame.h"
#include "alchemy/surface/virtual_knob.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace alchemy;

namespace {
int checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

constexpr LedRingLayout ring[] = {{0u, 16u, 7.5f, 0.75f}};
constexpr HardwareLayout layout{1u, 0u, 0u, 16u, ring, nullptr};
constexpr ArcGeometry geo{7.5f, 0.75f, 13u};
constexpr uint8_t positions8[] = {0, 2, 3, 5, 7, 9, 10, 12};
constexpr uint8_t positions16[] = {0, 1, 2, 2, 3, 4, 5, 6, 6, 7, 8, 9, 10, 10, 11, 12};

struct Capture : ILedStrip
{
    LedPanel::Rgb pixels[16] = {};
    void SetPixel(uint16_t i, uint8_t r, uint8_t g, uint8_t b) override
    { if (i < 16u) pixels[i] = {r, g, b}; }
    void Clear() override { for (auto& p : pixels) p = {0, 0, 0}; }
    void Show() override {}
    bool Busy() const override { return false; }
    uint16_t NumLeds() const override { return 16u; }
    void ExpectOnly(uint8_t selected, LedPanel::Rgb color) const
    {
        for (uint8_t i = 0; i < 16u; ++i)
        {
            const auto expected = i == selected ? color : LedPanel::Rgb{0, 0, 0};
            CHECK(pixels[i].r == expected.r);
            CHECK(pixels[i].g == expected.g);
            CHECK(pixels[i].b == expected.b);
        }
    }
};

struct RenderRig
{
    Capture strip;
    LedPanel panel;
    PerfRenderer renderer{geo};
    RenderRig() { panel.Init(strip, layout); panel.SetBrightness(1.0f); }
    void Render(const VirtualKnob& knob)
    {
        strip.Clear();
        const PotState pot{knob.Norm(), true, 0};
        const float combined = knob.Norm();
        renderer.Render(panel, &knob.Slot(), &pot, &combined, 1u, 0u);
    }
};

void TestPerformanceSelectors()
{
    RenderRig rig;
    float value = 0.1f;
    VirtualKnob knob(0);
    knob.Selector(8).Ring(SelectorRing({200, 0, 0}, {0, 0, 0}, 8));
    knob.AttachPhys(&value, 1);

    auto check = [&](float input, uint8_t zone, uint8_t led) {
        value = input;
        CHECK(knob.Value() == zone);
        rig.Render(knob);
        rig.strip.ExpectOnly(led, {200, 0, 0});
    };
    // Issue #47: the old renderer jumped to zone 1 at 0.1, value stayed 0.
    check(0.1f, 0, 0);
    for (auto geometry : {ZoneGeometry::Distributed, ZoneGeometry::Point,
                          ZoneGeometry::Region})
    {
        knob.Ring(SelectorRing({200, 0, 0}, {0, 0, 0}, 8, geometry));
        for (uint8_t z = 0; z < 8u; ++z)
        {
            const auto led = geometry == ZoneGeometry::Point ? z : positions8[z];
            check(z / 8.0f, z, led);
            check((z + 0.5f) / 8.0f, z, led);
            check(std::nextafter((z + 1.0f) / 8.0f, 0.0f), z, led);
        }
        check(1.0f, 7, geometry == ZoneGeometry::Point ? 7 : 12);
    }

    // The transform owns the count, whichever order the builder is used.
    knob.Ring(SelectorRing({200, 0, 0}, {0, 0, 0}, 3)).Selector(8);
    check(0.4f, 3, 5);
    knob.Selector(8).Ring(SelectorRing({200, 0, 0}, {0, 0, 0}, 3));
    check(0.4f, 3, 5);

    // More zones than LEDs: inactive neighbors must not erase selection.
    knob.Selector(16).Ring(SelectorRing({200, 0, 0}, {0, 0, 0}, 16));
    for (uint8_t z = 0; z < 16u; ++z)
        check((z + 0.5f) / 16.0f, z, positions16[z]);

    knob.Selector(0);
    check(0.0f, 0, 0);
    check(1.0f, 0, 0);
    knob.Selector(8);
    check(-1.0f, 0, 0);
    check(2.0f, 7, 12);
    check(std::numeric_limits<float>::quiet_NaN(), 0, 0);
    check(std::numeric_limits<float>::infinity(), 7, 12);
    knob.Selector(255);
    value = 1.0f;
    CHECK(knob.Value() == 254.0f);

    // The complete post-CV normalized value drives both value and LED.
    knob.Selector(8).Cv(0, 1.0f, true);
    value = 0.1f;
    float cv = 0.75f;
    knob.AttachCv(&cv, 1);
    CHECK(knob.Value() == 2.0f);
    rig.Render(knob);
    rig.strip.ExpectOnly(3, {200, 0, 0});
}

void TestLegacyRendering()
{
    RenderRig rig;
    float value = 0.1f;
    VirtualKnob knob(0);
    knob.AttachPhys(&value, 1);
    knob.Ring(SelectorRing({200, 0, 0}, {0, 0, 0}, 8));
    rig.Render(knob);
    rig.strip.ExpectOnly(2, {200, 0, 0}); // nearest-position legacy behavior
    knob.Selector(8).Linear(0.0f, 1.0f);
    rig.Render(knob);
    rig.strip.ExpectOnly(2, {200, 0, 0});
    knob.Selector(8).Exp(1.0f, 10.0f);
    rig.Render(knob);
    rig.strip.ExpectOnly(2, {200, 0, 0});

    // Reusing a frame must not repaint a previously selected, now masked zone.
    rig.strip.Clear();
    RingFrame frame;
    frame.Begin(geo);
    SelectorDesc desc;
    desc.num_zones = 16;
    frame.BaseZone(desc, 8);
    desc.avail_mask = 0;
    frame.BaseZone(desc, 8);
    frame.Emit(rig.panel, 0);
    rig.strip.ExpectOnly(0, {0, 0, 0});
}

} // namespace

void RunSelectorTests(int& total_checks, int& total_failures)
{
    checks = failures = 0;
    TestPerformanceSelectors();
    TestLegacyRendering();
    total_checks += checks;
    total_failures += failures;
}
