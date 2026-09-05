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
 *
 * Clocked mode (optional): give the manager a MusicalClock and set
 * LockSync::Clocked, and every slot that finishes recording snaps its
 * loop BOUNDARY to the nearest musical division (control/lock_snap.h).
 * The motion always replays at the speed it was performed; only the
 * restart is clock-derived, from absolute clock position, so tempo
 * changes move the boundary and nothing drifts.  Each slot records its
 * own timing (ParamLockSlot::musical_ticks), so the mode setting only
 * affects *new* recordings.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/control/lock_length.h"
#include "alchemy/control/lock_types.h"
#include "alchemy/control/pot_catch.h"

namespace alchemy {

class MusicalClock;

/** Maximum pots-per-page supported by the gesture engine. */
constexpr uint8_t kParamLockMaxGestureWidth = 8u;

/**
 * Per-slot header of the serialized form, followed by `stride` little-
 * endian uint16 samples.  `musical_ticks` is the Clocked loop length in
 * clock ticks (0 = free-running), so a clocked lock keeps its musical
 * length under any later tempo.
 */
struct ParamLockSavedHeader
{
    uint8_t  active;
    uint16_t length;
    uint16_t record_base;
    uint16_t musical_ticks;
} __attribute__((packed));

static_assert(sizeof(ParamLockSavedHeader) == 7u,
              "ParamLockSavedHeader must pack to 7 bytes");

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
    float          arm_threshold   = kParamLockArmThreshold;
    float          clear_threshold = kParamLockClearThreshold;
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

    /* ── Behavior configuration (all optional; see lock_types.h) ─────── */

    /** Musical clock consulted by Clocked mode.  May be null; Clocked
     *  recordings then fall back to free. */
    void SetClock(const MusicalClock* clk) { clock_ = clk; }
    bool HasClock() const                  { return clock_ != nullptr; }

    void     SetSyncMode(LockSync m) { sync_mode_ = m; }
    LockSync SyncMode() const        { return sync_mode_; }

    void    SetSnapGrid(uint8_t mask) { grid_mask_ = mask; }
    uint8_t SnapGrid() const          { return grid_mask_; }

    void SetArmThreshold  (float t) { arm_threshold_   = t; }
    void SetClearThreshold(float t) { clear_threshold_ = t; }

    /* ── Gesture ─────────────────────────────────────────────────────── */

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

    /** True if gesture lane @p i (0..gesture_width-1) armed or cleared a
     *  slot during the current / most recent hold; valid until the next
     *  OnButtonDown.  Lets the surface exit-policy exactly this hold's
     *  slots. */
    bool ActionTaken(uint8_t i) const
    {
        return i < kParamLockMaxGestureWidth && action_taken_[i];
    }

    /* ── Playback ────────────────────────────────────────────────────── */

    void Advance();

    /** Hold every play head in place (recording is unaffected).  Clocked
     *  slots re-anchor on resume so they continue from where they froze
     *  instead of jumping to where the clock got to. */
    void SetFrozen(bool on);
    bool Frozen() const { return frozen_; }

    float Read(uint8_t index) const;

    /** Loop phase 0..1 of an active slot's play head (0 for inactive). */
    float Phase(uint8_t index) const;

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

    /** Reset one slot to its default (inactive) state.  Safe against a
     *  concurrent audio-ISR Read(): `active` is dropped first, in its own
     *  store, before the rest of the slot is touched. */
    void ClearSlot(uint8_t index);

    /** Zero the motion arena.  Separate from Clear() because an arena in
     *  a NOLOAD section needs one explicit pass after the memory is up. */
    void ClearArena();

  private:
    void   Finalise();
    void   ActivateSlot(ParamLockSlot& lk);
    void   EmitSample(ParamLockSlot& lk, uint16_t* buf);
    float  SampleAt(const ParamLockSlot& lk, const uint16_t* buf) const;
    double NowTicks() const;

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
    float          arm_threshold_    = kParamLockArmThreshold;
    float          clear_threshold_  = kParamLockClearThreshold;
    const MusicalClock* clock_       = nullptr;
    LockSync       sync_mode_        = LockSync::Free;
    uint8_t        grid_mask_        = LockGrid::Default;
    bool           frozen_           = false;
    double         freeze_tick_      = 0.0;
    float          baseline_   [kParamLockMaxGestureWidth] = {};
    bool           action_taken_[kParamLockMaxGestureWidth] = {};
    bool           button_held_ = false;
    bool           consumed_    = false;
};

} // namespace alchemy
