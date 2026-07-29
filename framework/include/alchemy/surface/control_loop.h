/**
 * @file alchemy/surface/control_loop.h
 * @brief alchemy::ControlLoop — opt-in main-thread frame driver.
 *
 * ControlLoop owns the control-rate frame: hardware polling, per-frame
 * `phys[]` and `cv[]` buffer fills, surface update sequencing, LED render
 * bracketing, and user hooks. Distinct from the audio loop (sample rate,
 * ISR, owned by `hw.StartAudio`).
 *
 * Usage:
 *
 *   ControlLoop loop(hw);
 *   loop.Use(pager)                         // KnobStorage
 *       .Use(locks)                         // LockSource
 *       .Use(cv_router)                     // CvSource (optional)
 *       .Use(left_page)                     // Page of VirtualKnobs
 *       .Use(right_page)
 *       .OnFrame(UpdateCoeffs);
 *
 *   for (;;) loop.Tick();
 *
 * Attach order is irrelevant: surfaces slot into fixed roles internally and
 * the loop sequences them in a canonical order regardless of `Use(...)` order.
 *
 * Canonical per-frame sequence (each surface only runs if attached):
 *
 *   1. inner 1 ms poll loop:
 *        hw.ProcessAllControls + cv[] sampling
 *        settings.PollButtons      (gates the rest while active)
 *        cv_source.PollEdges       (sub-frame CV edge timestamps)
 *        locks.PollButtons         (gated by settings)
 *        buttons.PollButtons       (gated; gestures fire here, and may
 *                                   consume the pager's release)
 *        storage.PollButtons       (gated by settings)
 *        OnPoll user hook
 *   2. phys[] snapshot
 *   3. settings.Update             (always runs; B2+B3 enter gesture)
 *      locks.Advance               (always runs; playback survives Settings)
 *      buttons.Update              (always runs; flushes deferred
 *                                   preset-change callbacks)
 *   4. while !settings.IsActive():
 *        cv_source.Update          (refresh per-(page, pot) CV delta cache)
 *        locks.Update              (record/disarm gestures; may consume B1)
 *        storage.Update            (Pager: page-advance + pot-catch)
 *        OnPageChange hook
 *        OnFrame user hook
 *   5. Render: clear → (Settings if active, else: PerfRenderer pass →
 *      custom rings → overdraw → page indicator → ButtonBank colors) →
 *      OnRender hook → LockSource overlay → leds.Show
 */

#pragma once

#include <cstdint>
#include "alchemy/hw/alchemy_lab_layout.h"   /* kNumPots, kNumCvInputs */
#include "alchemy/led/perf_renderer.h"
#include "alchemy/surface/cv_source.h"
#include "alchemy/surface/host_service.h"
#include "alchemy/surface/knob_storage.h"
#include "alchemy/surface/lock_source.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/virtual_knob.h"

namespace alchemy {

#if defined(ALCHEMY_BOARD_V2)
class AlchemyLabV2;
using AlchemyLab = AlchemyLabV2;
#else
class AlchemyLabV1;
using AlchemyLab = AlchemyLabV1;
#endif
class ButtonBank;
class IButton;
class Settings;

class ControlLoop
{
  public:
    /** Maximum Pages attachable to the loop. */
    static constexpr uint8_t kMaxPages = 8u;

    /** Maximum pots a single page may declare (matches PerfRenderer's limit). */
    static constexpr uint8_t kMaxPagePots = 8u;

    /** Default frame interval (~60 Hz). */
    static constexpr uint32_t kDefaultFrameMs = 16u;

    /** Default inner-poll cadence (button debounce). */
    static constexpr uint32_t kDefaultPollMs  = 1u;

    using FrameFn  = void(*)();
    using RenderFn = void(*)(uint32_t t_ms);
    using PollFn   = void(*)(uint32_t t_ms);

    /**
     * @param hw         Board hardware abstraction.
     * @param frame_ms   Control-frame interval (default 16 ms).
     * @param poll_ms    Inner poll cadence for ProcessAllControls (default 1 ms).
     */
    explicit ControlLoop(AlchemyLab&   hw,
                         uint32_t      frame_ms = kDefaultFrameMs,
                         uint32_t      poll_ms  = kDefaultPollMs);

    /* ── Cadence setters (fluent, optional) ──────────────────────────── */

    ControlLoop& FrameMs(uint32_t ms) { frame_ms_ = ms; return *this; }
    ControlLoop& PollMs (uint32_t ms) { poll_ms_  = ms ? ms : 1u; return *this; }

    /* ── Surface attach ──────────────────────────────────────────────── */

    /** Attach the canonical KnobStorage (e.g. a Pager). */
    ControlLoop& Use(KnobStorage& s) { storage_ = &s; return *this; }

    /** Attach the canonical LockSource (e.g. a ParamLock<N>). */
    ControlLoop& Use(LockSource& l)  { locks_   = &l; return *this; }

    /**
     * Attach a CvSource for runtime-routed CV dispatch.  When attached,
     * VirtualKnob's static `.Cv(...)` binding is ignored — every knob's
     * CV contribution comes from `source.DeltaAtPage(page, pot)` instead.
     * The source is polled at 1 ms cadence (PollEdges) for sub-frame
     * edge timestamps and refreshed once per frame (Update).
     */
    ControlLoop& Use(CvSource& c)    { cv_source_ = &c; return *this; }

