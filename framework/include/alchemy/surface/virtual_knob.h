/**
 * @file alchemy/surface/virtual_knob.h
 * @brief alchemy::VirtualKnob — declarative knob abstraction.
 *
 * A VirtualKnob bundles everything the user thinks of as "one knob" — its
 * identity, its value transform, its ring style, and its CV binding — into
 * a single declarative object with one accessor for reading the resolved
 * value.
 *
 * Why virtual then? Well, a knob isn't that. It's a bundle of a param,
 * animation, and more. It gets composed into pages. By "virtualizing"
 * we provide an easier to understand abstraction.
 *
 * Usage:
 *
 *   VirtualKnob hi_level = VirtualKnob(0, "Hi Level")
 *       .Linear(-24.f, +24.f)
 *       .Ring(Bipolar(blue_pos, blue_neg, blue_ctr))
 *       .Cv(3);
 *
 *   // After ControlLoop attaches storage/locks/cv:
 *   const float db = hi_level.Value();   // -24..+24
 *   const float n  = hi_level.Norm();    // 0..1 escape hatch
 *
 * Value(), Norm(), and the custom-ring callbacks are ISR-safe and cheap;
 * the audio callback may read them.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/hardware_types.h"        /* ArcGeometry */
#include "alchemy/led/anim_desc.h"         /* ParamSlot, ArcStyle, FillAnim */
#include "alchemy/led/panel.h"             /* LedPanel, LedPanel::Rgb */
#include "alchemy/surface/cv_source.h"
#include "alchemy/surface/knob_storage.h"
#include "alchemy/surface/lock_source.h"

namespace alchemy {

/* ── Ring style specs ─────────────────────────────────────────────────── */
/*
 * Each ring style is its own type with a constructor that takes exactly
 * the parameters that style requires. Wrong-arity / wrong-typed parameters
 * fail to compile. Each exposes Apply(ParamSlot&) which VirtualKnob uses to
 * populate the underlying renderer descriptor.
 * This could be made more elegant in future, possibly.
 */

struct Level
{
    LedPanel::Rgb color;
    FillAnim      anim;
    LedPanel::Rgb passive_color = {0x00, 0x00, 0x00};
    float         anim_depth    = 0.4f;

    explicit Level(LedPanel::Rgb c, FillAnim a = FillAnim::None)
        : color(c), anim(a) {}

    /** Color of the unfilled "other half" of the arc (default off). */
    Level& Passive(LedPanel::Rgb c) { passive_color = c; return *this; }

    /** Modulation depth for `anim` (default 0.4). */
    Level& Depth(float d) { anim_depth = d; return *this; }

    void Apply(ParamSlot& s) const
    {
        s.arc_style          = ArcStyle::Level;
        s.fill.color         = color;
        s.fill.anim          = anim;
        s.fill.passive_color = passive_color;
        s.fill.anim_depth    = anim_depth;
    }
};

struct Bipolar
{
    LedPanel::Rgb pos, neg, center;
    float         pivot = 0.5f;

    Bipolar(LedPanel::Rgb p, LedPanel::Rgb n, LedPanel::Rgb c)
        : pos(p), neg(n), center(c) {}

    /** Fan origin in the control's 0..1 value space (default: midpoint).
     *  Each arm is normalized to its own side, so both reach their ends
     *  at the pot extremes. */
    Bipolar& Pivot(float p) { pivot = p; return *this; }

    void Apply(ParamSlot& s) const
    {
        s.arc_style         = ArcStyle::Bipolar;
        s.fill.color        = pos;
        s.fill.neg_color    = neg;
        s.fill.center_color = center;
        s.fill.pivot01      = pivot;
    }
};

struct SelectorRing
{
    LedPanel::Rgb on, off;
    uint8_t       num_zones;
    ZoneGeometry  geo;

    SelectorRing(LedPanel::Rgb on_c, LedPanel::Rgb off_c, uint8_t n,
                 ZoneGeometry g = ZoneGeometry::Distributed)
        : on(on_c), off(off_c), num_zones(n), geo(g) {}

