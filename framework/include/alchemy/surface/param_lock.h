/**
 * @file alchemy/surface/param_lock.h
 * @brief alchemy::ParamLock — opt-in looping-automation surface.
 *
 * Owns an array of automation slots and the trigger button's gesture
 * state.  Advance() moves every slot's play head each frame; Delta(p)
 * reads the current modulation for pot p on the visible page (resolved
 * via an optional Pager) without advancing.
 *
 * TODO: Update to use Matthew's sparse sampling, which will be sick.
 *
 * Two template parameters — how many slots, and how long each one is:
 *
 *   alchemy::ParamLock<6>                      locks(hw.buttons[0]);
 *   alchemy::ParamLock<6, LockLength<20>>      locks(hw.buttons[0]);
 *   alchemy::ParamLock<12, LockLength<30, 20>> locks(hw.buttons[0], pager);
 *
 * Length defaults to DefaultLockLength (16 s at 30 Hz).  Both the RAM
 * and preset-slot budgets are checked at the declaration site; see
 * alchemy/control/lock_length.h and docs/param-locks.md.
 *
 * The paged form resolves Delta(p) as pager.Page() * pager.NumPots() + p,
 * and kSlots must equal pager.NumPages() * pager.NumPots().  Advance()
 * moves all kSlots heads so inactive pages keep cycling; it is separate
 * from Update() (gestures) so playback survives modes — like the Settings
 * menu — that suspend gesture handling.
 *
 * Composition at the user's read site:
 *
 *   float knob(uint8_t p, void*) {
 *       return pager.Value(p) + locks.Delta(p) + cv.Delta(p, cv_buf, n);
 *   }
 *
 * Saving into a preset is just presets.Manage(locks).  Save()/Restore()
 * expose the raw bytes by hand — the only route for LockStore::RamOnly.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/control/lock_length.h"
#include "alchemy/control/param_lock_manager.h"
#include "alchemy/control/pot_catch.h"
#include "alchemy/control/preset_capacity.h"
#include "alchemy/hw/i_button.h"
#include "alchemy/surface/lock_source.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/serializable.h"

#define ALCHEMY_LOCK_OVERSIZE_MESSAGE                                          \
    "ParamLock's motion arena exceeds ALCHEMY_LOCK_INLINE_RAM_LIMIT (16 KiB "  \
    "by default). Framework statics link into DTCMRAM, which is 128 KiB "      \
    "shared with the stack, so an arena this size must be placed "             \
    "deliberately: declare `uint16_t ALCHEMY_SDRAM_BSS "                       \
    "arena[Locks::kArenaSamples];`, pass `alchemy::LockArena{arena, sizeof "   \
    "arena}` to the constructor, and call locks.Init() from main() after "     \
    "hw.Init(). Or reduce kSlots / kSeconds / kRateHz. See "                   \
    "ParamLock<>::kArenaBytes."

#define ALCHEMY_LOCK_UNDERSIZE_MESSAGE                                         \
    "This ParamLock configuration holds its motion arena inline "              \
    "(kArenaBytes is within ALCHEMY_LOCK_INLINE_RAM_LIMIT), so passing a "     \
    "LockArena would allocate the storage twice. Drop the LockArena "          \
    "argument. If you need the arena placed outside DTCM anyway, build with "  \
    "-DALCHEMY_LOCK_INLINE_RAM_LIMIT=0."

namespace alchemy {

class LedPanel;

/* Schema tag for ParamLock — bump if the saved layout changes shape.
 * 'PLK1': uint16 motion samples at a configurable rate, variable length. */
inline constexpr uint32_t kParamLockSchemaTag = 0x504C4B31u; /* 'PLK1' */

namespace detail {

/** Conditional inline motion storage — specialised away entirely past
 *  ALCHEMY_LOCK_INLINE_RAM_LIMIT, so an external arena never pays for a
 *  shadow copy inside the object. */
template<uint32_t kSamples, bool kInline>
struct LockArenaStore;

template<uint32_t kSamples>
struct LockArenaStore<kSamples, true>
{
    uint16_t  data[kSamples] = {};
    uint16_t* Ptr() { return data; }
};

template<uint32_t kSamples>
struct LockArenaStore<kSamples, false>
{
    uint16_t* Ptr() { return nullptr; }
};

} // namespace detail