    /**
     * Attach a Settings surface. While `settings.IsActive()` returns true,
     * ControlLoop skips perf updates (storage/locks) and the perf render
     * path; settings owns the screen via its own Render(). The B2+B3 enter
     * gesture is processed every frame regardless.
     */
    ControlLoop& Use(Settings& s)    { settings_ = &s; return *this; }

    /**
     * Attach a Page. The loop holds a reference and reads the page's knob
     * list fresh each frame, so runtime Add/Remove/Clear on the page (e.g.
     * driven by a settings option) takes effect on the next Tick().
     */
    ControlLoop& Use(Page& page)
    {
        if (num_pages_ < kMaxPages) pages_[num_pages_++] = &page;
        return *this;
    }

    /**
     * Attach a ButtonBank.  The loop wires the board's buttons and its
     * own page list into the bank, polls gestures at 1 ms (gated by
     * Settings like every surface), flushes deferred change callbacks
     * each frame, and renders the bank's per-zone button colors after
     * the page indicator (a mode color deliberately wins over the tint).
     */
    ControlLoop& Use(ButtonBank& bank);

    /**
     * Attach a host-communication service (e.g. hostlink::Host).  Polled
     * from the inner 1 ms loop so host commands execute on the same
     * thread as panel gestures.
     */
    ControlLoop& Use(HostService& h)
    {
        host_service_ = &h;
        h.OnAttach(*this);
        return *this;
    }

    /* ── Hooks (fluent, optional) ────────────────────────────────────── */

    ControlLoop& OnFrame      (FrameFn  fn) { on_frame_       = fn; return *this; }
    ControlLoop& OnRender     (RenderFn fn) { on_render_      = fn; return *this; }
    ControlLoop& OnPageChange (FrameFn  fn) { on_page_change_ = fn; return *this; }

    /**
     * Hook fired inside the inner 1 ms poll loop, immediately after the
     * SDK's own button polls. Receives the current millisecond timestamp
     * (handy for tap tempo, hold timing, etc.). The SDK's surfaces are
     * all already polling at this cadence; OnPoll is just the user's slot
     * in the same iteration, not a special-case escape hatch.
     */
    ControlLoop& OnPoll       (PollFn   fn) { on_poll_        = fn; return *this; }

    /* ── Lifecycle ───────────────────────────────────────────────────── */

    /** Run one frame: poll → update → render → show. */
    void Tick();

    /* ── Buffer accessors (for user composition sites and DSP) ───────── */

    const float* Phys()  const { return phys_; }
    const float* Cv()    const { return cv_; }
    uint8_t      NumCv() const { return num_cv_; }

    /* ── Introspection (for attached services, e.g. HostLink) ────────── */

    AlchemyLab*  Hw()               const { return hw_; }
    KnobStorage* AttachedStorage()  const { return storage_; }
    Settings*    AttachedSettings() const { return settings_; }
    uint8_t      NumAttachedPages() const { return num_pages_; }
    const Page*  AttachedPage(uint8_t i) const
    {
        return (i < num_pages_) ? pages_[i] : nullptr;
    }

  private:
    void WireKnobs();
    void Render(uint32_t t_ms);

    /** Re-wire one knob's storage/locks/cv/phys refs against the current loop state. */
    void WireKnob(VirtualKnob& k, uint8_t page_idx)
    {
        k.AttachStorage(storage_, page_idx);
        k.AttachLockSource(locks_);
        k.AttachCvSource(cv_source_);
        k.AttachCv(cv_, num_cv_);
        k.AttachPhys(phys_, num_pots_);
    }

    AlchemyLab*   hw_;
    uint32_t      frame_ms_;
    uint32_t      poll_ms_;

    /* Per-frame buffers. */
    float   phys_[kNumPots]     = {};
    float   cv_  [kNumCvInputs] = {};
    uint8_t num_pots_           = kNumPots;
    uint8_t num_cv_             = kNumCvInputs;

    /* Attached surfaces. */
    KnobStorage* storage_      = nullptr;
    LockSource*  locks_        = nullptr;
    CvSource*    cv_source_    = nullptr;
    Settings*    settings_     = nullptr;
    HostService* host_service_ = nullptr;
    ButtonBank*  buttons_      = nullptr;

    /* IButton refs handed to the ButtonBank (uniform across boards). */
    IButton* btn_refs_[kNumButtons] = {};

    /* Attached pages (each carries its own knob list). */
    Page*   pages_[kMaxPages] = {};
    uint8_t num_pages_        = 0;

    /* Hooks. */
    FrameFn  on_frame_       = nullptr;
    RenderFn on_render_      = nullptr;
    FrameFn  on_page_change_ = nullptr;
    PollFn   on_poll_        = nullptr;

    /* Renderer (constructed at first Tick from hw geometry). */
    PerfRenderer renderer_;

    /* State. */
    uint8_t last_page_ = 0xFFu;
};

} // namespace alchemy
