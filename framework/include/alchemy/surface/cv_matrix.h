/**
 * @file alchemy/surface/cv_matrix.h
 * @brief alchemy::CvMatrix — per-jack CV dispatch surface.
 *
 * `CvMatrix` is the canonical concrete `CvSource`.  Each CV jack has a
 * destination.  That's it.
 *
 * A destination is one of three things:
 *   - `Off`              — CV consumed, no effect.
 *   - `To(knob)`         — modulates a `VirtualKnob`.
 *   - `Custom(fn, ctx)`  — fires a user callback at poll cadence (tap
 *                          tempo, gates, freeze, direct-to-DSP, etc.).
 *
 * Setting and updating a destination are the same operation:
 *
 *   matrix.Jack(0).To(l_hi_level);                 // CV0 → l_hi_level
 *   matrix.Jack(1).Custom(OnTap, &tap_state);      // CV1 → callback
 *   matrix.Jack(2).Off();                          // CV2 → nothing
 *
 * A static layout is just calling these once at startup.  A runtime
 * layout is calling them again whenever you want a jack to point
 * somewhere else:
 *
 *   void OnFrame() {
 *       matrix.Jack(0).To(*menu[user_selector.Value()]);
 *   }
 *
 */

#pragma once

#include <cstdint>

#include "alchemy/surface/cv_source.h"

namespace alchemy {

class VirtualKnob;

class CvMatrix : public CvSource
{
  public:
    /** Max jacks one matrix can dispatch.  Matches kNumCvInputs. */
    static constexpr uint8_t kMaxJacks = 8u;

    /** Per-(page, pot) delta cache dims.  Match Pager/Page caps. */
    static constexpr uint8_t kMaxPages = 8u;
    static constexpr uint8_t kMaxPots  = 8u;

    /** Custom-destination callback signature.  Fired from PollEdges (1 ms). */
    using CustomFn = void (*)(float cv_value, uint32_t t_us, void* ctx);

    /* ── JackBuilder ───────────────────────────────────────────────────── */

    /**
     * Per-jack configuration / mutation proxy.  Returned by-value from
     * `CvMatrix::Jack(n)`; methods write directly into the matrix's
     * jack array and return `*this` for chaining.  Out-of-range jack
     * indices produce a no-op builder.
     *
     * The destination-setting methods (`To`, `Off`, `Custom`) are the
     * only mechanism for changing where a jack routes.  Call them once
     * for a static layout, or call them again whenever the routing
     * decision changes — there is no separate "bind" mechanism.
     */
    class JackBuilder
    {
      public:
        JackBuilder(CvMatrix* m, uint8_t jack) : m_(m), jack_(jack) {}

        /** Route this jack to `knob`. */
        JackBuilder& To(VirtualKnob& knob);

        /** Route this jack to nothing.  CV is consumed but has no effect. */
        JackBuilder& Off();

        /**
         * Route this jack to a user callback fired from PollEdges.
         * `ctx` must outlive the matrix (or be null).
         */
        JackBuilder& Custom(CustomFn fn, void* ctx = nullptr);

        /**
         * Attenuation multiplier applied before the knob accumulates.
         * Identity is 1.0; -1.0 inverts; 0.0 mutes.  Ignored for `Off`
         * and `Custom` destinations.
         */
        JackBuilder& Atten(float v);

        /**
         * Bipolar (default, eurorack ±5 V convention): raw CV in [0, 1]
         * is treated as [-0.5, +0.5].  Unipolar: raw CV is forwarded
         * as-is in [0, 1].  Ignored for `Off` and `Custom`.
         */
        JackBuilder& Bipolar(bool b);

      private:
        CvMatrix* m_;
        uint8_t   jack_;
    };

    /* ── CvMatrix ──────────────────────────────────────────────────────── */

    /**
     * @param num_jacks  Number of CV jacks to dispatch (≤ kMaxJacks).
     *                   Jacks are 1:1 mapped to physical CV jack indices.
     *
     * Jacks default to `Off`; call `.Jack(n).To(...)` to wire them.
     */
    explicit CvMatrix(uint8_t num_jacks);

    /** Begin configuring jack @p jack.  Out-of-range returns a no-op builder. */
    JackBuilder Jack(uint8_t jack);

    /** Number of jacks this matrix dispatches. */
    uint8_t NumJacks() const { return num_jacks_; }

    /* ── CvSource ──────────────────────────────────────────────────────── */

    float DeltaAtPage(uint8_t page, uint8_t pot) const override;
    void  PollEdges  (const float* cv, uint32_t t_us) override;
    void  Update     (const float* cv, uint32_t t_ms) override;

  private:
    enum class Kind : uint8_t { Off = 0, Knob, Custom };

    struct JackState
    {
        Kind         kind    = Kind::Off;
        VirtualKnob* knob    = nullptr;
        CustomFn     fn      = nullptr;
        void*        ctx     = nullptr;
        float        atten   = 1.0f;
        bool         bipolar = true;
    };

    uint8_t   num_jacks_;
    JackState jacks_[kMaxJacks] = {};
    float     delta_cache_[kMaxPages][kMaxPots] = {};
};

} // namespace alchemy
