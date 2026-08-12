/**
 * @file param_lock_manager.h
 * @brief Internal looping-automation engine used by the ParamLock surface.
 *
 * ParamLockManager is a low-level primitive: it owns nothing, allocates
 * nothing, and is driven by an external array of ParamLockSlot headers
 * plus a flat motion arena.  The public-facing API for application code
 * is the `alchemy::ParamLock` surface in surface/param_lock.h — this
 * header is the algorithm it delegates to.
 *
 *
 * Lifecycle (driven by the surface):
 *
 *   mgr.Init(cfg);
 *   on button down:                 mgr.OnButtonDown(phys);
 *   while button held, each tick:   mgr.ProcessGestures(offset, phys);
 *   on button up:                   bool consumed = mgr.OnButtonUp();
 *   every frame:                    mgr.Advance();         (advances all heads)
 *   when reading slot N:            float mod = mgr.Read(N);
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/control/lock_length.h"
#include "alchemy/control/pot_catch.h"

namespace alchemy {

/** Maximum pots-per-page supported by the gesture engine. */
constexpr uint8_t kParamLockMaxGestureWidth = 8u;

/**
 * Per-slot header of the serialized form, followed by `stride` little-
 * endian uint16 samples.
 */
struct ParamLockSavedHeader
{
    uint8_t  active;
    uint16_t length;
    uint16_t record_base;
} __attribute__((packed));

static_assert(sizeof(ParamLockSavedHeader) == 5u,
              "ParamLockSavedHeader must pack to 5 bytes");

/** Configuration handed to ParamLockManager::Init. */
struct LockConfig
{
    ParamLockSlot* slots       = nullptr;  ///< caller-owned header array
    uint16_t*      arena       = nullptr;  ///< caller-owned sample storage
    uint16_t       stride      = 0u;       ///< samples reserved per slot
    uint8_t        total_slots = 0u;       ///< length of @c slots
    uint8_t        gesture_width = 0u;     ///< pots per gesture frame
    uint16_t       rate_hz     = 30u;      ///< stored motion sample rate
    uint32_t       frame_ms    = 16u;      ///< control-frame interval
    float          gesture_threshold = kParamLockGestureThreshold;
};

class ParamLockManager
{
  public:
    /**
     * Bind the manager to caller-owned storage.
     *
     * Does not write to @c cfg.arena — safe to call from a static
     * constructor even when the arena lives in memory that is not yet
     * usable (SDRAM before the FMC is up).
     */
    void Init(const LockConfig& cfg);

    void SetFrameMs(uint32_t frame_ms);

    /** True once Init() has been given a usable configuration. */
    bool IsReady() const { return slots_ != nullptr && arena_ != nullptr; }

    /** Called when the trigger button goes down.  Snapshots baselines. */
    void OnButtonDown(const float phys[]);

    /** Called when the trigger button goes up.  Returns true if any gesture fired. */
    bool OnButtonUp();

    /**
     * Process one gesture frame.  Call every tick while button may be held.
     * @param offset  Index of the first slot to consider (e.g. page * pots).
     * @param phys    Physical pot positions; length must equal gesture_width.
     */
    void ProcessGestures(uint8_t offset, const float phys[]);

    void Advance();

    float Read(uint8_t index) const;

    bool IsActive   (uint8_t index) const;
    bool IsRecording(uint8_t index) const;
    bool IsButtonHeld() const { return button_held_; }
    bool WasConsumed()  const { return consumed_; }

    /* ── Serialized form ─────────────────────────────────────────────── */

    size_t SavedBytesPerSlot() const
    {
        return sizeof(ParamLockSavedHeader)
             + static_cast<size_t>(stride_) * sizeof(uint16_t);
    }

    /**
     * Write slot @p index into @p out as SavedBytesPerSlot() bytes.
     *
     * Samples past `length` are zero-filled so the byte image is a pure
     * function of the slot's musical content — two devices that recorded
     * the same gesture produce the same CRC.
     */
    void CaptureSlot(uint8_t index, uint8_t* out) const;

    void RestoreSlot(uint8_t index, const uint8_t* in);

    void Clear();

    /** Zero the motion arena.  Separate from Clear() because an arena in
     *  a NOLOAD section needs one explicit pass after the memory is up. */
    void ClearArena();

  private:
    void  Finalise();
    void  EmitSample(ParamLockSlot& lk, uint16_t* buf);
    float SampleAt(const ParamLockSlot& lk, const uint16_t* buf) const;

    uint16_t*      Buf(uint8_t index)
    {
        return arena_ + static_cast<size_t>(index) * stride_;
    }
    const uint16_t* Buf(uint8_t index) const
    {
        return arena_ + static_cast<size_t>(index) * stride_;
    }

    ParamLockSlot* slots_            = nullptr;
    uint16_t*      arena_            = nullptr;
    uint16_t       stride_           = 0;
    uint8_t        total_slots_      = 0;
    uint8_t        gesture_width_    = 0;
    uint16_t       rate_hz_          = 30;
    uint32_t       step_q16_         = 0;
    uint32_t       rec_phase_q16_    = 0;
    float          gesture_threshold_= kParamLockGestureThreshold;
    float          baseline_   [kParamLockMaxGestureWidth] = {};
    bool           action_taken_[kParamLockMaxGestureWidth] = {};
    bool           button_held_ = false;
    bool           consumed_    = false;
};

} // namespace alchemy