template<uint8_t kSlots, class Len = DefaultLockLength>
class ParamLock : public Serializable, public LockSource
{
  public:
    /* ── Configuration ───────────────────────────────────────────────
     * Compile-time constants — print at boot or static_assert against. */

    static constexpr uint8_t   kNumSlots      = kSlots;
    static constexpr uint16_t  kMaxSeconds    = Len::kMaxSeconds;
    static constexpr uint16_t  kSampleRateHz  = Len::kSampleRateHz;
    static constexpr LockStore kStorage       = Len::kStorage;

    /** Motion samples reserved per slot. */
    static constexpr uint16_t kFramesPerSlot =
        static_cast<uint16_t>(Len::kFramesPerSlot);

    /** Total motion samples across all slots (the arena length). */
    static constexpr uint32_t kArenaSamples =
        static_cast<uint32_t>(kSlots) * kFramesPerSlot;

    /** Motion arena size in bytes. */
    static constexpr size_t kArenaBytes =
        static_cast<size_t>(kArenaSamples) * sizeof(uint16_t);

    /** True when the arena lives inside the object (see LockArena). */
    static constexpr bool kInlineArena =
        (kArenaBytes <= static_cast<size_t>(ALCHEMY_LOCK_INLINE_RAM_LIMIT));

    /** Bytes one slot occupies in the serialized byte form. */
    static constexpr size_t kBytesPerSavedSlot =
        sizeof(ParamLockSavedHeader)
        + static_cast<size_t>(kFramesPerSlot) * sizeof(uint16_t);

    /** Full serialized byte form (Save()/Restore()), regardless of policy. */
    static constexpr size_t kSavedBytes =
        static_cast<size_t>(kSlots) * kBytesPerSavedSlot;

    /** Contribution to every preset slot — 0 for LockStore::RamOnly. */
    static constexpr size_t kPresetBytes =
        (kStorage == LockStore::Preset) ? kSavedBytes : size_t{0};

    /** Preset-slot bytes left for every other managed component. */
    static constexpr size_t kPresetBytesFree =
        (kPresetBytes < kPresetBlobCapacity)
            ? (kPresetBlobCapacity - kPresetBytes) : size_t{0};

    /** Total RAM this surface costs, object and arena together. */
    static constexpr size_t RamBytes();

    /* ── Compile-time budget ─────────────────────────────────────────── */

    static_assert(kSlots > 0u, "ParamLock: kSlots must be at least 1.");

    static_assert(kPresetBytes <= kPresetBlobCapacity,
        "ParamLock exceeds one preset slot (kPresetBlobCapacity, 20448 B). "
        "The preset region is the top 640 KiB of the QSPI chip and cannot "
        "grow. Reduce kSlots, kSeconds, or kRateHz — dropping the rate is "
        "usually free, since a hand gesture carries nothing above ~8 Hz "
        "(30 s x 12 slots fits at 20 Hz) — or declare the length with "
        "LockStore::RamOnly to keep these locks out of presets entirely. "
        "Compare ParamLock<>::kPresetBytes against alchemy::kPresetBlobCapacity.");

    /* ── Construction ────────────────────────────────────────────────── */

    /** Unpaged form: kSlots is the pot count, Delta(p) reads slot p. */
    explicit ParamLock(IButton& trigger)
        : trigger_(&trigger), pager_(nullptr)
    {
        static_assert(kInlineArena, ALCHEMY_LOCK_OVERSIZE_MESSAGE);
        Bind(arena_.Ptr(), kSlots);
    }

    /**
     * Paged form: kSlots must equal pager.NumPages() * pager.NumPots().
     * Delta(p) reads slot (pager.Page() * pager.NumPots() + p).
     */
    ParamLock(IButton& trigger, Pager& pgr)
        : trigger_(&trigger), pager_(&pgr)
    {
        static_assert(kInlineArena, ALCHEMY_LOCK_OVERSIZE_MESSAGE);
        Bind(arena_.Ptr(), pgr.NumPots());
    }