    void Apply(ParamSlot& s) const
    {
        s.arc_style               = ArcStyle::Selector;
        s.selector.active_color   = on;
        s.selector.inactive_color = off;
        s.selector.num_zones      = num_zones;
        s.selector.zone_geo       = geo;
    }
};

/**
 * Color-morphing filled arc.  The fill color interpolates between the
 * declared snap-point colors as the knob sweeps from 0 to 1; dim markers
 * sit at each snap position so the user can find them by feel.  As the
 * value approaches a snap, the fill saturates toward that snap's color.
 *
 * Caller owns the snap-point storage — pass a static-lifetime array.
 * Pair with `.Pip(SolidPip(...))` for a snap-confirmation pip if desired.
 *
 *   static constexpr MorphSnapPoint kEngines[4] = { {0.f,Digi}, {1.f/3,BBD},
 *                                                   {2.f/3,Tape}, {1.f,Vinyl} };
 *   VirtualKnob morph = VirtualKnob(0, "Engine").Ring(Gradient(kEngines, 4));
 */
struct Gradient
{
    const MorphSnapPoint* snaps;
    uint8_t               num_snaps;

    Gradient(const MorphSnapPoint* s, uint8_t n) : snaps(s), num_snaps(n) {}

    void Apply(ParamSlot& s) const
    {
        s.arc_style          = ArcStyle::Gradient;
        s.gradient.snaps     = snaps;
        s.gradient.num_snaps = num_snaps;
    }
};

/**
 * Plain Level fill whose color is sampled from a *sibling* pot's value
 * through the same snap-point gradient a `Gradient` arc uses.  Use for a
 * depth/amount control that should wear the color of its companion
 * morph/model control (e.g. delay-line depth tinted by that line's engine).
 *
 *   VirtualKnob depth = VirtualKnob(3, "Depth")
 *       .Ring(GradientFill(kEngines, 4, 2, kPageBlue));  // src pot = 2
 *
 * `src_pot` is the pot index whose resolved value selects the color;
 * `fallback` is used when that pot is off-page or no snaps are given.
 * Caller owns the snap-point storage — pass a static-lifetime array.
 */
struct GradientFill
{
    const MorphSnapPoint* snaps;
    uint8_t               num_snaps;
    uint8_t               src_pot;
    LedPanel::Rgb         fallback;

    GradientFill(const MorphSnapPoint* s, uint8_t n, uint8_t src,
                 LedPanel::Rgb fb)
        : snaps(s), num_snaps(n), src_pot(src), fallback(fb) {}

    void Apply(ParamSlot& s) const
    {
        s.arc_style              = ArcStyle::GradientFill;
        s.gradient.snaps         = snaps;
        s.gradient.num_snaps     = num_snaps;
        s.gradient.color_src_pot = src_pot;
        s.fill.color             = fallback;
    }
};

/* ── Pip style specs ──────────────────────────────────────────────────── */

struct SolidPip
{
    LedPanel::Rgb color;

    explicit SolidPip(LedPanel::Rgb c) : color(c) {}

    void Apply(ParamSlot& s) const
    {
        s.pip.style = PipStyle::Solid;
        s.pip.color = color;
    }
};

struct ThresholdSnapPip
{
    LedPanel::Rgb color, over_color;
    float         lo, hi;

    ThresholdSnapPip(LedPanel::Rgb c, float lo_, float hi_,
                     LedPanel::Rgb over = {0xFF, 0x00, 0x00})
        : color(c), over_color(over), lo(lo_), hi(hi_) {}

    void Apply(ParamSlot& s) const
    {
        s.pip.style         = PipStyle::ThresholdSnap;
        s.pip.color         = color;
        s.pip.snap_lo       = lo;
        s.pip.snap_hi       = hi;
        s.pip.snap_hi_color = over_color;
    }
};

/**
 * Snap-confirmation pip for a `Gradient` arc: lit in the morph fill color
 * when the value sits on a snap point and the pot is caught.  The color is
 * taken from the arc's own morph result at render time, so it always matches
 * the arc.  Pair with `.Ring(Gradient(...))`.
 */
struct GradientSnapPip
{
    void Apply(ParamSlot& s) const { s.pip.style = PipStyle::GradientSnap; }
};

/* ── Custom-ring callback ─────────────────────────────────────────────── */

/**
 * Custom / overdraw ring callback signature.
 *
 * @param panel   The LED panel to paint into.
 * @param pot     This knob's pot index (for the SetRingByHour family).
 * @param geo     Arc geometry for this board.
 * @param norm    Knob's resolved normalised value, 0..1.
 * @param t_ms    Frame timestamp (for animation phase).
 * @param ctx     Opaque context the user passed at .Ring(Custom(...)) /
 *                .Overdraw(...) registration.
 */
using RingFn = void(*)(LedPanel&          panel,
                       uint8_t            pot,
                       const ArcGeometry& geo,
                       float              norm,
                       uint32_t           t_ms,
                       void*              ctx);

struct Custom
{
    RingFn fn;
    void*  ctx;

