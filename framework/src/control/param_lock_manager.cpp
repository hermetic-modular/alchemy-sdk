/**
 * @file param_lock_manager.cpp
 */

#include "alchemy/control/param_lock_manager.h"

#include <cstring>

namespace alchemy {

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void ParamLockManager::Init(const LockConfig& cfg)
{
    /* Stay inert rather than half-initialise into a null arena. */
    if (cfg.slots == nullptr || cfg.arena == nullptr
        || cfg.stride == 0u || cfg.total_slots == 0u)
    {
        slots_  = nullptr;
        arena_  = nullptr;
        stride_ = 0u;
        total_slots_ = 0u;
        return;
    }

    slots_         = cfg.slots;
    arena_         = cfg.arena;
    stride_        = cfg.stride;
    total_slots_   = cfg.total_slots;
    gesture_width_ = cfg.gesture_width < kParamLockMaxGestureWidth
                         ? cfg.gesture_width
                         : kParamLockMaxGestureWidth;
    rate_hz_           = cfg.rate_hz;
    gesture_threshold_ = cfg.gesture_threshold;
    step_q16_          = LockStepQ16(cfg.rate_hz, cfg.frame_ms);
    rec_phase_q16_     = 0u;
    button_held_       = false;
    consumed_          = false;

    for (uint8_t i = 0; i < gesture_width_; i++)
    {
        baseline_[i]     = 0.5f;
        action_taken_[i] = false;
    }
}

void ParamLockManager::SetFrameMs(uint32_t frame_ms)
{
    step_q16_ = LockStepQ16(rate_hz_, frame_ms);
}

/* ── Gesture ──────────────────────────────────────────────────────────── */

void ParamLockManager::OnButtonDown(const float phys[])
{
    button_held_ = true;
    /* Restart the record clock with the gesture. */
    rec_phase_q16_ = 0u;
    for (uint8_t i = 0; i < gesture_width_; i++)
    {
        baseline_[i]     = phys[i];
        action_taken_[i] = false;
    }
}

bool ParamLockManager::OnButtonUp()
{
    button_held_ = false;
    Finalise();
    const bool was_consumed = consumed_;
    consumed_ = false;
    return was_consumed;
}

void ParamLockManager::ProcessGestures(uint8_t offset, const float phys[])
{
    if (!button_held_ || !IsReady())
        return;

    /* Once per frame, ahead of the per-pot loop, so locks armed together
     * stay sample-aligned.  `pending` exceeds 1 only when the control
     * frame is slower than the stored rate — a zero-order hold, rather
     * than silently running the recording fast. */
    rec_phase_q16_ += step_q16_;
    const uint32_t pending = rec_phase_q16_ >> 16;
    rec_phase_q16_ &= 0xFFFFu;

    for (uint8_t i = 0; i < gesture_width_; i++)
    {
        const uint8_t idx = static_cast<uint8_t>(offset + i);
        if (idx >= total_slots_)
            break;

        ParamLockSlot& lk    = slots_[idx];
        const float    diff  = phys[i] - baseline_[i];
        const float    delta = diff > 0.0f ? diff : -diff;
        const bool     nudged = (delta >= gesture_threshold_);

        if (nudged && !lk.recording && !action_taken_[i])
        {
            action_taken_[i] = true;
            consumed_        = true;
            baseline_[i]     = phys[i];

            if (lk.active)
            {
                lk = ParamLockSlot{};
            }
            else
            {
                lk = ParamLockSlot{};
                lk.recording   = true;
                lk.record_base = LockQuant(phys[i]);
            }
        }

        if (!lk.recording)
            continue;

        /* Box-average, so a fast flick between two stored samples is
         * attenuated rather than aliased into a spike. */
        lk.rec_accum += phys[i];
        lk.rec_count++;

        for (uint32_t n = 0; n < pending && lk.recording; n++)
            EmitSample(lk, Buf(idx));
    }
}

void ParamLockManager::EmitSample(ParamLockSlot& lk, uint16_t* buf)
{
    if (lk.length < stride_)
    {
        /* No frames accumulated (the 2nd..Nth emit of a slow control
         * frame): hold the previous sample.  Falling back to record_base
         * here would interleave the arm position into every gap. */
        uint16_t q;
        if (lk.rec_count)
            q = LockQuant(lk.rec_accum / static_cast<float>(lk.rec_count));
        else
            q = lk.length ? buf[lk.length - 1u] : lk.record_base;

        buf[lk.length++] = q;
        lk.rec_accum = 0.0f;
        lk.rec_count = 0u;
    }

    if (lk.length >= stride_)
    {
        /* Buffer full: loop what was captured rather than drop the tail.
         * `active` publishes last — the ISR-side Read() gates on it. */
        lk.recording = false;
        lk.play_pos  = 0u;
        lk.active    = true;
    }
}

void ParamLockManager::Finalise()
{
    if (!IsReady())
        return;

    for (uint8_t i = 0; i < total_slots_; i++)
    {
        ParamLockSlot& lk = slots_[i];
        if (!lk.recording)
            continue;

        /* Flush the partial sample in flight.  Without this a gesture
         * shorter than one sample period (40 ms at 20 Hz) records
         * nothing and leaves a slot that is neither recording nor
         * active. */
        if (lk.rec_count && lk.length < stride_)
            EmitSample(lk, Buf(i));

        /* `active` last, same reasoning as EmitSample. */
        lk.recording = false;
        lk.play_pos  = 0u;
        lk.rec_accum = 0.0f;
        lk.rec_count = 0u;
        lk.active    = (lk.length > 0);
    }
}

/* ── Playback ─────────────────────────────────────────────────────────── */

void ParamLockManager::Advance()
{
    if (!IsReady()) return;

    for (uint8_t i = 0; i < total_slots_; i++)
    {
        ParamLockSlot& lk = slots_[i];
        if (!lk.active || lk.length == 0) continue;

        /* Compute the whole new position, then publish it as ONE aligned
         * 32-bit store.  The audio ISR reads play_pos between any two of
         * these statements. */
        uint32_t pos  = lk.play_pos + step_q16_;
        uint32_t head = pos >> 16;
        if (head >= lk.length)
            head %= lk.length;
        lk.play_pos = (head << 16) | (pos & 0xFFFFu);
    }
}

float ParamLockManager::SampleAt(const ParamLockSlot& lk,
                                 const uint16_t* buf) const
{
    /* One aligned read, so index and fraction are always coherent even
     * when this runs in the audio ISR mid-Advance. */
    const uint32_t pos  = lk.play_pos;
    const uint16_t head = static_cast<uint16_t>(pos >> 16);
    const uint16_t next = static_cast<uint16_t>(
        (static_cast<uint32_t>(head) + 1u) % lk.length);

    const float a = LockDequant(buf[head]);
    const float b = LockDequant(buf[next]);
    const float f = static_cast<float>(pos & 0xFFFFu) * (1.0f / 65536.0f);

    /* Interpolate rather than hold: at 20-30 Hz a held sample steps
     * audibly on a cutoff or pitch.  When a == b this returns exactly a,
     * so a stationary lock contributes exactly 0.0. */
    return a + (b - a) * f;
}

float ParamLockManager::Read(uint8_t index) const
{
    if (!IsReady() || index >= total_slots_) return 0.0f;

    const ParamLockSlot& lk = slots_[index];
    if (!lk.active || lk.length == 0) return 0.0f;

    return SampleAt(lk, Buf(index)) - LockDequant(lk.record_base);
}

bool ParamLockManager::IsActive(uint8_t index) const
{
    return IsReady() && index < total_slots_ && slots_[index].active;
}

bool ParamLockManager::IsRecording(uint8_t index) const
{
    return IsReady() && index < total_slots_ && slots_[index].recording;
}

/* ── Serialized form ──────────────────────────────────────────────────── */

void ParamLockManager::CaptureSlot(uint8_t index, uint8_t* out) const
{
    const size_t sample_bytes =
        static_cast<size_t>(stride_) * sizeof(uint16_t);

    const bool valid = IsReady()
                    && index < total_slots_
                    && slots_[index].active
                    && slots_[index].length > 0;

    ParamLockSavedHeader h = {0u, 0u, 0u};

    if (!valid)
    {
        std::memcpy(out, &h, sizeof h);
        std::memset(out + sizeof h, 0, sample_bytes);
        return;
    }

    const ParamLockSlot& lk = slots_[index];
    h.active      = 1u;
    h.length      = lk.length;
    h.record_base = lk.record_base;
    std::memcpy(out, &h, sizeof h);

    const size_t used = static_cast<size_t>(lk.length) * sizeof(uint16_t);
    std::memcpy(out + sizeof h, Buf(index), used);
    std::memset(out + sizeof h + used, 0, sample_bytes - used);
}

void ParamLockManager::RestoreSlot(uint8_t index, const uint8_t* in)
{
    if (!IsReady() || index >= total_slots_)
        return;

    ParamLockSlot& lk = slots_[index];
    lk = ParamLockSlot{};   /* active=false while we rebuild below */

    ParamLockSavedHeader h;
    std::memcpy(&h, in, sizeof h);

    if (!h.active || h.length == 0u || h.length > stride_)
        return;

    std::memcpy(Buf(index), in + sizeof h,
                static_cast<size_t>(h.length) * sizeof(uint16_t));

    lk.length      = h.length;
    lk.record_base = h.record_base;
    lk.active      = true;
}

void ParamLockManager::Clear()
{
    if (!IsReady())
        return;
    for (uint8_t i = 0; i < total_slots_; i++)
        slots_[i] = ParamLockSlot{};
}

void ParamLockManager::ClearArena()
{
    if (!IsReady())
        return;
    std::memset(arena_, 0,
                static_cast<size_t>(total_slots_) * stride_ * sizeof(uint16_t));
}

} // namespace alchemy