    /** Unpaged form with application-placed motion storage.  Required
     *  past ALCHEMY_LOCK_INLINE_RAM_LIMIT; see LockArena for placement
     *  rules.  A short arena is refused (IsReady() stays false). */
    ParamLock(IButton& trigger, LockArena arena)
        : trigger_(&trigger), pager_(nullptr)
    {
        static_assert(!kInlineArena, ALCHEMY_LOCK_UNDERSIZE_MESSAGE);
        Bind(arena.bytes >= kArenaBytes ? arena.data : nullptr, kSlots);
    }

    /** Paged form with application-placed motion storage. */
    ParamLock(IButton& trigger, Pager& pgr, LockArena arena)
        : trigger_(&trigger), pager_(&pgr)
    {
        static_assert(!kInlineArena, ALCHEMY_LOCK_UNDERSIZE_MESSAGE);
        Bind(arena.bytes >= kArenaBytes ? arena.data : nullptr, pgr.NumPots());
    }

    /** Clear the motion arena and every slot.  Optional for an inline
     *  arena, REQUIRED for an external one in a NOLOAD section such as
     *  .sdram_bss.  Call from main() after hw.Init() — never from a
     *  static constructor. */
    void Init()
    {
        mgr_.Clear();
        mgr_.ClearArena();
    }

    /** False when an external arena was too small — the surface is inert. */
    bool IsReady() const { return mgr_.IsReady(); }

    /* ── Per-frame ───────────────────────────────────────────────────── */

    /**
     * Process one frame of GESTURE state: consume the trigger edges
     * PollButtons detected and run the record/disarm gesture if held.
     *
     * Playback is NOT advanced here — that's Advance()'s job, split out
     * so recorded modulation keeps running in modes (e.g. the Settings
     * menu) that suspend gesture handling.  ControlLoop calls both;
     * standalone users must now call Advance() every frame themselves
     * in addition to Update().
     *
     * **Call order (paged form).**  When constructed with a `Pager&`,
     * this method calls `pager.ConsumeButton()` on the trigger's
     * falling edge if a hold-and-nudge gesture armed a lock — so the
     * B1 release that ended the hold does not also advance the page.
     * For that suppression to land in the right frame, `Update()` MUST
     * be called **before** `Pager::Update()`:
     *
     * ```cpp
     * locks.Update(phys, now);   // ← runs first; may set pager.ConsumeButton()
     * pager.Update(phys, now);   // ← reads the consume flag this frame
     * ```
     *
     * Reverse the order and the pager has already processed the
     * release this frame; the consume flag latches to the next,
     * unrelated B1 release.  See `Pager::Update` for the full contract.
     */
    void PollButtons(uint32_t t_ms, bool gated) override;
    void Update     (const float* phys, uint32_t /*t_ms*/) override;

    /** Advance every active slot's play head by one frame.  Touches no
     *  gesture state — safe (and required) every frame, even while a
     *  mode like Settings suspends Update(). */
    void Advance() override;

    /** Declare the control-frame interval, so kMaxSeconds means seconds
     *  rather than frames.  ControlLoop calls this every frame. */
    void SetFrameMs(uint32_t frame_ms) override { mgr_.SetFrameMs(frame_ms); }

    /* ── Reads ───────────────────────────────────────────────────────── */

    /** Modulation delta for pot @p p on the visible page (no advance). */
    float Delta(uint8_t pot) const;

    /** True if a slot is actively playing back (current page, or unpaged). */
    bool IsActive(uint8_t pot) const;

    /** True if a slot is currently recording. */
    bool IsRecording(uint8_t pot) const;

    /** Read the modulation delta for an arbitrary (page, pot) slot, ignoring
     *  the pager's current page.  For composing inactive-page parameters. */
    float DeltaAtPage(uint8_t page, uint8_t pot) const override;

    /** True if the (page, pot) slot is actively playing back. */
    bool IsActiveAtPage(uint8_t page, uint8_t pot) const override;

