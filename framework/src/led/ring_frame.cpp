/**
 * @file ring_frame.cpp
 * @brief alchemy::RingFrame implementation — the single rendering core
 *        behind Base fills, selectors, pips, fields, and the legacy
 *        panel-direct Draw* wrappers.
 */

#include "alchemy/led/ring_frame.h"
#include "alchemy/led/signal.h"
#include "alchemy/led/anims/catch_pip.h"

#include <cmath>

namespace alchemy {

namespace {

constexpr float kTwoPi         = 6.28318530f;
constexpr float kPulsePeriodMs = 2000.0f;
constexpr float kRippleHz      = 0.5f;
constexpr float kRippleSpread  = 0.8f;

inline float Clamp01(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

inline bool IsBlack(const LedPanel::Rgb& c)
{
    return c.r == 0u && c.g == 0u && c.b == 0u;
}

} // namespace

/* ── Frame lifecycle ─────────────────────────────────────────────────────── */

void RingFrame::BeginCommon(const ArcGeometry& geo)
{
    geo_ = geo;
    n_   = geo.arc_leds < kMaxLeds ? geo.arc_leds : kMaxLeds;
    for (uint8_t k = 0; k < n_; k++)
    {
        fr_[k] = 0.0f;
        fg_[k] = 0.0f;
        fb_[k] = 0.0f;
    }
    touched_    = 0u;
    active_     = SpanLeds{};
    arm_cw_     = SpanLeds{};
    arm_ccw_    = SpanLeds{};
    past_tip_   = SpanLeds{};
    span_a01_   = 0.0f;
    span_b01_   = 1.0f;
    bottom_set_ = false;
}

void RingFrame::Begin(const ArcGeometry& geo)
{
    BeginCommon(geo);
    overlay_ = false;
}

void RingFrame::BeginOverlay(const ArcGeometry& geo)
{
    BeginCommon(geo);
    overlay_ = true;
}

/* ── Raw access ──────────────────────────────────────────────────────────── */

void RingFrame::Set(uint8_t k, const LedPanel::Rgb& c)
{
    if (k >= n_) return;
    fr_[k] = static_cast<float>(c.r);
    fg_[k] = static_cast<float>(c.g);
    fb_[k] = static_cast<float>(c.b);
    touched_ |= (1u << k);
}

void RingFrame::Add(uint8_t k, const LedPanel::Rgb& c, float gain)
{
    if (k >= n_ || gain <= 0.0f) return;
    fr_[k] += static_cast<float>(c.r) * gain;
    fg_[k] += static_cast<float>(c.g) * gain;
    fb_[k] += static_cast<float>(c.b) * gain;
    touched_ |= (1u << k);
}

void RingFrame::Dim(uint8_t k, float gain)
{
    if (k >= n_) return;
    /* Dimming modulates frame contents.  In an overlay frame an untouched
     * LED holds nothing — dimming it would emit black over the underlying
     * render instead of leaving it alone, so it stays a no-op. */
    if (overlay_ && !(touched_ & (1u << k))) return;
    const float g = Clamp01(gain);
    fr_[k] *= g;
    fg_[k] *= g;
    fb_[k] *= g;
    touched_ |= (1u << k);
}

void RingFrame::MarkActive(int first_led, int last_led)
{
    if (first_led < 0 || last_led < first_led
        || first_led >= static_cast<int>(n_))
    {
        active_ = SpanLeds{};
        return;
    }
    if (last_led >= static_cast<int>(n_)) last_led = n_ - 1;
    active_ = SpanLeds{first_led, last_led,
                       static_cast<float>(first_led),
                       static_cast<float>(last_led)};
}

void RingFrame::SetSpan(float a01, float b01)
{
    span_a01_ = Clamp01(a01);
    span_b01_ = Clamp01(b01);
    if (span_b01_ < span_a01_) span_b01_ = span_a01_;
}

/* ── Region resolution ───────────────────────────────────────────────────── */

RingFrame::SpanLeds RingFrame::Resolve(Region region) const
{
    const float last = static_cast<float>(n_ ? n_ - 1u : 0u);
    switch (region)
    {
        case Region::Full:
            return SpanLeds{0, static_cast<int>(n_) - 1, 0.0f, last};
        case Region::Active:  return active_;
        case Region::PastTip:
        case Region::Passive: return past_tip_;
        case Region::ArmCw:   return arm_cw_;
        case Region::ArmCcw:  return arm_ccw_;
        case Region::Span:
        {
            const float a = span_a01_ * last;
            const float b = span_b01_ * last;
            return SpanLeds{static_cast<int>(a + 0.5f),
                            static_cast<int>(b + 0.5f), a, b};
        }
        case Region::BottomPip:
        default:
            return SpanLeds{};
    }
}

/* ── Legacy fill animation (exact FillAnim semantics) ────────────────────── */

float RingFrame::AnimGain(FillAnim anim, float depth, uint8_t step,
                          uint32_t t_ms) const
{
    if (anim == FillAnim::Pulse)
    {
        const float lo    = 1.0f - depth;
        const float phase = kTwoPi
            * static_cast<float>(t_ms % static_cast<uint32_t>(kPulsePeriodMs))
            / kPulsePeriodMs;
        return lo + depth * (0.5f + 0.5f * std::sin(phase));
    }
    if (anim == FillAnim::Ripple)
    {
        float k = 1.0f + depth * std::sin(
            kTwoPi * kRippleHz * (static_cast<float>(t_ms) * 0.001f)
            + static_cast<float>(step) * kRippleSpread);
        return Clamp01(k);
    }
    return 1.0f;
}

/* ── Fill base ───────────────────────────────────────────────────────────── */

void RingFrame::Base(const FillDesc& desc, float value01, uint32_t t_ms)
{
    if (n_ == 0u) return;
    const float v = Clamp01(value01);

    if (desc.mode == FillMode::Edge)
    {
        const float fill_f    = v * static_cast<float>(n_);
        uint8_t     fill_hard = static_cast<uint8_t>(fill_f);
        float       fill_frac = fill_f - static_cast<float>(fill_hard);
        ArcSnapFrac(fill_hard, fill_frac, n_);

        const bool ccw = desc.direction == FillDirection::Ccw;
        const auto led = [&](uint8_t step) -> uint8_t {
            return ccw ? static_cast<uint8_t>(n_ - 1u - step) : step;
        };

        for (uint8_t step = 0u; step < n_; step++)
        {
            const uint8_t k      = led(step);
            const float   anim_k = AnimGain(desc.anim, desc.anim_depth,
                                            k, t_ms);
            if (step < fill_hard)
                Set(k, LedPanel::Scale(desc.color, anim_k));
            else if (step == fill_hard && fill_frac > 0.0f)
                Set(k, LedPanel::Scale(desc.color, fill_frac * anim_k));
            else if (desc.compose == FillCompose::Replace)
                Set(k, desc.passive_color);
        }

        /* Region map.  last_lit includes a fractional tip LED. */
        const int last_lit = fill_frac > 0.0f
                                 ? static_cast<int>(fill_hard)
                                 : static_cast<int>(fill_hard) - 1;
        if (last_lit >= 0)
        {
            const int a = ccw ? static_cast<int>(n_) - 1 - last_lit : 0;
            const int b = ccw ? static_cast<int>(n_) - 1 : last_lit;
            active_ = SpanLeds{a, b, static_cast<float>(a),
                               static_cast<float>(b)};
        }
        const int pt_first = last_lit + 1;
        if (pt_first < static_cast<int>(n_))
        {
            const int a = ccw ? 0 : pt_first;
            const int b = ccw ? static_cast<int>(n_) - 1 - pt_first
                              : static_cast<int>(n_) - 1;
            if (b >= a)
                past_tip_ = SpanLeds{a, b, static_cast<float>(a),
                                     static_cast<float>(b)};
        }
        return;
    }

    /* Center: fan out from desc.pivot01 (0..1), each arm normalized to
     * its own side so both reach their ends at the pot extremes. */
    const float pivot   = Clamp01(desc.pivot01);
    const int   pivot_k = static_cast<int>(pivot
                              * static_cast<float>(n_ - 1u) + 0.5f);
    const int   ccw_cap = pivot_k;
    const int   cw_cap  = static_cast<int>(n_) - 1 - pivot_k;

    const float disp   = v - pivot;
    const bool  is_cw  = disp > 0.0f;
    const float side   = is_cw ? (1.0f - pivot) : pivot;
    const float amount = side > 0.0f ? Clamp01((disp < 0.0f ? -disp : disp)
                                               / side)
                                     : 0.0f;
    const int   cap    = is_cw ? cw_cap : ccw_cap;

    const float side_f    = amount * static_cast<float>(cap);
    uint8_t     side_hard = static_cast<uint8_t>(side_f);
    float       side_frac = side_f - static_cast<float>(side_hard);
    ArcSnapFrac(side_hard, side_frac, static_cast<uint8_t>(cap));

    const bool          neg_distinct = !IsBlack(desc.neg_color);
    const LedPanel::Rgb ccw_color    = neg_distinct ? desc.neg_color
                                                    : desc.color;

    for (uint8_t k = 0u; k < n_; k++)
    {
        const float anim_k = AnimGain(desc.anim, desc.anim_depth, k, t_ms);
        LedPanel::Rgb c{0u, 0u, 0u};

        if (static_cast<int>(k) == pivot_k)
        {
            c = LedPanel::Scale(desc.center_color, anim_k);
        }
        else if (static_cast<int>(k) < pivot_k && !is_cw && amount > 0.0f)
        {
            const int dist = pivot_k - static_cast<int>(k);
            if (dist <= static_cast<int>(side_hard))
                c = LedPanel::Scale(ccw_color, anim_k);
            else if (dist == static_cast<int>(side_hard) + 1)
                c = LedPanel::Scale(ccw_color, side_frac * anim_k);
        }
        else if (static_cast<int>(k) > pivot_k && is_cw)
        {
            const int dist = static_cast<int>(k) - pivot_k;
            if (dist <= static_cast<int>(side_hard))
                c = LedPanel::Scale(desc.color, anim_k);
            else if (dist == static_cast<int>(side_hard) + 1)
                c = LedPanel::Scale(desc.color, side_frac * anim_k);
        }

        if (desc.compose == FillCompose::Overlay && IsBlack(c))
        {
            if (static_cast<int>(k) != pivot_k || amount <= kArcEdgeSnap)
                continue;
        }
        Set(k, c);
    }

    /* Region map: lit arm spans plus the pivot. */
    const int lit = static_cast<int>(side_hard)
                  + (side_frac > 0.0f ? 1 : 0);
    if (lit > 0)
    {
        if (is_cw)
        {
            const int b = pivot_k + (lit > cw_cap ? cw_cap : lit);
            arm_cw_ = SpanLeds{pivot_k + 1, b,
                               static_cast<float>(pivot_k + 1),
                               static_cast<float>(b)};
            active_ = SpanLeds{pivot_k, b, static_cast<float>(pivot_k),
                               static_cast<float>(b)};
        }
        else
        {
            const int a = pivot_k - (lit > ccw_cap ? ccw_cap : lit);
            arm_ccw_ = SpanLeds{a, pivot_k - 1, static_cast<float>(a),
                                static_cast<float>(pivot_k - 1)};
            active_ = SpanLeds{a, pivot_k, static_cast<float>(a),
                               static_cast<float>(pivot_k)};
        }
    }
    else
    {
        active_ = SpanLeds{pivot_k, pivot_k, static_cast<float>(pivot_k),
                           static_cast<float>(pivot_k)};
    }
}

/* ── Selector base ───────────────────────────────────────────────────────── */

namespace {

inline uint8_t DistributedZonePosition(uint8_t z, uint8_t num_zones,
                                       uint8_t arc_leds)
{
    if (arc_leds == 0u)  return 0u;
    if (num_zones <= 1u) return 0u;
    const float frac = static_cast<float>(z)
                     / static_cast<float>(num_zones - 1u);
    const float pos  = frac * static_cast<float>(arc_leds - 1u) + 0.5f;
    uint8_t step = static_cast<uint8_t>(pos);
    if (step >= arc_leds) step = static_cast<uint8_t>(arc_leds - 1u);
    return step;
}

inline LedPanel::Rgb ZoneColor(const SelectorDesc& desc, uint8_t z,
                               uint8_t selected)
{
    if (z == selected) return desc.active_color;
    if (IsBlack(desc.inactive_color)) return {0u, 0u, 0u};
    return LedPanel::Scale(desc.inactive_color, desc.inactive_dim);
}

} // namespace

void RingFrame::Base(const SelectorDesc& desc, float value01)
{
    if (desc.num_zones == 0u) return;
    /* Region zones too narrow for the arc fall back to Distributed
     * layout in BaseZone; the zone must then be derived with the
     * Distributed rounding rule or boundary values select a different
     * zone than the one rendered. */
    const bool region_falls_back =
        desc.zone_geo == ZoneGeometry::Region
        && static_cast<uint16_t>(n_) + 1u
               < static_cast<uint16_t>(desc.num_zones) * 2u;

    uint8_t zone;
    if (desc.zone_geo == ZoneGeometry::Distributed || region_falls_back)
    {
        /* Nearest-zone rounding, matching DrawSelector. */
        const float pos = Clamp01(value01)
                        * static_cast<float>(desc.num_zones - 1u) + 0.5f;
        zone = static_cast<uint8_t>(pos);
    }
    else
    {
        zone = static_cast<uint8_t>(Clamp01(value01)
                                    * static_cast<float>(desc.num_zones));
    }
    if (zone >= desc.num_zones)
        zone = static_cast<uint8_t>(desc.num_zones - 1u);
    BaseZone(desc, zone);
}

void RingFrame::BaseZone(const SelectorDesc& desc, uint8_t zone)
{
    if (n_ == 0u || desc.num_zones == 0u) return;
    if (zone >= desc.num_zones)
        zone = static_cast<uint8_t>(desc.num_zones - 1u);

    if (desc.zone_geo == ZoneGeometry::Region
        && static_cast<uint16_t>(n_) + 1u
               >= static_cast<uint16_t>(desc.num_zones) * 2u)
    {
        const uint16_t N = desc.num_zones;
        for (uint8_t k = 0u; k < n_; k++)
        {
            const uint8_t zone_idx = static_cast<uint8_t>(
                (static_cast<uint16_t>(k) * N) / n_);
            const uint8_t slot_end = static_cast<uint8_t>(
                ((static_cast<uint16_t>(zone_idx) + 1u) * n_ + N - 1u) / N
                - 1u);
            const bool is_final = (zone_idx == desc.num_zones - 1u);
            const bool is_gap   = (k == slot_end) && !is_final;

            LedPanel::Rgb c{0u, 0u, 0u};
            if (!is_gap
                && (desc.avail_mask & static_cast<uint16_t>(1u << zone_idx)))
                c = ZoneColor(desc, zone_idx, zone);
            Set(k, c);

            if (zone_idx == zone && !is_gap)
            {
                if (active_.first < 0) active_.first = k;
                active_.last = k;
            }
        }
        if (active_.first >= 0)
        {
            active_.a = static_cast<float>(active_.first);
            active_.b = static_cast<float>(active_.last);
        }
        return;
    }

    if (desc.zone_geo == ZoneGeometry::Point)
    {
        for (uint8_t k = 0u; k < n_; k++)
        {
            LedPanel::Rgb c{0u, 0u, 0u};
            if (k < desc.num_zones
                && (desc.avail_mask & static_cast<uint16_t>(1u << k)))
                c = ZoneColor(desc, k, zone);
            Set(k, c);
        }
        if (zone < n_)
            active_ = SpanLeds{zone, zone, static_cast<float>(zone),
                               static_cast<float>(zone)};
        return;
    }

    /* Distributed (default, and the Region fallback). */
    for (uint8_t k = 0u; k < n_; k++) Set(k, {0u, 0u, 0u});
    for (uint8_t z = 0u; z < desc.num_zones; z++)
    {
        /* avail_mask has 16 bits; zones beyond it are unavailable (and a
         * shift ≥ 16 would be undefined for zone counts > 32). */
        if (z >= 16u) break;
        if (!(desc.avail_mask & static_cast<uint16_t>(1u << z))) continue;
        const uint8_t k = DistributedZonePosition(z, desc.num_zones, n_);
        Set(k, ZoneColor(desc, z, zone));
        if (z == zone)
            active_ = SpanLeds{k, k, static_cast<float>(k),
                               static_cast<float>(k)};
    }
}

/* ── Pip overlay ─────────────────────────────────────────────────────────── */

void RingFrame::PaintTap(const PipDesc& desc, const SpanLeds& s, int k,
                         float w, bool has_bg)
{
    if (w <= 0.0f || n_ == 0u) return;
    if (w > 1.0f) w = 1.0f;
    if (k < s.first || k > s.last) return;
    if (k < 0 || k >= static_cast<int>(n_)) return;

    switch (desc.compose)
    {
        case PipCompose::Replace:
        {
            const LedPanel::Rgb c =
                has_bg ? LedPanel::Mix(desc.background, desc.color, w)
                       : LedPanel::Scale(desc.color, w);
            Set(static_cast<uint8_t>(k), c);
            break;
        }
        case PipCompose::Add:
            Add(static_cast<uint8_t>(k), desc.color, w);
            break;
        case PipCompose::Carve:
            Dim(static_cast<uint8_t>(k), 1.0f - w);
            break;
    }
}

void RingFrame::Pip(Region region, const PipDesc& desc, float pos01,
                    float gain, uint32_t t_ms)
{
    if (n_ == 0u || gain <= 0.0f) return;
    if (gain > 1.0f) gain = 1.0f;

    if (desc.blink_hz > 0.0f)
    {
        float half_f = 500.0f / desc.blink_hz;
        if (half_f > 4.0e9f) half_f = 4.0e9f;   /* keep the cast defined */
        const uint32_t half_ms = static_cast<uint32_t>(half_f);
        if (half_ms > 0u && (t_ms / half_ms) % 2u != 0u) return;
    }

    /* Motion mapping. */
    float p = pos01;
    switch (desc.motion)
    {
        case PipMotion::Wrap:
            p -= std::floor(p);
            break;
        case PipMotion::Bounce:
            p = p < 0.0f ? 0.0f : Tri01(p);
            break;
        case PipMotion::Direct:
        default:
            p = Clamp01(p);
            break;
    }

    if (region == Region::BottomPip)
    {
        bottom_set_ = true;
        bottom_     = LedPanel::Scale(desc.color, gain);
        return;
    }

    const SpanLeds s = Resolve(region);
    if (s.first < 0) return;

    const bool has_bg = !IsBlack(desc.background);
    if (has_bg)
        for (int k = s.first; k <= s.last; k++)
            Set(static_cast<uint8_t>(k), desc.background);

    /* Trail configuration — tail_intensity is the one-step comet form. */
    uint8_t trail_steps   = desc.trail_steps;
    float   trail_falloff = desc.trail_falloff;
    if (trail_steps == 0u && desc.tail_intensity > 0.0f)
    {
        trail_steps   = 1u;
        trail_falloff = desc.tail_intensity;
    }

    const float center_f = s.a + p * (s.b - s.a);

    if (desc.smooth && desc.width <= 1u)
    {
        const int   ci   = static_cast<int>(center_f);
        const float frac = center_f - static_cast<float>(ci);

        PaintTap(desc, s, ci,     (1.0f - frac) * gain, has_bg);
        PaintTap(desc, s, ci + 1, frac * gain,          has_bg);

        float w = gain;
        for (uint8_t i = 1u; i <= trail_steps; i++)
        {
            w *= trail_falloff;
            if (w <= 0.003f) break;
            if (desc.trail_side != PipTrailSide::Cw)
                PaintTap(desc, s, ci - static_cast<int>(i),
                         w * (1.0f - frac), has_bg);
            if (desc.trail_side != PipTrailSide::Ccw)
                PaintTap(desc, s, ci + 1 + static_cast<int>(i),
                         w * frac, has_bg);
        }
        return;
    }

    /* Quantised path: original LED-stepping behavior. */
    int center = static_cast<int>(center_f + 0.5f);
    if (center > s.last)  center = s.last;
    if (center < s.first) center = s.first;

    const uint8_t width   = desc.width > 0u ? desc.width : 1u;
    const int     first_i = center - static_cast<int>(width) / 2;
    const int     last_i  = first_i + static_cast<int>(width) - 1;
    for (int k = first_i; k <= last_i; k++)
        PaintTap(desc, s, k, gain, has_bg);

    float w = gain;
    for (uint8_t i = 1u; i <= trail_steps; i++)
    {
        w *= trail_falloff;
        if (w <= 0.003f) break;
        if (desc.trail_side != PipTrailSide::Cw)
            PaintTap(desc, s, first_i - static_cast<int>(i), w, has_bg);
        if (desc.trail_side != PipTrailSide::Ccw)
            PaintTap(desc, s, last_i + static_cast<int>(i), w, has_bg);
    }
}

/* ── Field overlay ───────────────────────────────────────────────────────── */

void RingFrame::Field(Region region, const FieldDesc& desc, float amount,
                      uint32_t t_ms, float phase01)
{
    if (n_ == 0u || amount <= 0.0f) return;
    if (amount > 1.0f) amount = 1.0f;

    const SpanLeds s = Resolve(region);
    if (s.first < 0) return;

    /* Whole grain is one modulation level for the region — evaluate once. */
    float whole_m = 0.0f;
    if (desc.grain == FieldGrain::Whole)
    {
        whole_m = FieldEval(desc, 0u, t_ms, phase01);
        if (desc.invert) whole_m = 1.0f - whole_m;
    }

    for (int k = s.first; k <= s.last; k++)
    {
        float m;
        if (desc.grain == FieldGrain::Whole)
        {
            m = whole_m;
        }
        else
        {
            uint32_t unit = static_cast<uint32_t>(k - s.first);
            if (desc.grain == FieldGrain::Chunk)
                unit /= (desc.chunk_leds ? desc.chunk_leds : 1u);
            m = FieldEval(desc, unit, t_ms, phase01);
            if (desc.invert) m = 1.0f - m;
        }

        Dim(static_cast<uint8_t>(k), 1.0f - amount * m);
    }
}

/* ── Emit ────────────────────────────────────────────────────────────────── */

void RingFrame::Emit(LedPanel& dst, uint8_t pot) const
{
    const auto quant = [](float v) -> uint8_t {
        if (v <= 0.0f)   return 0u;
        if (v >= 255.0f) return 255u;
        return static_cast<uint8_t>(v + 0.5f);
    };

    for (uint8_t k = 0; k < n_; k++)
    {
        if (overlay_ && !(touched_ & (1u << k))) continue;
        const float hour = geo_.start_hour
                         + static_cast<float>(k) * geo_.step_hours;
        dst.SetRingByHour(pot, hour,
                          dst.ScaleGlobal({quant(fr_[k]), quant(fg_[k]),
                                           quant(fb_[k])}));
    }

    if (bottom_set_)
        dst.SetRingByHour(pot, 6.0f, dst.ScaleGlobal(bottom_));
}

void RingFrame::Emit(LedPanel& dst, uint8_t pot, const PotState& ps,
                     LedPanel::Rgb catch_color) const
{
    Emit(dst, pot);
    DrawCatchPip(dst, pot, ps, geo_, catch_color);
}

} // namespace alchemy