    explicit Custom(RingFn f, void* c = nullptr) : fn(f), ctx(c) {}

    void Apply(ParamSlot& s) const
    {
        /* ArcStyle::None tells PerfRenderer to skip; the loop calls fn instead. */
        s.arc_style = ArcStyle::None;
    }
};

/* ── VirtualKnob ──────────────────────────────────────────────────────── */

class VirtualKnob
{
  public:
    enum class Xform : uint8_t { Linear, Exp, Selector };

    /**
     * Construct an unconfigured knob.
     *
     * @param pot   Physical pot index on the board.
     * @param name  Diagnostic name (e.g. "Hi Level"); not used in audio.
     */
    VirtualKnob(uint8_t pot = 0, const char* name = "")
        : pot_(pot), name_(name) {}

    /* ── Value transforms (one chosen; default Linear(0, 1)) ──────────── */

    VirtualKnob& Linear(float min, float max)
    {
        xform_     = Xform::Linear;
        xform_min_ = min;
        xform_max_ = max;
        return *this;
    }

    /** Geometric interpolation: value = min * (max/min)^norm. Requires min > 0. */
    VirtualKnob& Exp(float min, float max)
    {
        xform_     = Xform::Exp;
        xform_min_ = min;
        xform_max_ = max;
        return *this;
    }

    /** Discrete N-zone selector. .Value() returns an integer cast to float in [0, N). */
    VirtualKnob& Selector(uint8_t num_zones)
    {
        xform_     = Xform::Selector;
        xform_min_ = 0.f;
        xform_max_ = static_cast<float>(num_zones);
        return *this;
    }

    /* ── Ring style (optional; omit for no ring) ─────────────────────── */

    template <typename Spec>
    VirtualKnob& Ring(const Spec& spec)
    {
        spec.Apply(slot_);
        return *this;
    }

    /** Custom ring: replaces the SDK's declarative ring renderer for this knob. */
    VirtualKnob& Ring(const Custom& c)
    {
        c.Apply(slot_);
        custom_fn_  = c.fn;
        custom_ctx_ = c.ctx;
        return *this;
    }

    /**
     * Bottom-pip overlay (SolidPip or ThresholdSnapPip). Independent of the
     * arc style — paint a fixed indicator or threshold marker below any ring.
     */
    template <typename Spec>
    VirtualKnob& Pip(const Spec& spec)
    {
        spec.Apply(slot_);
        return *this;
    }

    /**
     * Paint @p fn on top of the SDK's declarative ring after PerfRenderer.
     * Same signature as Custom. Useful for envelope/meter overlays.
     */
    VirtualKnob& Overdraw(RingFn fn, void* ctx = nullptr)
    {
        overdraw_fn_  = fn;
        overdraw_ctx_ = ctx;
        return *this;
    }

    /* ── CV binding (optional; omit for no CV) ───────────────────────── */

    VirtualKnob& Cv(uint8_t cv_index, float attenuation = 1.0f, bool bipolar = true)
    {
        cv_index_   = static_cast<int8_t>(cv_index);
        cv_atten_   = attenuation;
        cv_bipolar_ = bipolar;
        return *this;
    }

    /* ── Host descriptor metadata (all optional; string literals) ────────
     * Consumed by HostLink's descriptor auto-derivation.  Without them a
     * knob still appears in the web editor — named after Name(), with a
     * display hint derived from its value transform and a positional
     * field id.  See alchemy/host_link/describe.h. */

