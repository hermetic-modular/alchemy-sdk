/**
 * @file alchemy/storage/sd_card.h
 * @brief alchemy::SdCard — the module's one SD volume, owned properly.
 *
 * Everything volume-level lives here so the module's own storage
 * features (an SD recorder, a sample player) and the HostLink filesystem
 * block share one mount, one busy signal, and one notion of "this file
 * is in use" instead of each carrying private copies:
 *
 *   - **Mount ownership.**  SdCard registers its own FATFS on volume
 *     "0:".  On Daisy bootloader (BOOT_SRAM) builds the board class's
 *     lazily-mounted FATFS lives in DTCM, which SDMMC1's internal DMA
 *     cannot address — every FatFS work buffer must sit in DMA-reachable
 *     memory.  SdCard's FATFS lives in SDRAM (32-byte aligned for the
 *     cache-maintenance ranges in sd_diskio) and is memset explicitly in
 *     Init() because .sdram_bss is NOLOAD.  Consumers own their FIL/DIR
 *     objects but must place them under the same rules — see
 *     ALCHEMY_SDRAM_BSS below.
 *
 *   - **Busy signal.**  A volatile refcount raised around every
 *     filesystem operation (BusyGuard).  Audio-side code reads Busy()
 *     to ride through SD stalls — e.g. relaxing a musical-clock
 *     snapshot-age clamp so card housekeeping pauses extrapolate phase
 *     instead of freezing it.
 *
 *   - **File locks.**  A firmware feature holding a file open registers
 *     its protocol-form path ("/REC0001.WAV"); the HostLink filesystem
 *     block refuses writes/delete/rename on locked paths (FS_LOCKED)
 *     and flags them in listings.  Case-insensitive, like FAT itself.
 *
 *   - **Card-state tracking.**  EnsureMounted() performs the deferred
 *     mount with failed-attempt rate limiting; OnIoError() flags the
 *     volume for a forced remount so a swapped card is picked up.
 *
 * One instance per system (the volume is a hardware singleton, like the
 * CDC transport).  All methods are control-loop-thread only except
 * Busy(), which any context may read.
 */

#pragma once

#include <cstdint>

#include "ff.h"

/** Placement attribute for FatFS-touched statics (FATFS/FIL/DIR/staging
 *  buffers): SDRAM on target — SDMMC1's IDMA cannot address DTCM, where
 *  bootloader-build .bss lives — and plain memory in host test builds.
 *  Pair with alignas(32) and an explicit memset before first use. */
#if defined(__arm__)
#define ALCHEMY_SDRAM_BSS __attribute__((section(".sdram_bss")))
#else
#define ALCHEMY_SDRAM_BSS
#endif

/* ── The three SD-DMA placement rules ─────────────────────────────────
 *
 * Every buffer that FatFS may hand to disk I/O (the FATFS work area,
 * FILs/DIRs, and any staging you f_read into / f_write from) must obey
 * ALL THREE, or data corrupts silently on hardware while passing every
 * RAM-backed test:
 *
 *   1. Never DTCM — SDMMC1's IDMA cannot address it (bootloader builds
 *      link .bss there).
 *   2. AXI SRAM — the IDMA's proven home.  Use ALCHEMY_SDMMC_BSS below;
 *      the firmware's linker script must provide `.axi_bss` in the AXI
 *      region (see the eocfx/Spagyros preset_flash_region.ld pattern).
 *      NOLOAD semantics: memset before first use, alignas(32).
 *   3. Word-aligned direct transfers — FatFS moves whole sectors
 *      directly between the disk and `your_buffer + head_len`, where
 *      head_len depends on the FILE OFFSET; the IDMA silently truncates
 *      unaligned addresses, shifting data 1–3 bytes (the REC0001.WAV
 *      download corruption).  Offset your staging pointer by
 *      SdDmaStagingPad(file_offset) — and reserve 3 spare bytes — so
 *      the direct-sector portion always lands word-aligned.
 */
#if defined(__arm__)
#define ALCHEMY_SDMMC_BSS __attribute__((section(".axi_bss")))
#else
#define ALCHEMY_SDMMC_BSS
#endif

namespace alchemy {

/**
 * Staging-pointer pad (0–3 bytes) that word-aligns FatFS's direct-
 * sector DMA for a transfer starting at @p file_offset (rule 3 above):
 * read into / write from `staging + SdDmaStagingPad(offset)` and the
 * whole-sector portion of the transfer is guaranteed word-aligned.
 * The host test suite's RAM disk asserts no unaligned pointer ever
 * reaches disk I/O, so skipping this is caught in CI — at the
 * production frame size only; test storage code at 2048-byte bodies.
 */
constexpr uint32_t SdDmaStagingPad(uint32_t file_offset)
{
    const uint32_t head = (512u - (file_offset % 512u)) % 512u;
    return (4u - (head & 3u)) & 3u;
}

} // namespace alchemy

namespace alchemy {

class SdCard
{
  public:
    enum class State : uint8_t
    {
        Unmounted,   ///< not yet mounted (or flagged for remount)
        Mounted,     ///< volume usable
        NoCard,      ///< last mount attempt failed (absent / unreadable)
    };

