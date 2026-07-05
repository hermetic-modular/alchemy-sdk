/**
 * clock_sync.cpp — Alchemy Lab minimal MusicalClock + ClockFollower demo.
 *
 * Reads an external Eurorack clock on CV-1 (24 PPQN), follows it with a
 * PI-PLL, and amplitude-modulates the stereo passthrough with a
 * beat-phase tremolo.  When the clock is locked, the tremolo is
 * sample-accurately phased to the incoming pulses; when it drops out,
 * `ClockLossPolicy::Freewheel` keeps the tremolo running through brief
 * dropouts.
 *
 * Patch / play:
 *   - In:   patch audio into the audio inputs
 *   - CV-1: patch an external clock (any 24 PPQN gate, 60–240 BPM)
 *   - P1:   tremolo Depth (CCW = bypass, CW = full)
 *   - P2:   beat Division — 1 / 2 / 4 / 8 cycles per beat
 *   - Out:  modulated audio
 *
 * This file is intentionally minimal: it demonstrates *only* the clock
 * wiring.  No Pager / Settings / Presets / ParamLock; one Page, two
 * Knobs.  See `kick.cpp` and `stereo_eq.cpp` for fully-loaded modules.
 *
 * SDK features OPTED IN:
 *   - alchemy::MusicalClock        free-running NCO timeline
 *   - alchemy::ClockFollower       PI-PLL steering the NCO from CV
 *   - alchemy::CvEdge              rising-edge detector on CV-1
 *   - ControlLoop + VirtualKnob    page surface
 *   - .OnPoll(fn)                  1 ms hook for the clock pipeline
 */

#include <math.h>

#include "daisy_seed.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/control/clock_follower.h"
#include "alchemy/control/cv_edge.h"
#include "alchemy/control/musical_clock.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/virtual_knob.h"

using namespace alchemy;

/* CV channel carrying the external clock (CV-1 on the V1 panel). */
static constexpr uint8_t kClockCvChan = 0u;

/* ── Knobs ─────────────────────────────────────────────────────────────── */

static VirtualKnob depth = VirtualKnob(0, "Depth")
    .Ring(Level({0x00, 0xC0, 0xFF}));

static VirtualKnob div_knob = VirtualKnob(1, "Division")
    .Selector(4u)
    .Ring(SelectorRing({0xFF, 0x80, 0x00},
                       {0x18, 0x10, 0x08},
                       4u,
                       ZoneGeometry::Region));

static Page main_page = Page(0).Knobs(depth, div_knob);

/* ── Hardware + clock primitives (statics so OnPoll can reach them) ────── */

static AlchemyLab      hw;
static ControlLoop     loop(hw);
static MusicalClock    clock_(/*ppqn=*/kClockPpqnInternal, /*beats_per_bar=*/4u);
static ClockFollower   follower(clock_);
static CvEdge          clk_edge;

/* ── 1 ms poll: edge intake → follower → NCO Tick (spec §7) ────────────── */

static void OnPoll(uint32_t /*t_ms*/)
{
    const uint32_t now_us = daisy::System::GetUs();

    /* CvEdge wants the *raw* CV buffer.  The same buffer the ControlLoop
     * built this frame is exposed via loop.Cv(); each entry is the
     * normalised reading where 0.5 = 0 V. */
    clk_edge.Tick(loop.Cv(), loop.NumCv(), now_us);
    if (clk_edge.JustRose(kClockCvChan))
        follower.OnPulse(clk_edge.LastRiseUs(kClockCvChan));

    follower.Update(now_us);
    clock_.Tick(now_us);
}

/* ── Audio: per-sample beat-phase tremolo ──────────────────────────────── */

static inline float DivisionMultiplier(int idx)
{
    /* 1 / 2 / 4 / 8 cycles per beat.  Out-of-range maps to 1. */
    switch (idx) { case 1: return 2.f; case 2: return 4.f; case 3: return 8.f; }
    return 1.f;
}

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out, size_t n)
{
    /* Sample anchor + rate once per block; advance phase per sample.  This
     * is the pattern spec §10 prescribes for sub-block accuracy: the clock
     * supplies the anchor, the FX does the per-sample ramp. */
    const double tpu       = clock_.TicksPerUs();
    const double sr        = hw.SampleRate();
    const float  phase_inc =
        static_cast<float>(tpu * 1e6 / sr / static_cast<double>(clock_.Ppqn()));

    const float d   = depth.Norm();
    const float mul = DivisionMultiplier(static_cast<int>(div_knob.Value()));
    float       p   = clock_.BeatPhase();

    for (size_t i = 0; i < n; ++i)
    {
        /* env: 0 at p=0 (beat boundary), 1 at p=0.5 (half-beat), smooth. */
        const float env  = 0.5f + 0.5f * cosf(2.f * 3.14159265f * (p * mul + 0.5f));
        const float gain = 1.f - d * (1.f - env);
        out[0][i] = in[0][i] * gain;
        out[1][i] = in[1][i] * gain;

        p += phase_inc;
        if (p >= 1.f) p -= 1.f;
    }
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main()
{
    hw.Init();

    clk_edge.Init(/*num_channels=*/static_cast<uint8_t>(kClockCvChan + 1u));

    /* Default Freewheel keeps the tremolo running through brief dropouts;
     * a sequencer-style module would pick Freeze or Stop. */
    follower.SetLossPolicy(ClockLossPolicy::Freewheel);
    follower.SetAutoStart(true);
    follower.Enable(kClockExtPpqnDefault);   /* 24 PPQN */

    hw.StartAudio(AudioCallback);

    loop.Use(main_page)
        .OnPoll(OnPoll);

    for (;;) loop.Tick();
}
