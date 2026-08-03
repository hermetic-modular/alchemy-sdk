/**
 * hostlink.cpp — manage this module's presets and settings from a
 * browser, live over the front-panel USB-C, while the module runs.
 *
 * The whole integration is one declaration and one attach:
 *
 *   static hostlink::Host host(presets, "hostlink_demo", ...);
 *   ...
 *   loop.Use(host);
 *
 * Everything else is derived or defaulted.  The web editor's layout
 * comes from the same objects Presets serializes — knob names, value
 * transforms (Exp/Linear/Selector become display hints), page tab
 * names/colors, settings controls — so it can never drift from the
 * firmware.  Transport (CDC on the panel USB-C), buffers, the MCU
 * unique id, and reboot handling are SDK defaults; hostlink::Host has
 * a fluent override for each, and OnDescribe()/Descriptor() take over
 * descriptor generation entirely when you outgrow the derivation (see
 * alchemy/host_link/host.h).
 */

#include <cstring>

#include "daisy_seed.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/host.h"
#include "alchemy/surface/button_bank.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

using namespace alchemy;

/* ── Knobs ────────────────────────────────────────────────────────────
 * The optional host metadata rides the declarations you already write:
 * transforms become display hints, .Unit() labels the readout, .Ident()
 * pins a stable field id (default is positional).  Plain knobs need
 * nothing — they show up named, as a percent readout. */

static const char* kEngines[4] = {"Digi", "BBD", "Tape", "Vinyl"};

static constexpr LedPanel::Rgb kAlphaColor = {0x67, 0xE8, 0xF9};
static constexpr LedPanel::Rgb kBetaColor  = {0xFC, 0xA5, 0xA5};

static VirtualKnob a1 = VirtualKnob(0, "Drive")
    .Linear(0.f, 24.f).Unit("dB").Ident("drive")
    .Ring(Level(kAlphaColor));
static VirtualKnob a2 = VirtualKnob(1, "Cutoff")
    .Exp(20.f, 12000.f).Unit("Hz").Ident("flt.cutoff")
    .Ring(Level(kAlphaColor));
static VirtualKnob a3 = VirtualKnob(2, "Engine")
    .Selector(4).Labels(kEngines, 4)
    .Ring(Level(kAlphaColor));
static VirtualKnob a4 = VirtualKnob(3, "A 4").Ring(Level(kAlphaColor));
static VirtualKnob a5 = VirtualKnob(4, "A 5").Ring(Level(kAlphaColor));
static VirtualKnob a6 = VirtualKnob(5, "A 6").Ring(Level(kAlphaColor));

static VirtualKnob b1 = VirtualKnob(0, "B 1").Ring(Level(kBetaColor));
static VirtualKnob b2 = VirtualKnob(1, "B 2").Ring(Level(kBetaColor));
static VirtualKnob b3 = VirtualKnob(2, "B 3").Ring(Level(kBetaColor));
static VirtualKnob b4 = VirtualKnob(3, "B 4").Ring(Level(kBetaColor));
static VirtualKnob b5 = VirtualKnob(4, "B 5").Ring(Level(kBetaColor));
static VirtualKnob b6 = VirtualKnob(5, "B 6").Ring(Level(kBetaColor));

/* ── A button ─────────────────────────────────────────────────────────
 * One declaration covers the whole feature: B3 cycles the output
 * routing while page Alpha is visible, the zone persists in presets
 * (one byte, host-editable inside the Alpha page), the button LEDs wear
 * the zone color, and Bind() keeps the audio callback's variable in
 * sync on gesture and preset load alike.  Zones, labels, the tap
 * gesture, and its browser label all derive from the one array. */

static uint8_t s_route = 0;   /* 0 stereo, 1 swapped, 2 mono */

static const char* kRouteModes[3] = {"Stereo", "Swapped", "Mono"};
static constexpr LedPanel::Rgb kRouteColors[3] = {
    {0x67, 0xE8, 0xF9}, {0xFC, 0xA5, 0xA5}, {0xFF, 0xFF, 0xFF}};

static VirtualButton route = VirtualButton(kButtonB3, "Routing")
    .Ident("out.route")
    .Selector(kRouteModes)
    .Colors(kRouteColors)
    .Anchor("drive")
    .Bind(&s_route);

/* Same physical button, second page: on Beta, B3 toggles output phase
 * instead.  Each declaration is its own preset byte and browser field —
 * pages are pure dispatch scope. */

static uint8_t s_phase = 0;   /* 0 normal, 1 inverted */