    static constexpr uint8_t  kMaxLocks        = 4u;
    static constexpr uint16_t kMaxLockPath     = 192u;
    static constexpr uint32_t kRemountBackoffMs = 2000u;

    /** Register the SDRAM FATFS on volume "0:" (lazy — no disk I/O) and
     *  zero the arena.  Call once at boot, before audio starts and
     *  before any consumer touches the volume. */
    void Init();

    /**
     * Make the volume usable, performing the deferred/forced mount if
     * needed.  Failed attempts are rate-limited (kRemountBackoffMs) so
     * pollers don't hammer a slow empty slot; @p now_ms is any
     * monotonic millisecond clock.  Returns true when mounted.
     */
    bool EnsureMounted(uint32_t now_ms);

    State CardState() const { return state_; }

    /** FatFS result of the most recent mount attempt — the diagnostic
     *  FS_INFO forwards to the host when the volume won't come up
     *  (0xFE = no attempt yet). */
    uint8_t LastMountResult() const { return last_mount_fr_; }

    /* ── Raw-media diagnostics (FS_INFO no-card trailer, §8.3) ──────────
     * When the mount fails, the host wants EVIDENCE, not a code: what do
     * the first sectors actually contain?  Distinguishes "card is exFAT"
     * from "reads return garbage" from "valid FAT that FatFS rejected"
     * without swapping cards blind. */

    /** One probed sector's identification bytes. */
    struct SectorProbe
    {
        uint8_t read_result;   ///< diskio DRESULT; 0xFF = not attempted
        uint8_t sig[2];        ///< bytes 510..511 (expect 0x55 0xAA)
        uint8_t first;         ///< byte 0 (0xEB/0xE9 = boot jump → VBR)
        uint8_t oem[8];        ///< bytes 3..10  ("EXFAT   ", "MSDOS5.0", …)
        uint8_t fstype[8];     ///< bytes 82..89 ("FAT32   " on a FAT32 VBR)
    };

    struct MediaDiagnostics
    {
        uint8_t     mount_fr;     ///< FatFS FRESULT of the failed mount
        uint8_t     disk_init;    ///< diskio DSTATUS (0 = ok)
        SectorProbe sector0;
        uint8_t     pt_types[4];  ///< MBR partition type IDs (sector 0)
        uint32_t    pt1_lba;      ///< partition 1 start LBA
        SectorProbe part1;        ///< partition 1's boot sector
    };

    /** Run the raw probes (bounded: ≤ 2 sector reads).  Main thread. */
    void Diagnose(MediaDiagnostics& out);

    /** Report a filesystem I/O failure (FR_DISK_ERR / FR_NOT_READY):
     *  flags the volume for a forced remount on next EnsureMounted, so
     *  card swaps and transient faults recover without a reboot. */
    void OnIoError();

    /** FatFS type of the mounted volume (FS_FAT16 / FS_FAT32), 0 when
     *  not mounted. */
    uint8_t FsType() const;

    /**
     * Volume capacity, KiB.  Free space comes from the volume's FSINFO
     * as cached by FatFS — when the card carries no valid FSINFO this
     * returns false for `free_valid` rather than stalling on a full FAT
     * scan (protocol §8 FS_INFO contract).  Total is always valid while
     * mounted.
     */
    bool VolumeKib(uint32_t& total_kib, uint32_t& free_kib,
                   bool& free_valid);

    /** The FATFS this volume runs on (for f_* calls that need it). */
    FATFS& Fs();

    /* ── Busy signal ────────────────────────────────────────────────── */

    /** True while any consumer holds a BusyGuard.  Safe from any
     *  context (single volatile byte). */
    bool Busy() const { return busy_ != 0u; }

    /** RAII busy-refcount; hold across every f_* call sequence. */
    class BusyGuard
    {
      public:
        explicit BusyGuard(SdCard& card) : card_(card) { card_.busy_ = card_.busy_ + 1u; }
        ~BusyGuard() { card_.busy_ = card_.busy_ - 1u; }
        BusyGuard(const BusyGuard&)            = delete;
        BusyGuard& operator=(const BusyGuard&) = delete;

      private:
        SdCard& card_;
    };

    /* ── Firmware file locks ────────────────────────────────────────── */

    /** Register @p path (protocol form, "/DIR/FILE.EXT") as held by the
     *  firmware.  False when the table is full or the path is overlong.
     *  Re-locking an already-locked path succeeds (idempotent). */
    bool Lock(const char* path);

    /** Release a held path (no-op when absent). */
    void Unlock(const char* path);

    /** True when @p path is currently firmware-held.  Case-insensitive
     *  (FAT semantics). */
    bool IsLocked(const char* path) const;

    /** True when any path is firmware-held (FS_INFO's fw-busy flag). */
    bool AnyLocked() const;

  private:
    friend class BusyGuard;

    State    state_           = State::Unmounted;
    uint32_t last_attempt_ms_ = 0u;
    bool     attempted_       = false;
    uint8_t  last_mount_fr_   = 0xFEu;

    volatile uint8_t busy_ = 0u;

    char lock_[kMaxLocks][kMaxLockPath + 1u] = {};
};

} // namespace alchemy
