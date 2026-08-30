/**
 * @file alchemy/surface/param_lock_impl.h
 * @brief Template implementation of alchemy::ParamLock.  Included from param_lock.h.
 */

#pragma once

#include <cstring>

#include "alchemy/surface/param_lock.h"
#include "alchemy/led/panel.h"

namespace alchemy {

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::Save(uint8_t* out) const
{
    /* An inert surface (external arena refused) still owes the caller a
     * full, well-formed image: SerializedSize() promised kSavedBytes, and
     * the manager — which has no stride to work from — would write only
     * the 7-byte headers, leaving the caller's buffer uninitialised in
     * the gaps and putting stack residue into a preset slot. */
    if (!mgr_.IsReady())
    {
        std::memset(out, 0, kSavedBytes);
        return;
    }

    for (uint8_t i = 0; i < kSlots; i++, out += kBytesPerSavedSlot)
        mgr_.CaptureSlot(i, out);
}

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::Restore(const uint8_t* in)
{
    for (uint8_t i = 0; i < kSlots; i++, in += kBytesPerSavedSlot)
        mgr_.RestoreSlot(i, in);
}

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::PollButtons(uint32_t /*t_ms*/, bool gated)
{
    const bool pressed = trigger_->Pressed();
    const bool rising  = pressed && !prev_pressed_;
    const bool falling = !pressed && prev_pressed_;
    prev_pressed_ = pressed;
    if (gated) return;
    if (rising)  rising_pending_  = true;
    if (falling) falling_pending_ = true;
}

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::Update(const float* phys, uint32_t /*t_ms*/)
{
    /* Apply edges detected by PollButtons. Rising is processed first so a
     * fast tap (both flags set in the same frame) sees OnButtonDown before
     * OnButtonUp. phys captured here is the per-frame sample — up to one
     * frame stale relative to the actual button press, well below human
     * nudge speed. */
    if (rising_pending_)
    {
        rising_pending_ = false;
        /* Bank at the press edge; the view latch keeps a layer the
         * recording started on showing until release. */
        held_offset_ = Offset();
        if (pager_) pager_->RetainPage();
        mgr_.OnButtonDown(phys);
    }
    if (falling_pending_)
    {
        falling_pending_ = false;
        const bool consumed = mgr_.OnButtonUp();
        if (pager_) pager_->ReleasePage();
        if (consumed && pager_ && pager_->ConsumesRelease(trigger_))
            pager_->ConsumeButton();

        /* Return exit: every slot THIS hold armed snaps its base back to
         * the gesture origin and re-arms pot catch.  Cleared slots ended
         * inactive, so ActionTaken ∧ IsActive excludes them.  Resolves
         * against the press-edge bank, so a mid-hold page change cannot
         * retarget the snap. */
        if (consumed && exit_mode_ == LockExit::Return && pager_)
        {
            const uint8_t off = held_offset_;
            const uint8_t w   = Width();
            const uint8_t pg  = static_cast<uint8_t>(off / w);
            for (uint8_t i = 0; i < w; i++)
            {
                const uint8_t idx = static_cast<uint8_t>(off + i);
                if (idx >= kSlots) break;
                if (mgr_.ActionTaken(i) && mgr_.IsActive(idx))
                    pager_->SetStored(pg, i,
                                      LockDequant(slots_[idx].record_base),
                                      phys);
            }
        }
    }

    if (mgr_.IsButtonHeld())
        mgr_.ProcessGestures(held_offset_, phys);
}

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::Advance()
{
    mgr_.Advance();
}

template<uint8_t kSlots, class Len>
float ParamLock<kSlots, Len>::Delta(uint8_t pot) const
{
    return mgr_.Read(static_cast<uint8_t>(Offset() + pot));
}

template<uint8_t kSlots, class Len>
bool ParamLock<kSlots, Len>::IsActive(uint8_t pot) const
{
    return mgr_.IsActive(static_cast<uint8_t>(Offset() + pot));
}

template<uint8_t kSlots, class Len>
bool ParamLock<kSlots, Len>::IsRecording(uint8_t pot) const
{
    return mgr_.IsRecording(static_cast<uint8_t>(Offset() + pot));
}

template<uint8_t kSlots, class Len>
float ParamLock<kSlots, Len>::DeltaAtPage(uint8_t page, uint8_t pot) const
{
    return mgr_.Read(static_cast<uint8_t>(page * Width() + pot));
}

template<uint8_t kSlots, class Len>
bool ParamLock<kSlots, Len>::IsActiveAtPage(uint8_t page, uint8_t pot) const
{
    return mgr_.IsActive(static_cast<uint8_t>(page * Width() + pot));
}

template<uint8_t kSlots, class Len>
bool ParamLock<kSlots, Len>::IsRecordingAtPage(uint8_t page, uint8_t pot) const
{
    return mgr_.IsRecording(static_cast<uint8_t>(page * Width() + pot));
}

template<uint8_t kSlots, class Len>
bool ParamLock<kSlots, Len>::AnyRecording() const
{
    const uint8_t off = Offset();
    const uint8_t w   = Width();
    for (uint8_t i = 0; i < w; i++)
        if (mgr_.IsRecording(static_cast<uint8_t>(off + i))) return true;
    return false;
}

template<uint8_t kSlots, class Len>
bool ParamLock<kSlots, Len>::AnyActive() const
{
    const uint8_t off = Offset();
    const uint8_t w   = Width();
    for (uint8_t i = 0; i < w; i++)
        if (mgr_.IsActive(static_cast<uint8_t>(off + i))) return true;
    return false;
}

template<uint8_t kSlots, class Len>
float ParamLock<kSlots, Len>::PlayPhaseAtPage(uint8_t page, uint8_t pot) const
{
    const uint8_t idx = static_cast<uint8_t>(page * Width() + pot);
    if (idx >= kSlots) return 0.0f;
    if (mgr_.IsRecording(idx))
        return static_cast<float>(slots_[idx].length)
             / static_cast<float>(kFramesPerSlot);
    return mgr_.Phase(idx);
}

template<uint8_t kSlots, class Len>
float ParamLock<kSlots, Len>::PlayPhase(uint8_t pot) const
{
    return PlayPhaseAtPage(pager_ ? pager_->Page() : 0u, pot);
}

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::ClearSlot(uint8_t pot)
{
    mgr_.ClearSlot(static_cast<uint8_t>(Offset() + pot));
}

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::ClearSlotAtPage(uint8_t page, uint8_t pot)
{
    mgr_.ClearSlot(static_cast<uint8_t>(page * Width() + pot));
}

template<uint8_t kSlots, class Len>
void ParamLock<kSlots, Len>::Render(LedPanel& panel, uint32_t t_ms) const
{
    const uint8_t off = Offset();
    const uint8_t w   = Width();
    for (uint8_t i = 0; i < w; i++)
    {
        const uint8_t idx       = static_cast<uint8_t>(off + i);
        const bool    recording = mgr_.IsRecording(idx);
        if (!recording && !mgr_.IsActive(idx))
            continue;

        const LockStyle&    st = recording ? record_style_ : play_style_;
        const LockAnimFrame f  =
            LockAnimEval(st.anim, t_ms, PlayPhase(i), recording);
        if (f.level <= 0.0f)
            continue;

        /* Sweep orbits clockwise from the 6-o'clock home pip. */
        float hour = f.sweep ? 6.0f + f.sweep_pos01 * 12.0f : 6.0f;
        if (hour >= 12.0f) hour -= 12.0f;
        panel.SetRingByHour(i, hour, LedPanel::Scale(st.color, f.level));
    }
}

} // namespace alchemy