    /** True if the (page, pot) slot is currently recording. */
    bool IsRecordingAtPage(uint8_t page, uint8_t pot) const override;

    /** True if any slot on the visible page is recording. */
    bool AnyRecording() const;

    /** True if any slot on the visible page is active. */
    bool AnyActive() const;

    /**
     * Optional default overlay: marks rings whose lock is active/recording with
     * a colored pip at 6 o'clock.  Recording = red, active = green.
     */
    void Render(LedPanel& panel, uint32_t t_ms) const override;

    /* ── Raw byte form ───────────────────────────────────────────────── */

    /** Serialise every slot into @p out[0..kSavedBytes-1].  Always the
     *  full byte form, independent of the LockStore policy. */
    void Save(uint8_t* out) const;

    /** Restore every slot from @p in[0..kSavedBytes-1]. */
    void Restore(const uint8_t* in);

    /** Reset every slot to its default (inactive) state. */
    void Clear() { mgr_.Clear(); }

    /** True while the trigger button is held (between rising and falling). */
    bool IsButtonHeld() const { return mgr_.IsButtonHeld(); }

    /* ── Serializable ──────────────────────────────────────────────────
     * Full byte form under LockStore::Preset, nothing under RamOnly.
     * Written straight through — no per-entry stack staging, which at
     * these buffer sizes would spike the control-loop stack.          */

    size_t SerializedSize() const override { return kPresetBytes; }

    void Serialize(uint8_t* out) const override
    {
        if constexpr (kStorage == LockStore::Preset) Save(out);
        else                                         (void)out;
    }

    bool Deserialize(const uint8_t* in) override
    {
        if constexpr (kStorage == LockStore::Preset) Restore(in);
        else                                         (void)in;
        return true;
    }

    /** Layout hash.  Folds in everything that changes the byte image or
     *  its meaning — slot count, length, rate, storage policy — so a
     *  reconfigured build sees old slots as empty, not as garbage. */
    uint32_t SchemaHash() const override
    {
        return kParamLockSchemaTag
             ^ (static_cast<uint32_t>(kSlots) << 8)
             ^ (static_cast<uint32_t>(kFramesPerSlot) * 0x9E3779B1u)
             ^ (static_cast<uint32_t>(kSampleRateHz) * 0x85EBCA6Bu)
             ^ (static_cast<uint32_t>(kStorage) * 0xC2B2AE35u)
             ^ static_cast<uint32_t>(kBytesPerSavedSlot);
    }

  private:
    /** Shared constructor tail — binds the manager to slots + arena. */
    void Bind(uint16_t* arena, uint8_t gesture_width)
    {
        LockConfig cfg;
        cfg.slots         = slots_;
        cfg.arena         = arena;
        cfg.stride        = kFramesPerSlot;
        cfg.total_slots   = kSlots;
        cfg.gesture_width = gesture_width;
        cfg.rate_hz       = kSampleRateHz;
        cfg.frame_ms      = kDefaultLockFrameMs;
        mgr_.Init(cfg);
    }

    /** Assumed control-frame interval until SetFrameMs() says otherwise.
     *  Matches ControlLoop::kDefaultFrameMs. */
    static constexpr uint32_t kDefaultLockFrameMs = 16u;

    IButton*         trigger_;
    Pager*           pager_;
    bool             prev_pressed_    = false;
    bool             rising_pending_  = false;
    bool             falling_pending_ = false;
    ParamLockSlot    slots_[kSlots] = {};
    ParamLockManager mgr_;

    detail::LockArenaStore<kArenaSamples, kInlineArena> arena_;

    uint8_t Offset() const { return pager_ ? pager_->Page() * pager_->NumPots() : 0u; }
    uint8_t Width()  const { return pager_ ? pager_->NumPots() : kSlots; }
};

template<uint8_t kSlots, class Len>
constexpr size_t ParamLock<kSlots, Len>::RamBytes()
{
    return sizeof(ParamLock<kSlots, Len>) + (kInlineArena ? size_t{0} : kArenaBytes);
}

} // namespace alchemy

#include "alchemy/surface/param_lock_impl.h"
