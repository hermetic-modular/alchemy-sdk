/**
 * @file alchemy/led/ring_frame.h
 * @brief RingFrame: the compose canvas for pot-ring rendering.
 *
 * A ring rendering is a stack:
 *
 *   Ring = Base (exactly 1) + Overlays (0..N: Pip | Field) + catch pip
 *
 * driven by plain floats (knob values, DSP telemetry, time phases,
 * constants).  RingFrame is the canvas the stack composes into:
 *
 *   RingFrame f;
 *   f.Begin(geo);                       // own the ring (cleared)
 *   f.Base(fill, value);                // stamps the region map
 *   f.Field(Region::Active, shimmer, wet_env, t_ms);
 *   f.Pip(Region::Full, playhead, lag01);
 *   f.Emit(panel, pot, pot_state);      // quantize, write, catch pip last
 *
 * Compositing rules:
 *   - Accumulation is float RGB; one quantize + ScaleGlobal at Emit.
 *   - Rendering is call order.  Draw the Base first; the catch pip is
 *     painted by Emit and is always last.
 *   - Begin() owns the ring: Emit writes every LED (doubling as the
 *     clear).  BeginOverlay() layers over an existing render: Emit
 *     writes only the LEDs this frame touched.
 *   - Composition (Add / Carve pips, Fields) works against this frame's
 *     contents, not the panel — the frame cannot read pixels back.  In
 *     an overlay frame, Add over an untouched LED replaces it, and
 *     Carve / Field on untouched LEDs is a no-op.  Draw the layer you
 *     want modulated into the same frame.
 *
 * Regions are stamped by the Base and consumed by overlays.  All overlay
 * positions are region-relative 0..1 (0 = the region's CCW end), never
 * LED indices — compositions are portable across ring geometries.
 *
 * The legacy panel-direct DrawFill / DrawPip / DrawSelector functions are
 * thin wrappers over this class; there is one rendering core.
 */

#pragma once

#include <cstdint>

#include "alchemy/hardware_types.h"
#include "alchemy/led/panel.h"
#include "alchemy/led/anims/fill.h"
#include "alchemy/led/anims/pip.h"
#include "alchemy/led/anims/selector.h"
#include "alchemy/led/anims/field.h"

namespace alchemy {

struct PotState;

/**
 * Overlay target regions.  Stamped by the Base; a region the current
 * base cannot express resolves to empty, and overlays targeting it are
 * no-ops (never errors).
 */
enum class Region : uint8_t
{
    Full,       ///< The whole arc.  Always available.
    Active,     ///< The lit value region (fill / both fan arms / selected zone).
    Passive,    ///< Alias of PastTip — the unlit remainder of an Edge fill.
    PastTip,    ///< From the fill tip to the CW end (Edge fills).
    ArmCw,      ///< Lit CW arm of a Center fan.
    ArmCcw,     ///< Lit CCW arm of a Center fan.
    BottomPip,  ///< The off-arc bottom LED (hour 6).  Pip only.
    Span,       ///< The sub-span set by SetSpan().
};

class RingFrame
{
  public:
    static constexpr uint8_t kMaxLeds = 32u;

    /* ── Frame lifecycle ─────────────────────────────────────────────── */

    /** Own the ring: all LEDs cleared; Emit writes every LED. */
    void Begin(const ArcGeometry& geo);

    /** Layer over an existing render: Emit writes only touched LEDs. */
    void BeginOverlay(const ArcGeometry& geo);

    /* ── Base (exactly one; stamps the region map) ───────────────────── */

    /**
     * Fill base.  @p value01 is the control's 0..1 norm for both modes.
     * Center mode fans out from desc.pivot01 (same 0..1 space), each arm
     * normalized to its own side so both reach their ends at the pot
     * extremes.  desc.anim renders here so declarative Pulse/Ripple
     * styling needs no separate Field call.
     */
    void Base(const FillDesc& desc, float value01, uint32_t t_ms = 0u);

    /** Selector base; the zone is derived from @p value01. */
    void Base(const SelectorDesc& desc, float value01);

    /** Selector base with an explicit zone index — for callers that run
     *  their own zone map (hysteresis, custom boundaries). */
    void BaseZone(const SelectorDesc& desc, uint8_t zone);

    /* ── Overlays (0..N, call order) ─────────────────────────────────── */

    /**
     * Positioned element.  @p pos01 is region-relative (constant =
     * landmark, signal = motion); desc.motion maps it (Direct / Wrap /
     * Bounce) and desc.compose decides how it lands (Replace / Add /
     * Carve).  @p gain scales brightness (Carve: cut depth).
     */
    void Pip(Region region, const PipDesc& desc, float pos01,
             float gain = 1.0f, uint32_t t_ms = 0u);

    /**
     * Region-wide brightness modulation.  @p amount is the depth 0..1;
     * @p phase01 ≥ 0 drives Wave/Uniform motion from a signal instead of
     * wall time.
     */
    void Field(Region region, const FieldDesc& desc, float amount,
               uint32_t t_ms = 0u, float phase01 = -1.0f);

    /** Configure Region::Span (0..1 endpoints across the full arc). */
    void SetSpan(float a01, float b01);

    /* ── Raw access (bespoke bases and marks) ────────────────────────── */

    uint8_t NumLeds() const { return n_; }
    void Set(uint8_t k, const LedPanel::Rgb& c);
    void Add(uint8_t k, const LedPanel::Rgb& c, float gain = 1.0f);
    void Dim(uint8_t k, float gain);

    /** Stamp the Active region by hand (bespoke bases). */
    void MarkActive(int first_led, int last_led);

    /* ── Emit ────────────────────────────────────────────────────────── */

    void Emit(LedPanel& dst, uint8_t pot) const;

    /** Emit, then paint the catch pip from @p ps — always the top layer. */
    void Emit(LedPanel& dst, uint8_t pot, const PotState& ps,
              LedPanel::Rgb catch_color = {0xFF, 0xFF, 0xFF}) const;

  private:
    struct SpanLeds
    {
        int   first = -1;   ///< First member LED (-1 = empty).
        int   last  = -1;   ///< Last member LED.
        float a     = 0.0f; ///< Position-mapping CCW endpoint (LED units).
        float b     = 0.0f; ///< Position-mapping CW endpoint (LED units).
    };

    SpanLeds Resolve(Region region) const;
    void     BeginCommon(const ArcGeometry& geo);
    float    AnimGain(FillAnim anim, float depth, uint8_t step,
                      uint32_t t_ms) const;
    void     PaintTap(const PipDesc& desc, const SpanLeds& s, int k,
                      float w, bool has_bg);

    float    fr_[kMaxLeds] = {};
    float    fg_[kMaxLeds] = {};
    float    fb_[kMaxLeds] = {};
    uint32_t touched_      = 0u;
    bool     overlay_      = false;

    ArcGeometry geo_ = {0.0f, 0.0f, 0u};
    uint8_t     n_   = 0u;

    SpanLeds active_;
    SpanLeds arm_cw_;
    SpanLeds arm_ccw_;
    SpanLeds past_tip_;
    float    span_a01_ = 0.0f;
    float    span_b01_ = 1.0f;

    bool          bottom_set_ = false;
    LedPanel::Rgb bottom_     = {0u, 0u, 0u};
};

} // namespace alchemy
