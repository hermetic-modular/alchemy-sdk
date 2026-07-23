/**
 * @file storage/sd_card.cpp
 * @brief SdCard implementation.  See the header for the ownership story.
 */

#include "alchemy/storage/sd_card.h"

#include <cstring>

#include "diskio.h"

namespace alchemy {

namespace {

/* The volume's FATFS — the one FatFS work area every consumer shares.
 * SDRAM + 32-byte alignment per the DMA/cache rules in the header;
 * .sdram_bss is NOLOAD, so Init() memsets it before use. */
alignas(32) ALCHEMY_SDMMC_BSS FATFS s_fs;

/* Dedicated, perfectly aligned sector buffer for the raw diagnostics —
 * deliberately NOT fs.win, so the probe also exercises the DMA path
 * with ideal alignment (a corrupt read here is a data-path fault, not
 * an alignment artifact). */
alignas(32) ALCHEMY_SDMMC_BSS uint8_t s_probe_sector[512];

/** ASCII-case-insensitive path equality (FAT name semantics). */
bool PathEq(const char* a, const char* b)
{
    while (*a && *b)
    {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
    }
    return *a == *b;
}

} // namespace

void SdCard::Init()
{
    std::memset(&s_fs, 0, sizeof(s_fs));
    std::memset(lock_, 0, sizeof(lock_));
    state_           = State::Unmounted;
    attempted_       = false;
    last_attempt_ms_ = 0u;
    busy_            = 0u;

    /* Lazy registration: claims volume "0:" for OUR work area (the board
     * class may have registered its DTCM-resident FATFS first — this
     * re-registration supersedes it).  No disk I/O happens here. */
    f_mount(&s_fs, "0:", 0);
}

bool SdCard::EnsureMounted(uint32_t now_ms)
{
    if (state_ == State::Mounted) return true;

    /* Rate-limit failed attempts: an empty slot can take hundreds of ms
     * to fail, and pollers (FS_INFO) would otherwise pay that every
     * request. */
    if (attempted_
        && static_cast<uint32_t>(now_ms - last_attempt_ms_) < kRemountBackoffMs)
        return false;

    attempted_       = true;
    last_attempt_ms_ = now_ms;

    BusyGuard busy(*this);
    const FRESULT fr = f_mount(&s_fs, "0:", 1);   /* forced: mounts NOW */
    last_mount_fr_   = static_cast<uint8_t>(fr);
    state_ = (fr == FR_OK) ? State::Mounted : State::NoCard;
    return state_ == State::Mounted;
}

void SdCard::OnIoError()
{
    /* Force a fresh disk_initialize on the next EnsureMounted — the
     * path a swapped or glitched card recovers through. */
    state_     = State::Unmounted;
    attempted_ = false;
}

uint8_t SdCard::FsType() const
{
    return (state_ == State::Mounted) ? s_fs.fs_type : 0u;
}

bool SdCard::VolumeKib(uint32_t& total_kib, uint32_t& free_kib,
                       bool& free_valid)
{
    total_kib  = 0u;
    free_kib   = 0u;
    free_valid = false;
    if (state_ != State::Mounted) return false;

    /* Sector math in KiB units (sectors are 512 B → 2 sectors/KiB).
     * n_fatent - 2 = data clusters; csize = sectors per cluster. */
    const uint32_t clusters = static_cast<uint32_t>(s_fs.n_fatent - 2u);
    total_kib = clusters * s_fs.csize / 2u;

    /* free_clst is FatFS's cached free count — seeded from FSINFO on
     * FAT32 or maintained incrementally once known.  0xFFFFFFFF means
     * "unknown"; per the FS_INFO contract we report invalid instead of
     * paying a full FAT scan on the control-loop thread. */
    if (s_fs.free_clst != 0xFFFFFFFFu && s_fs.free_clst <= clusters)
    {
        free_kib   = s_fs.free_clst * s_fs.csize / 2u;
        free_valid = true;
    }
    return true;
}

FATFS& SdCard::Fs() { return s_fs; }

namespace {

void ProbeSector(uint32_t lba, SdCard::SectorProbe& p)
{
    std::memset(&p, 0, sizeof(p));
    std::memset(s_probe_sector, 0, sizeof(s_probe_sector));
    p.read_result = static_cast<uint8_t>(disk_read(0u, s_probe_sector, lba, 1u));
    if (p.read_result != 0u) return;
    p.sig[0] = s_probe_sector[510];
    p.sig[1] = s_probe_sector[511];
    p.first  = s_probe_sector[0];
    std::memcpy(p.oem,    &s_probe_sector[3],  8u);
    std::memcpy(p.fstype, &s_probe_sector[82], 8u);
}

} // namespace

void SdCard::Diagnose(MediaDiagnostics& out)
{
    std::memset(&out, 0, sizeof(out));
    out.mount_fr           = last_mount_fr_;
    out.sector0.read_result = 0xFFu;
    out.part1.read_result   = 0xFFu;

    BusyGuard busy(*this);
    out.disk_init = static_cast<uint8_t>(disk_initialize(0u));
    if (out.disk_init != 0u) return;

    ProbeSector(0u, out.sector0);
    if (out.sector0.read_result != 0u) return;

    /* MBR partition table (only meaningful when sector 0 is not itself
     * a boot-jump VBR — the host interprets). */
    for (uint8_t i = 0u; i < 4u; i++)
        out.pt_types[i] = s_probe_sector[0x1BEu + i * 16u + 4u];
    out.pt1_lba = static_cast<uint32_t>(s_probe_sector[0x1BEu + 8u])
                | (static_cast<uint32_t>(s_probe_sector[0x1BEu + 9u]) << 8)
                | (static_cast<uint32_t>(s_probe_sector[0x1BEu + 10u]) << 16)
                | (static_cast<uint32_t>(s_probe_sector[0x1BEu + 11u]) << 24);

    const bool s0_is_vbr = out.sector0.first == 0xEBu || out.sector0.first == 0xE9u;
    if (!s0_is_vbr && out.pt_types[0] != 0u && out.pt1_lba != 0u)
        ProbeSector(out.pt1_lba, out.part1);
}

bool SdCard::Lock(const char* path)
{
    if (path == nullptr || path[0] == '\0') return false;
    if (std::strlen(path) > kMaxLockPath) return false;
    if (IsLocked(path)) return true;

    for (uint8_t i = 0u; i < kMaxLocks; i++)
    {
        if (lock_[i][0] == '\0')
        {
            std::strcpy(lock_[i], path);
            return true;
        }
    }
    return false;
}

void SdCard::Unlock(const char* path)
{
    if (path == nullptr) return;
    for (uint8_t i = 0u; i < kMaxLocks; i++)
        if (lock_[i][0] != '\0' && PathEq(lock_[i], path))
            lock_[i][0] = '\0';
}

bool SdCard::IsLocked(const char* path) const
{
    if (path == nullptr) return false;
    for (uint8_t i = 0u; i < kMaxLocks; i++)
        if (lock_[i][0] != '\0' && PathEq(lock_[i], path))
            return true;
    return false;
}

bool SdCard::AnyLocked() const
{
    for (uint8_t i = 0u; i < kMaxLocks; i++)
        if (lock_[i][0] != '\0') return true;
    return false;
}

} // namespace alchemy
