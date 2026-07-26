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
     * the 5-byte headers, leaving the caller's buffer uninitialised in
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
void ParamLock<kSlots, Len>::Render(LedPanel& panel, uint32_t /*t_ms*/) const
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
