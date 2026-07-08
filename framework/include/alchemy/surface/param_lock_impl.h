/**
 * @file alchemy/surface/param_lock_impl.h
 * @brief Template implementation of alchemy::ParamLock.  Included from param_lock.h.
 */

#pragma once

#include "alchemy/surface/param_lock.h"
#include "alchemy/led/panel.h"

namespace alchemy {

template<uint8_t kSlots>
void ParamLock<kSlots>::PollButtons(uint32_t /*t_ms*/, bool gated)
{
    const bool pressed = trigger_->Pressed();
    const bool rising  = pressed && !prev_pressed_;
    const bool falling = !pressed && prev_pressed_;
    prev_pressed_ = pressed;
    if (gated) return;
    if (rising)  rising_pending_  = true;
    if (falling) falling_pending_ = true;
}

template<uint8_t kSlots>
void ParamLock<kSlots>::Update(const float* phys, uint32_t /*t_ms*/)
{
    /* Apply edges detected by PollButtons. Rising is processed first so a
     * fast tap (both flags set in the same frame) sees OnButtonDown before
     * OnButtonUp. phys captured here is the per-frame sample — up to one
     * frame stale relative to the actual button press, well below human
     * nudge speed. */
    if (rising_pending_)
    {
        rising_pending_ = false;
        mgr_.OnButtonDown(phys);
    }
    if (falling_pending_)
    {
        falling_pending_ = false;
        const bool consumed = mgr_.OnButtonUp();
        if (consumed && pager_) pager_->ConsumeButton();
    }

    if (mgr_.IsButtonHeld())
        mgr_.ProcessGestures(Offset(), phys);
}

template<uint8_t kSlots>
void ParamLock<kSlots>::Advance()
{
    mgr_.Advance();
}

template<uint8_t kSlots>
float ParamLock<kSlots>::Delta(uint8_t pot) const
{
    return mgr_.Read(static_cast<uint8_t>(Offset() + pot));
}

template<uint8_t kSlots>
bool ParamLock<kSlots>::IsActive(uint8_t pot) const
{
    return mgr_.IsActive(static_cast<uint8_t>(Offset() + pot));
}

template<uint8_t kSlots>
bool ParamLock<kSlots>::IsRecording(uint8_t pot) const
{
    return mgr_.IsRecording(static_cast<uint8_t>(Offset() + pot));
}

template<uint8_t kSlots>
float ParamLock<kSlots>::DeltaAtPage(uint8_t page, uint8_t pot) const
{
    return mgr_.Read(static_cast<uint8_t>(page * Width() + pot));
}

template<uint8_t kSlots>
bool ParamLock<kSlots>::IsActiveAtPage(uint8_t page, uint8_t pot) const
{
    return mgr_.IsActive(static_cast<uint8_t>(page * Width() + pot));
}

template<uint8_t kSlots>
bool ParamLock<kSlots>::IsRecordingAtPage(uint8_t page, uint8_t pot) const
{
    return mgr_.IsRecording(static_cast<uint8_t>(page * Width() + pot));
}

template<uint8_t kSlots>
bool ParamLock<kSlots>::AnyRecording() const
{
    const uint8_t off = Offset();
    const uint8_t w   = Width();
    for (uint8_t i = 0; i < w; i++)
        if (mgr_.IsRecording(static_cast<uint8_t>(off + i))) return true;
    return false;
}

template<uint8_t kSlots>
bool ParamLock<kSlots>::AnyActive() const
{
    const uint8_t off = Offset();
    const uint8_t w   = Width();
    for (uint8_t i = 0; i < w; i++)
        if (mgr_.IsActive(static_cast<uint8_t>(off + i))) return true;
    return false;
}

template<uint8_t kSlots>
void ParamLock<kSlots>::Render(LedPanel& panel, uint32_t /*t_ms*/) const
{
    const uint8_t off = Offset();
    const uint8_t w   = Width();
    for (uint8_t i = 0; i < w; i++)
    {
        const uint8_t idx = static_cast<uint8_t>(off + i);
        const LedPanel::Rgb red   = {0xFF, 0x00, 0x00};
        const LedPanel::Rgb green = {0x00, 0xFF, 0x40};
        if (mgr_.IsRecording(idx))
            panel.SetRingByHour(i, 6.0f, red);
        else if (mgr_.IsActive(idx))
            panel.SetRingByHour(i, 6.0f, green);
    }
}

} // namespace alchemy
