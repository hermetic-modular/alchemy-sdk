/**
 * @file alchemy/control/lock_length.h
 * @brief Compile-time capacity policy for looping automation (ParamLock).
 *
 * See param-locks.md in docs
 */

#pragma once

#include <cstddef>
#include <cstdint>
#ifndef ALCHEMY_LOCK_INLINE_RAM_LIMIT
#define ALCHEMY_LOCK_INLINE_RAM_LIMIT (16u * 1024u)
#endif

namespace alchemy {

inline uint16_t LockQuant(float v)
{
    if (!(v > 0.0f)) return 0u;            /* also catches NaN */
    if (v >= 1.0f)   return 0xFFFFu;
    return static_cast<uint16_t>(v * 65535.0f + 0.5f);
}

inline float LockDequant(uint16_t q)
{
    return static_cast<float>(q) * (1.0f / 65535.0f);
}

/* ── Storage policy ───────────────────────────────────────────────────── */

/**
 * Whether a ParamLock's contents ride along in presets.
 *
 *   Preset  — serialized into every preset slot.  Budget-checked at
 *             compile time against kPresetBlobCapacity.
 *   RamOnly — never serialized; SerializedSize() is 0 and Manage()ing it
 *             costs nothing.  No flash ceiling, so this is the policy for
 *             "I want minute-long locks and I don't need them saved".
 *             Save()/Restore() still expose the byte form by hand.
 */
enum class LockStore : uint8_t
{
    Preset,
    RamOnly,
};

/* ── LockLength ───────────────────────────────────────────────────────── */

/**
 * Capacity policy: the second template argument to ParamLock.
 *
 * @tparam kSeconds Longest recordable gesture, in real seconds.  Honoured
 *                  regardless of the control-loop frame rate — ParamLock
 *                  is told the frame interval and clocks its buffer in
 *                  milliseconds, so `LockLength<20>` is twenty seconds at
 *                  16 ms frames and twenty seconds at 32 ms frames.
 * @tparam kRateHz  Stored motion sample rate (see header notes).
 * @tparam kStore   Preset participation (see LockStore).
 */
template<uint16_t kSecondsArg,
         uint16_t kRateHzArg = 30u,
         LockStore kStoreArg = LockStore::Preset>
struct LockLength
{
    static constexpr uint16_t  kMaxSeconds   = kSecondsArg;
    static constexpr uint16_t  kSampleRateHz = kRateHzArg;
    static constexpr LockStore kStorage      = kStoreArg;

    /** Motion samples reserved per lock slot. */
    static constexpr uint32_t kFramesPerSlot =
        static_cast<uint32_t>(kSecondsArg) * kRateHzArg;

    static_assert(kSecondsArg >= 1u,
                  "LockLength: kSeconds must be at least 1.");
    static_assert(kRateHzArg >= 5u,
                  "LockLength: kRateHz below 5 Hz cannot represent a hand "
                  "gesture. 20-30 Hz is the useful range.");
    static_assert(kFramesPerSlot <= 60000u,
                  "LockLength: kSeconds * kRateHz exceeds the uint16 sample "
                  "index range. Lower kRateHz (30 s at 20 Hz = 600 samples; "
                  "even 1000 s at 60 Hz is only 60000).");
};

using DefaultLockLength = LockLength<16u, 30u, LockStore::Preset>;

/* ── LockArena ────────────────────────────────────────────────────────── */

/**
 * Externally-placed motion storage for a ParamLock too large to hold its
 * own arena (see ALCHEMY_LOCK_INLINE_RAM_LIMIT).
 */
struct LockArena
{
    uint16_t* data  = nullptr;
    size_t    bytes = 0u;
};

/* ── Sample clock ─────────────────────────────────────────────────────── */

/**
 * Motion samples advanced per control frame, in Q16 fixed point.
 */
inline uint32_t LockStepQ16(uint16_t rate_hz, uint32_t frame_ms)
{
    constexpr uint64_t kMaxStep = 4096ull << 16;

    const uint64_t num = static_cast<uint64_t>(rate_hz)
                       * static_cast<uint64_t>(frame_ms) * 65536ull;
    uint64_t step = (num + 500ull) / 1000ull;
    if (step < 1ull)      step = 1ull;
    if (step > kMaxStep)  step = kMaxStep;
    return static_cast<uint32_t>(step);
}

} // namespace alchemy