    /** Stable field id (e.g. "flt.cutoff").  Defaults to the positional
     *  "p<page>.<pot>"; set one to keep host presets addressable when the
     *  knob later moves to a different pot. */
    VirtualKnob& Ident(const char* id) { ident_ = id; return *this; }

    /** Engineering unit shown next to the value (e.g. "Hz", "dB"). */
    VirtualKnob& Unit(const char* unit) { unit_ = unit; return *this; }

    /** Zone labels for a Selector knob (array of NumZones entries). */
    VirtualKnob& Labels(const char* const* labels, uint8_t n)
    {
        labels_     = labels;
        num_labels_ = n;
        return *this;
    }

    template <size_t N>
    VirtualKnob& Labels(const char* const (&labels)[N])
    {
        return Labels(labels, static_cast<uint8_t>(N));
    }

    /** Raw display-hint JSON (protocol §5); overrides the derived hint. */
    VirtualKnob& Disp(const char* disp_json) { disp_json_ = disp_json; return *this; }

    /* ── Read accessors (ISR-safe; cheap) ────────────────────────────── */

    /**
     * Raw normalised value, post-mix (catch + locks + CV), pre-transform.
     * Always in [0, 1]. The canonical escape hatch when the user needs a
     * range or transform that the built-in transforms don't express.
     */
    float Norm() const
    {
        float v = StoredValue() + LockDelta() + CvDelta();
        if (v < 0.f) v = 0.f;
        if (v > 1.f) v = 1.f;
        return v;
    }

    /** Transformed value in engineering units. */
    float Value() const
    {
        const float n = Norm();
        switch (xform_)
        {
            case Xform::Exp:      return ExpTransform(n);
            case Xform::Selector: return SelectorTransform(n);
            case Xform::Linear:   /* fall through */
            default:              return xform_min_ + n * (xform_max_ - xform_min_);
        }
    }

    /* ── Identity / config accessors ─────────────────────────────────── */

    uint8_t          Pot()  const { return pot_; }
    const char*      Name() const { return name_; }
    const ParamSlot& Slot() const { return slot_; }

    const char*        Ident()     const { return ident_; }
    const char*        Unit()      const { return unit_; }
    const char* const* Labels()    const { return labels_; }
    uint8_t            NumLabels() const { return num_labels_; }
    const char*        DispJson()  const { return disp_json_; }
    Xform              Transform() const { return xform_; }
    float              XformMin()  const { return xform_min_; }
    float              XformMax()  const { return xform_max_; }

    /* ── Wiring (called by ControlLoop at attach time) ───────────────── */

    /** Sentinel for AttachStorage's `pot` arg: keep the construction-time pot. */
    static constexpr uint8_t kKeepPot = 0xFFu;

    /**
     * Attach storage and assign this knob's (page, pot) slot for this frame.
     *
     * ControlLoop re-calls this every frame, so passing a different `page` or
     * `pot` moves the knob between slots at runtime — the documented
     * reassignment pattern (see page.h), now covering position as well as
     * page.  `pot == kKeepPot` keeps the construction-time pot, so existing
     * page-only callers are byte-identical.
     */
    void AttachStorage(KnobStorage* s, uint8_t page, uint8_t pot = kKeepPot)
    {
        storage_ = s;
        page_    = page;
        if (pot != kKeepPot) pot_ = pot;
    }
    void AttachLockSource(LockSource* l) { locks_ = l; }
    void AttachCv(const float* cv_buf, uint8_t num_cv)
    {
        cv_buf_ = cv_buf;
        num_cv_ = num_cv;
    }

    /**
     * Attach a CvSource that resolves CV deltas at the (page, pot) level.
     * When set, the static `.Cv(...)` binding is ignored — the source's
     * `DeltaAtPage(page, pot)` is the sole CV contribution.  Pass nullptr
     * to detach and fall back to the static binding.
     */
    void AttachCvSource(CvSource* s) { cv_source_ = s; }

    /**
     * Fallback for the stored value when no KnobStorage is attached.
     * Lets single-page modules skip Pager entirely — the knob reads the
     * physical pot position directly with no catch logic.
     */
    void AttachPhys(const float* phys_buf, uint8_t num_phys)
    {
        phys_buf_ = phys_buf;
        num_phys_ = num_phys;
    }

