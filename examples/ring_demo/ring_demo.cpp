/**
 * ring_demo.cpp — Alchemy Lab ring composition demo.
 *
 * A stereo tremolo whose two rings render the LFO itself, composed from
 * the primitives in docs/ring-animations.md:
 *
 *   - P1 (Rate): declarative dim Level fill for the knob value, with an
 *     Overdraw layering a comet Pip on top — position driven by the
 *     LFO's actual phase, so the comet bounces at the audible rate.
 *     The overdraw re-draws the fill into its own frame (composition
 *     works on frame contents, not the panel) before adding the comet.
 *   - P2 (Depth): fully custom ring (Custom).  Level fill for the knob
 *     value, a Carve pip sweeping a notch through the lit region at the
 *     LFO phase, and a Shimmer field whose amount follows the depth.
 *
 * Patch / play:
 *   - In:   patch audio into the audio inputs
 *   - P1:   tremolo rate, 0.3–8 Hz
 *   - P2:   tremolo depth (CCW = bypass)
 *   - Out:  modulated audio
 *
 * The audio callback owns the LFO and publishes its phase once per block
 * (volatile single-word store); the ring callbacks read it on the main
 * thread.  Rings driven by published DSP state move exactly when the
 * audio does — this is the pattern to copy for signal-driven animation.
 *
 * SDK features OPTED IN:
 *   - RingFrame + Pip / Field     ring composition
 *   - VirtualKnob Custom / Overdraw   per-ring render hooks
 *   - ControlLoop + VirtualKnob   page surface
 */

#include <math.h>

#include "daisy_seed.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/led/ring_frame.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/virtual_knob.h"

using namespace alchemy;

static constexpr float kRateMinHz = 0.3f;
static constexpr float kRateMaxHz = 8.0f;

static constexpr LedPanel::Rgb kRateColor  = {0x00, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kDepthColor = {0xFF, 0x70, 0x00};

/* LFO phase, published by the audio callback once per block. */
static volatile float g_lfo_phase01 = 0.0f;

/* ── Ring callbacks ────────────────────────────────────────────────────── */

/* Rate ring: comet layered over the value fill.  A frame composes
 * against its own contents — it cannot read the panel back — so the
 * overlay re-draws the dim fill the comet adds over.  The saw phase is
 * folded by PipMotion::Bounce, so the comet ping-pongs end to end once
 * per LFO cycle. */
static void RateOverdraw(LedPanel& panel, uint8_t pot,
                         const ArcGeometry& geo, float norm,
                         uint32_t t_ms, void* /*ctx*/)
{
    RingFrame f;
    f.BeginOverlay(geo);

    FillDesc fill;
    fill.color = LedPanel::Scale(kRateColor, 0.35f);
    f.Base(fill, norm, t_ms);

    PipDesc comet;
    comet.color          = kRateColor;
    comet.compose        = PipCompose::Add;
    comet.motion         = PipMotion::Bounce;
    comet.smooth         = true;
    comet.tail_intensity = 0.25f;
    f.Pip(Region::Full, comet, 2.0f * g_lfo_phase01);

    f.Emit(panel, pot);
}

/* Depth ring: full takeover.  Fill = knob value; a notch carved at the
 * LFO phase and a shimmer scaled by depth make the ring show what the
 * tremolo is doing to the signal. */
static void DepthRing(LedPanel& panel, uint8_t pot,
                      const ArcGeometry& geo, float norm,
                      uint32_t t_ms, void* /*ctx*/)
{
    RingFrame f;
    f.Begin(geo);

    FillDesc fill;
    fill.color = kDepthColor;
    f.Base(fill, norm, t_ms);

    PipDesc notch;
    notch.compose = PipCompose::Carve;
    notch.motion  = PipMotion::Bounce;
    notch.smooth  = true;
    f.Pip(Region::Active, notch, 2.0f * g_lfo_phase01, norm);

    f.Field(Region::Active, FieldShimmer(70u), 0.3f * norm, t_ms);

    f.Emit(panel, pot);
}

/* ── Knobs ─────────────────────────────────────────────────────────────── */

static VirtualKnob rate = VirtualKnob(0, "Rate")
    .Ring(Level(LedPanel::Scale(kRateColor, 0.35f)))
    .Overdraw(RateOverdraw);

static VirtualKnob depth = VirtualKnob(1, "Depth")
    .Ring(Custom(DepthRing));

static Page main_page = Page(0).Knobs(rate, depth);

/* ── Hardware + audio ──────────────────────────────────────────────────── */

static AlchemyLab  hw;
static ControlLoop loop(hw);

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out, size_t n)
{
    static float phase = 0.0f;

    const float rate_hz = kRateMinHz
        + (kRateMaxHz - kRateMinHz) * rate.Norm() * rate.Norm();
    const float inc = rate_hz / static_cast<float>(hw.SampleRate());
    const float d   = depth.Norm();

    for (size_t i = 0; i < n; ++i)
    {
        const float env  = 0.5f + 0.5f * sinf(2.f * 3.14159265f * phase);
        const float gain = 1.f - d * env;
        out[0][i] = in[0][i] * gain;
        out[1][i] = in[1][i] * gain;

        phase += inc;
        if (phase >= 1.f) phase -= 1.f;
    }

    g_lfo_phase01 = phase;
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main()
{
    hw.Init();
    hw.StartAudio(AudioCallback);
    loop.Use(main_page);

    while (true)
        loop.Tick();
}