static const char* kPhaseModes[2] = {"Normal", "Inverted"};
static constexpr LedPanel::Rgb kPhaseColors[2] = {
    {0xFC, 0xA5, 0xA5}, {0xB4, 0x3C, 0xFF}};

static VirtualButton phase = VirtualButton(kButtonB3, "Phase")
    .Ident("out.phase")
    .Selector(kPhaseModes)
    .Colors(kPhaseColors)
    .Bind(&s_phase);

/* Page names/colors label and tint the web editor's tabs (and match the
 * ring colors above, so panel and browser agree on page identity). */
static Page page_a = Page(0).Name("Alpha").Color("#67e8f9")
                            .Knobs(a1, a2, a3, a4, a5, a6)
                            .Buttons(route);
static Page page_b = Page(1).Name("Beta").Color("#fca5a5")
                            .Knobs(b1, b2, b3, b4, b5, b6)
                            .Buttons(phase);

/* ── A custom component ───────────────────────────────────────────────
 * App state outside the standard surfaces rides along by inheriting
 * Serializable — and one Describe() override makes it editable in the
 * browser too (skip it and the bytes still round-trip, shown as an
 * opaque blob).  Fields address the same bytes Serialize() writes. */

struct OutTrim : Serializable
{
    /* Normalized 0..1; the disp hint maps the readout to 0..2× gain. */
    float l = 0.5f;
    float r = 0.5f;

    size_t SerializedSize() const override { return 2u * sizeof(float); }
    void   Serialize(uint8_t* out) const override
    {
        std::memcpy(out + 0, &l, 4);
        std::memcpy(out + 4, &r, 4);
    }
    bool Deserialize(const uint8_t* in) override
    {
        std::memcpy(&l, in + 0, 4);
        std::memcpy(&r, in + 4, 4);
        return true;
    }
    uint32_t SchemaHash() const override { return 0x54524D31u; /* 'TRM1' */ }

    bool Describe(hostlink::ComponentWriter& w) const override
    {
        w.Label("Output Trim");
        bool ok = w.Field("trim.l", "Trim L", 0, hostlink::FieldType::F32,
                          0.5f, "{\"kind\":\"linear\",\"lo\":0,\"hi\":2}");
        ok &= w.Field("trim.r", "Trim R", 4, hostlink::FieldType::F32,
                      0.5f, "{\"kind\":\"linear\",\"lo\":0,\"hi\":2}");
        return ok;
    }
};

/* ── Surfaces ─────────────────────────────────────────────────────── */

static AlchemyLab  hw;
static ControlLoop loop    (hw);
static Pager       pager   (hw.buttons[0], 2, kNumPots);
static Presets     presets (hw.seed.qspi);
static Settings    settings(hw, &pager);
static OutTrim     trim;
static ButtonBank  buttons;

/* The host: module identity once, everything else defaulted.  The USB
 * product string defaults to the display name; Hermetic modules pass
 * .Product("Alchemy Lab") to enumerate under the platform name. */
static hostlink::Host host(presets, "hostlink_demo", "HostLink Demo",
                           "1.0.0", "example");

/* Audio: stereo passthrough through the host-editable output trim,
 * routed per the button's zone. */
static void Passthrough(daisy::AudioHandle::InputBuffer  in,
                        daisy::AudioHandle::OutputBuffer out,
                        size_t                           size)
{
    const float   sgn   = s_phase ? -2.f : 2.f;
    const float   gl    = sgn * trim.l;
    const float   gr    = sgn * trim.r;
    const uint8_t mode  = s_route;
    for (size_t i = 0; i < size; i++)
    {
        const float l = in[0][i] * gl;
        const float r = in[1][i] * gr;
        switch (mode)
        {
            case 1:  out[0][i] = r; out[1][i] = l; break;
            case 2:  out[0][i] = out[1][i] = 0.5f * (l + r); break;
            default: out[0][i] = l; out[1][i] = r; break;
        }
    }
}

int main()
{
    hw.Init();

    settings.UseBrightness();
    settings.UsePresets(presets);
    settings.Page(0).Name("Setup");

    /* Preset payload, walked in this order on Save/Load.  UseNames()
     * stores preset display names in the blob itself (renameable from
     * the browser) and stays pinned last automatically. */
    presets.Manage(pager);
    presets.Manage(trim);
    presets.Manage(buttons);
    presets.Manage(settings);
    presets.UseNames();

    loop.Use(pager)
        .Use(settings)
        .Use(page_a)
        .Use(page_b)
        .Use(buttons)
        .Use(host);

    presets.Init();
    presets.BootLoad();   /* the host starts here: descriptor + USB up */

    hw.StartAudio(Passthrough);

    for (;;) loop.Tick();
}
