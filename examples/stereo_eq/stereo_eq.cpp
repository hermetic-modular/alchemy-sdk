/**
 * stereo_eq.cpp — Alchemy Lab dual mono three-band EQ.
 * Demonstrates a large surface of the SDK.
 *
 * Pure DSP in *_dsp files implements:
 * - Three-band EQ per channel, ±24 dB per band
 * - Independent left/right channels
 *
 * This file implements:
 * - All the required hardware and audio callback management.
 * - Two pages of controls, switched by B1
 * - Pot-catch on page switch (no value jumps)
 * - Record looping automation by holding B1 and moving a pot
 * - Each pot modulated by its assigned CV input
 * - Animations for each LED ring.
 * - LED ring per pot displaying summed value.
 * - Save and recall presets with flash wear leveling.
 * - Settings menu for managing basics
 */

#include "daisy_seed.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/param_lock.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_knob.h"

#include "stereo_eq_dsp.h"
#include "stereo_eq_palette.h"

using namespace alchemy;

/* We define each knob's curve and LED Ring animation.  CV routing lives
 * in the CvMatrix declaration below; declaring it once at the matrix
 * level keeps the knob declarations purely about the knob.  */
static constexpr float kGainMaxDb = 24.f;

/* Page 1 */

static VirtualKnob l_hi_level = VirtualKnob(kPotTopLeft, "Hi Level")
    .Linear(-kGainMaxDb, +kGainMaxDb)
    .Ring(Bipolar(kLeftPalette.hi.level_pos,
                  kLeftPalette.hi.level_neg,
                  kLeftPalette.hi.level_center));

static VirtualKnob l_hi_freq = VirtualKnob(kPotTopRight, "Hi Freq")
    .Exp(1000.f, 16000.f)
    .Ring(Level(kLeftPalette.hi.freq, FillAnim::Pulse));

static VirtualKnob l_mid_level = VirtualKnob(kPotMiddleLeft, "Mid Level")
    .Linear(-kGainMaxDb, +kGainMaxDb)
    .Ring(Bipolar(kLeftPalette.mid.level_pos,
                  kLeftPalette.mid.level_neg,
                  kLeftPalette.mid.level_center));

static VirtualKnob l_mid_freq = VirtualKnob(kPotMiddleRight, "Mid Freq")
    .Exp(200.f, 5000.f)
    .Ring(Level(kLeftPalette.mid.freq, FillAnim::Ripple));

static VirtualKnob l_lo_level = VirtualKnob(kPotBottomLeft, "Lo Level")
    .Linear(-kGainMaxDb, +kGainMaxDb)
    .Ring(Bipolar(kLeftPalette.lo.level_pos,
                  kLeftPalette.lo.level_neg,
                  kLeftPalette.lo.level_center));

static VirtualKnob l_lo_freq = VirtualKnob(kPotBottomRight, "Lo Freq")
    .Exp(60.f, 600.f)
    .Ring(Level(kLeftPalette.lo.freq, FillAnim::Pulse));


/* Page 2 */
static VirtualKnob r_hi_level = VirtualKnob(kPotTopLeft, "Hi Level")
    .Linear(-kGainMaxDb, +kGainMaxDb)
    .Ring(Bipolar(kRightPalette.hi.level_pos,
                  kRightPalette.hi.level_neg,
                  kRightPalette.hi.level_center));

static VirtualKnob r_hi_freq = VirtualKnob(kPotTopRight, "Hi Freq")
    .Exp(1000.f, 16000.f)
    .Ring(Level(kRightPalette.hi.freq, FillAnim::Pulse));

static VirtualKnob r_mid_level = VirtualKnob(kPotMiddleLeft, "Mid Level")
    .Linear(-kGainMaxDb, +kGainMaxDb)
    .Ring(Bipolar(kRightPalette.mid.level_pos,
                  kRightPalette.mid.level_neg,
                  kRightPalette.mid.level_center));

static VirtualKnob r_mid_freq = VirtualKnob(kPotMiddleRight, "Mid Freq")
    .Exp(200.f, 5000.f)
    .Ring(Level(kRightPalette.mid.freq, FillAnim::Ripple));

static VirtualKnob r_lo_level = VirtualKnob(kPotBottomLeft, "Lo Level")
    .Linear(-kGainMaxDb, +kGainMaxDb)
    .Ring(Bipolar(kRightPalette.lo.level_pos,
                  kRightPalette.lo.level_neg,
                  kRightPalette.lo.level_center));

static VirtualKnob r_lo_freq = VirtualKnob(kPotBottomRight, "Lo Freq")
    .Exp(60.f, 600.f)
    .Ring(Level(kRightPalette.lo.freq, FillAnim::Pulse));

/* Bind knobs to page */
static Page left_page  = Page(0).Knobs(l_hi_level, l_hi_freq,
                                       l_mid_level, l_mid_freq,
                                       l_lo_level, l_lo_freq);
static Page right_page = Page(1).Knobs(r_hi_level, r_hi_freq,
                                       r_mid_level, r_mid_freq,
                                       r_lo_level, r_lo_freq);

/* Get our SDK surfaces and opt in to everything */
static AlchemyLab                        hw;
static ControlLoop                       loop    (hw);
static Pager                             pager   (hw.buttons[0], 2, kNumPots);
static ParamLock<2 * kNumPots, LockLength<30, 20>>
                                         locks   (hw.buttons[0], pager);
static Presets                           presets (hw.seed.qspi);
static Settings                          settings(hw, &pager);
static CvMatrix                          cv_matrix(kNumCvInputs);

/* summed CV+knob values → DSP each frame */
static constexpr float kMidQ      = 1.2f;
static void UpdateCoeffs()
{
    eq_dsp::SetChannel(0, {
        l_lo_freq.Value(),  l_lo_level.Value(),
        l_mid_freq.Value(), l_mid_level.Value(), kMidQ,
        l_hi_freq.Value(),  l_hi_level.Value(),
    });
    eq_dsp::SetChannel(1, {
        r_lo_freq.Value(),  r_lo_level.Value(),
        r_mid_freq.Value(), r_mid_level.Value(), kMidQ,
        r_hi_freq.Value(),  r_hi_level.Value(),
    });
}

int main()
{
    hw.Init();
    eq_dsp::Init(hw.SampleRate());

    /* CV routing.  A static layout is just setting each channel once. */
    cv_matrix.Jack(0).To(l_hi_level);
    cv_matrix.Jack(1).To(l_hi_freq);
    cv_matrix.Jack(2).To(l_mid_level);
    cv_matrix.Jack(3).To(l_mid_freq);
    cv_matrix.Jack(4).To(l_lo_level);
    cv_matrix.Jack(5).To(l_lo_freq);

    /* Opting into default settings gestures and controls.*/
    settings.UseBrightness();
    settings.UsePresets(presets);

    /* Preset payload — every Serializable surface gets walked on Save/Load. */
    presets.Manage(pager);
    presets.Manage(locks);
    presets.Manage(settings);
    presets.Init();
    presets.BootLoad();

    UpdateCoeffs();
    hw.StartAudio(eq_dsp::Process);

    /* ControlLoop is a thin, opt-in driver for the canonical control-rate frame.
     * If desired, you can unroll and modify. */
    loop.Use(pager)
        .Use(locks)
        .Use(settings)
        .Use(cv_matrix)
        .Use(left_page)
        .Use(right_page)
        .OnFrame(UpdateCoeffs);

    for (;;) loop.Tick();
}
