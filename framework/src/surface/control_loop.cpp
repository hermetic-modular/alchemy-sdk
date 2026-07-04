/**
 * @file surface/control_loop.cpp
 * @brief alchemy::ControlLoop implementation.
 */

#include "alchemy/surface/control_loop.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/surface/settings.h"
#include "daisy_seed.h"

namespace alchemy {

ControlLoop::ControlLoop(AlchemyLab& hw, uint32_t frame_ms, uint32_t poll_ms)
    : hw_(&hw),
      frame_ms_(frame_ms),
      poll_ms_(poll_ms ? poll_ms : 1u),
      renderer_(hw.Arc())
{
}

void ControlLoop::WireKnobs()
{
    /* Re-attach storage/locks/cv to every knob in every page. Called from
     * Tick() so runtime changes (knobs added/removed/moved between pages
     * by user code) are picked up on the next frame. */
    for (uint8_t i = 0; i < num_pages_; i++)
    {
        Page* p = pages_[i];
        const uint8_t idx = p->Index();
        for (uint8_t j = 0; j < p->Count(); j++)
        {
            VirtualKnob* k = p->At(j);
            if (k) WireKnob(*k, idx);
        }
    }
}

void ControlLoop::Tick()
{
    /* Re-wire every frame so dynamic Page::Add/Remove takes effect. Cheap
     * — just three pointer assigns per knob — and removes a class of bugs
     * where a knob moves between pages but the loop still has stale refs. */
    WireKnobs();

    const uint32_t now = daisy::System::GetNow();

    /* ── Inner poll: 1 ms cadence for button-edge detection + CV sampling.
     *    Every button-touching surface participates: Settings gates the
     *    perf surfaces while active so locks/storage don't see B1 edges
     *    that belong to settings page nav.
     *
     *    CV is sampled before CvSource::PollEdges so the source sees the
     *    freshest readings.  PollEdges runs gated like the button polls
     *    so a settings-mode user doesn't accidentally fire a tap-tempo
     *    edge while navigating settings pages. */
    for (uint32_t elapsed = 0u; elapsed < frame_ms_; elapsed += poll_ms_)
    {
        hw_->ProcessAllControls();
        const uint32_t t_ms = now + elapsed;

        for (uint8_t i = 0; i < num_cv_; i++) cv_[i] = hw_->cv[i].Value();

        if (settings_) settings_->PollButtons(t_ms);
        const bool gated = settings_ && settings_->IsActive();
        if (cv_source_) cv_source_->PollEdges(cv_, daisy::System::GetUs(), gated);
        if (locks_)   locks_  ->PollButtons(t_ms, gated);
        if (storage_) storage_->PollButtons(t_ms, gated);
        if (host_service_) host_service_->Poll(t_ms);
        if (on_poll_) on_poll_(t_ms);

        daisy::System::Delay(poll_ms_);
    }

    /* ── Snapshot physical pot positions for this frame. */
    for (uint8_t p = 0; p < num_pots_; p++) phys_[p] = hw_->pots[p].Value();

    /* ── Settings runs every frame so the B2+B3 enter gesture is always live.
     *    While active, perf surfaces and the user OnFrame hook stand down. */
    if (settings_) settings_->Update(phys_, now);
    const bool in_settings = settings_ && settings_->IsActive();

    if (!in_settings)
    {
        /* ── Surface updates in canonical order: cv_source first (refreshes
         *    its delta cache from the per-frame cv[] snapshot), then locks
         *    before storage so consume-button semantics land in the same
         *    frame as the release. */
        if (cv_source_) cv_source_->Update(cv_, now);
        if (locks_)   locks_  ->Update(phys_, now);
        if (storage_) storage_->Update(phys_, now);

        /* ── Page-change detection. */
        if (storage_)
        {
            const uint8_t p = storage_->ActivePage();
            if (p != last_page_)
            {
                last_page_ = p;
                if (on_page_change_) on_page_change_();
            }
        }

        /* ── User frame hook (typical: recompute DSP coefficients). */
        if (on_frame_) on_frame_();
    }

    /* ── Render. */
    Render(now);
}

void ControlLoop::Render(uint32_t t_ms)
{
    hw_->leds.Clear();

    /* Settings owns the screen while active. */
    if (settings_ && settings_->IsActive())
    {
        settings_->Render(t_ms);
        if (on_render_) on_render_(t_ms);
        hw_->leds.Show();
        return;
    }

    if (num_pages_ > 0)
    {
        /* Without storage, single-page modules render Page(0) always. */
        const uint8_t active = storage_ ? storage_->ActivePage() : 0;

        Page* page = nullptr;
        for (uint8_t i = 0; i < num_pages_; i++)
            if (pages_[i]->Index() == active) { page = pages_[i]; break; }

        if (page)
        {
            /* Build pot-indexed arrays for PerfRenderer. */
            ParamSlot slots   [ControlLoop::kMaxPagePots] = {};
            PotState  pots    [ControlLoop::kMaxPagePots] = {};
            float     combined[ControlLoop::kMaxPagePots] = {};

            const uint8_t n = num_pots_ < ControlLoop::kMaxPagePots
                                ? num_pots_
                                : ControlLoop::kMaxPagePots;
            for (uint8_t p = 0; p < n; p++)
            {
                pots[p]     = storage_ ? storage_->State(active, p)
                                       : PotState{phys_[p], true, 0};
                combined[p] = pots[p].stored;
            }

            for (uint8_t i = 0; i < page->Count(); i++)
            {
                VirtualKnob* k = page->At(i);
                if (!k)                continue;
                const uint8_t p = k->Pot();
                if (p >= n)            continue;
                slots[p]    = k->Slot();
                combined[p] = k->Norm();
            }

            renderer_.Render(hw_->leds, slots, pots, combined, n, t_ms);

            /* Custom-ring + overdraw callbacks on this page's knobs. */
            for (uint8_t i = 0; i < page->Count(); i++)
            {
                VirtualKnob* k = page->At(i);
                if (k && k->HasCustomRing())
                    k->CustomFn()(hw_->leds, k->Pot(), hw_->Arc(),
                                  k->Norm(), t_ms, k->CustomCtx());
            }
            for (uint8_t i = 0; i < page->Count(); i++)
            {
                VirtualKnob* k = page->At(i);
                if (k && k->HasOverdraw())
                    k->OverdrawFn()(hw_->leds, k->Pot(), hw_->Arc(),
                                    k->Norm(), t_ms, k->OverdrawCtx());
            }
        }

        /* Page indicator (only meaningful when storage advertises one). */
        if (storage_)
        {
            const LedPanel::Rgb c = storage_->PageColor(active);
            if (c.r || c.g || c.b) hw_->leds.SetButtonPair(0, c);
        }
    }

    /* Global OnRender hook fires before the LockSource overlay so the
     * overlay always sits on top of anything the user paints. */
    if (on_render_) on_render_(t_ms);

    if (locks_) locks_->Render(hw_->leds, t_ms);

    hw_->leds.Show();
}

} // namespace alchemy