    /** Page index this knob was attached to (0 if unattached). */
    uint8_t Page() const { return page_; }

    /* ── Render hooks (called by ControlLoop) ────────────────────────── */

    bool   HasCustomRing()  const { return custom_fn_   != nullptr; }
    bool   HasOverdraw()    const { return overdraw_fn_ != nullptr; }
    RingFn CustomFn()       const { return custom_fn_; }
    void*  CustomCtx()      const { return custom_ctx_; }
    RingFn OverdrawFn()     const { return overdraw_fn_; }
    void*  OverdrawCtx()    const { return overdraw_ctx_; }

  private:
    /* Read helpers — all ISR-safe. */

    float StoredValue() const
    {
        if (storage_) return storage_->Stored(page_, pot_);
        if (phys_buf_ && pot_ < num_phys_) return phys_buf_[pot_];
        return 0.5f;
    }

    float LockDelta() const
    {
        return locks_ ? locks_->DeltaAtPage(page_, pot_) : 0.f;
    }

    float CvDelta() const
    {
        /* An attached CvSource owns the CV→knob mapping entirely.  It is
         * resolved at the (page, pot) level so a single CvSource can drive
         * a heterogeneous matrix of CV destinations (continuous params on
         * arbitrary pages, plus non-knob targets like clock taps that the
         * source dispatches itself). */
        if (cv_source_) return cv_source_->DeltaAtPage(page_, pot_);

        if (cv_index_ < 0 || !cv_buf_) return 0.f;
        if (static_cast<uint8_t>(cv_index_) >= num_cv_) return 0.f;
        const float raw = cv_buf_[cv_index_];
        const float v   = cv_bipolar_ ? (raw - 0.5f) : raw;
        return v * cv_atten_;
    }

    float ExpTransform(float n) const
    {
        /* min * (max/min)^n.  Guard against min <= 0 by falling back to linear. */
        if (xform_min_ <= 0.f) return xform_min_ + n * (xform_max_ - xform_min_);
        /* exp(n * ln(max/min)) = (max/min)^n */
        const float ratio = xform_max_ / xform_min_;
        return xform_min_ * Pow(ratio, n);
    }

    float SelectorTransform(float n) const
    {
        const float upper = xform_max_;            /* num_zones */
        float       z     = n * upper;
        if (z >= upper)         z = upper - 1.f;
        else if (z < 0.f)       z = 0.f;
        else                    z = static_cast<float>(static_cast<int>(z));
        return z;
    }

    /* Inline a^b without dragging <cmath> into a hot ISR path on -O0 builds.
     * The compiler reduces this to a libm call regardless; this is just to
     * avoid the unused-warning when no transforms are configured. */
    static float Pow(float base, float exponent);

    /* Identity. */
    uint8_t     pot_  = 0;
    const char* name_ = "";

    /* Host descriptor metadata (optional). */
    const char*        ident_      = nullptr;
    const char*        unit_       = nullptr;
    const char*        disp_json_  = nullptr;
    const char* const* labels_     = nullptr;
    uint8_t            num_labels_ = 0;

    /* Transform. */
    Xform xform_     = Xform::Linear;
    float xform_min_ = 0.f;
    float xform_max_ = 1.f;

    /* Ring slot — populated by .Ring(...). Consumed by ControlLoop / PerfRenderer. */
    ParamSlot slot_ = {};

    /* Custom ring + overdraw hooks. */
    RingFn custom_fn_    = nullptr;
    void*  custom_ctx_   = nullptr;
    RingFn overdraw_fn_  = nullptr;
    void*  overdraw_ctx_ = nullptr;

    /* CV binding. */
    int8_t cv_index_   = -1;
    float  cv_atten_   = 1.0f;
    bool   cv_bipolar_ = true;

    /* Wiring (set by ControlLoop). */
    KnobStorage* storage_   = nullptr;
    LockSource*  locks_     = nullptr;
    CvSource*    cv_source_ = nullptr;
    const float* cv_buf_    = nullptr;
    uint8_t      num_cv_    = 0;
    const float* phys_buf_  = nullptr;
    uint8_t      num_phys_  = 0;
    uint8_t      page_      = 0;
};

} // namespace alchemy
