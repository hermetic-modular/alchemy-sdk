/**
 * @file storage/fs_extension.cpp
 * @brief Protocol §8 filesystem block implementation.
 *
 * Layout of this file: wire/body helpers → path helpers → FatFS error
 * mapping → the dispatcher → one handler per command → lifecycle.
 *
 * Every handler follows the same shape: parse + validate the body,
 * ensure the volume is mounted, do at most one bounded disk operation
 * through the SDRAM staging set, and write the response body (status
 * first).  All disk work runs under an SdCard::BusyGuard so audio-side
 * consumers can ride through card stalls.
 */

#include "alchemy/storage/fs_extension.h"

#include <cstring>

#include "alchemy/host_link/crc32.h"

namespace alchemy {
namespace hostlink {

namespace {

/* ── SDRAM working set ──────────────────────────────────────────────────
 * Frame buffers live with the engine (DTCM on bootloader builds), which
 * SDMMC1's IDMA cannot address — every byte between FatFS and the wire
 * stages through here.  One instance per system, like SdCard; NOLOAD
 * section, zeroed lazily below (no boot-time hook is guaranteed to run
 * before the first command, so first use zeroes).                       */
alignas(32) ALCHEMY_SDMMC_BSS FIL     s_fil;       /* the open transfer  */
alignas(32) ALCHEMY_SDMMC_BSS FIL     s_meta_fil;  /* .partmeta writes   */
alignas(32) ALCHEMY_SDMMC_BSS DIR     s_dir;       /* the listing cursor */
/* +32: room for the SdDmaStagingPad alignment offset (placement rule 3
 * in sd_card.h — FatFS's direct-sector DMA must land word-aligned for
 * arbitrary file offsets). */
alignas(32) ALCHEMY_SDMMC_BSS uint8_t s_staging[kMaxBody + 32u];

bool s_sdram_zeroed = false;

void EnsureZeroed()
{
    if (s_sdram_zeroed) return;
    std::memset(&s_fil,      0, sizeof(s_fil));
    std::memset(&s_meta_fil, 0, sizeof(s_meta_fil));
    std::memset(&s_dir,      0, sizeof(s_dir));
    std::memset(s_staging,   0, sizeof(s_staging));
    s_sdram_zeroed = true;
}

/* CPU-only path state (plain memory is fine — never handed to FatFS as
 * an I/O buffer, only as path strings). */
constexpr size_t kPartSuffixLen = 5u;   /* ".part"     */
constexpr size_t kMetaSuffixLen = 9u;   /* ".partmeta" */

char s_xfer_path [kFsMaxPath + 1u]                  = {};  /* protocol form */
char s_part_path [kFsMaxPath + kPartSuffixLen + 1u] = {};
char s_meta_path [kFsMaxPath + kMetaSuffixLen + 1u] = {};
char s_dir_path  [kFsMaxPath + 1u]                  = {};

/* .partmeta image (§8.4). */
constexpr uint32_t kMetaMagic = 0x4D504D48u;   /* "HMPM" little-endian */
constexpr size_t   kMetaBytes = 16u;

/* ── Body reader ──────────────────────────────────────────────────────── */

struct BodyReader
{
    const uint8_t* p;
    uint16_t       len;
    uint16_t       off = 0u;
    bool           ok  = true;

    BodyReader(const ParsedFrame& f) : p(f.body), len(f.len) {}

    uint8_t U8()
    {
        if (off + 1u > len) { ok = false; return 0u; }
        return p[off++];
    }
    uint16_t U16()
    {
        if (off + 2u > len) { ok = false; return 0u; }
        const uint16_t v = RdU16(p + off); off += 2u; return v;
    }
    uint32_t U32()
    {
        if (off + 4u > len) { ok = false; return 0u; }
        const uint32_t v = RdU32(p + off); off += 4u; return v;
    }
    /** Length-prefixed string → NUL-terminated @p dst. */
    bool Str(char* dst, size_t cap)
    {
        const uint8_t n = U8();
        if (!ok || off + n > len || n >= cap) { ok = false; return false; }
        std::memcpy(dst, p + off, n);
        dst[n] = '\0';
        off += n;
        return true;
    }
    /** Remaining bytes (payload tail). */
    const uint8_t* Rest(uint16_t& n)
    {
        n = static_cast<uint16_t>(len - off);
        return p + off;
    }
};

/* ── Path helpers (§8.1) ──────────────────────────────────────────────── */

bool CharOk(char c)
{
    if (c < 0x20 || c > 0x7E) return false;
    switch (c)
    {
        case '\\': case ':': case '*': case '?':
        case '"': case '<': case '>': case '|': return false;
        default: return true;
    }
}

bool SegmentOk(const char* s, size_t n)
{
    if (n == 0u || n > kFsMaxSegment) return false;
    if (n == 1u && s[0] == '.') return false;
    if (n == 2u && s[0] == '.' && s[1] == '.') return false;
    if (s[n - 1u] == '.' || s[n - 1u] == ' ') return false;
    for (size_t i = 0u; i < n; i++)
        if (!CharOk(s[i]) || s[i] == '/') return false;
    return true;
}

bool ValidatePath(const char* path, bool allow_root)
{
    if (path == nullptr || path[0] != '/') return false;
    const size_t len = std::strlen(path);
    if (len > kFsMaxPath) return false;
    if (len == 1u) return allow_root;          /* "/" */
    if (path[len - 1u] == '/') return false;   /* no trailing slash */

    const char* seg = path + 1u;
    while (true)
    {
        const char* slash = std::strchr(seg, '/');
        const size_t n = slash ? static_cast<size_t>(slash - seg)
                               : std::strlen(seg);
        if (!SegmentOk(seg, n)) return false;
        if (!slash) return true;
        seg = slash + 1u;
    }
}

bool HasSuffixCI(const char* s, const char* suffix)
{
    const size_t ls = std::strlen(s), lx = std::strlen(suffix);
    if (ls < lx) return false;
    s += ls - lx;
    while (*suffix)
    {
        char a = *s++, b = *suffix++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
    }
    return true;
}

bool PathEqCI(const char* a, const char* b)
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

/** Protocol path → FatFS path ("0:" + path).  @p out cap must be ≥
 *  kFsMaxPath + kMetaSuffixLen + 3. */
void FatPath(char* out, const char* proto_path)
{
    out[0] = '0'; out[1] = ':';
    std::strcpy(out + 2, proto_path);
}

/** dir + "/" + name → @p out (protocol form).  False on overflow. */
bool JoinChild(char* out, size_t cap, const char* dir, const char* name)
{
    const size_t ld = std::strlen(dir), ln = std::strlen(name);
    const bool root = (ld == 1u);   /* dir == "/" */
    const size_t total = (root ? 1u : ld + 1u) + ln;
    if (total + 1u > cap) return false;
    std::strcpy(out, dir);
    if (!root) out[ld] = '/';
    std::strcpy(out + (root ? 1u : ld + 1u), name);
    return true;
}

uint32_t FatMtime(const FILINFO& fno)
{
    return (static_cast<uint32_t>(fno.fdate) << 16) | fno.ftime;
}

/* ── FatFS error mapping ──────────────────────────────────────────────── */

Status MapFr(SdCard& card, FRESULT fr)
{
    switch (fr)
    {
        case FR_OK:                return Status::Ok;
        case FR_NO_FILE:
        case FR_NO_PATH:           return Status::FsNoFile;
        case FR_EXIST:             return Status::FsExists;
        case FR_DENIED:            return Status::BadState;
        case FR_INVALID_NAME:
        case FR_INVALID_PARAMETER: return Status::BadArgs;
        case FR_LOCKED:            return Status::FsLocked;
        case FR_NOT_READY:
        case FR_DISK_ERR:
        /* FatFS's validate() answers INVALID_OBJECT when disk_status
         * reports the medium gone — on a soldered-down SDMMC slot that
         * means the card was pulled, not a stale handle.  Treat it as
         * an I/O loss so the remount path runs. */
        case FR_INVALID_OBJECT:
            card.OnIoError();
            return Status::FsIo;
        default:                   return Status::FsIo;
    }
}

} // namespace

/* ── Dispatcher ─────────────────────────────────────────────────────────── */

void FsExtension::Handle(const ParsedFrame& f, FrameWriter& w, uint32_t now_ms)
{
    EnsureZeroed();
    SdCard::BusyGuard busy(card_);

    switch (static_cast<Cmd>(f.type))
    {
        case Cmd::FsInfo:      OnInfo(f, w, now_ms);      return;
        case Cmd::FsList:      OnList(f, w, now_ms);      return;
        case Cmd::FsStat:      OnStat(f, w, now_ms);      return;
        case Cmd::FsOpenRead:  OnOpenRead(f, w, now_ms);  return;
        case Cmd::FsRead:      OnRead(f, w, now_ms);      return;
        case Cmd::FsClose:     OnClose(f, w);             return;
        case Cmd::FsOpenWrite: OnOpenWrite(f, w, now_ms); return;
        case Cmd::FsWrite:     OnWrite(f, w, now_ms);     return;
        case Cmd::FsCommit:    OnCommit(f, w);            return;
        case Cmd::FsDelete:    OnDelete(f, w, now_ms);    return;
        case Cmd::FsMkdir:     OnMkdir(f, w, now_ms);     return;
        case Cmd::FsRename:    OnRename(f, w, now_ms);    return;
        default:
            w.U8(static_cast<uint8_t>(Status::Unsupported));
            return;
    }
}

void FsExtension::Tick(uint32_t now_ms)
{
    if (mode_ != XferMode::None
        && static_cast<uint32_t>(now_ms - xfer_last_ms_) > kXferTimeoutMs)
    {
        SdCard::BusyGuard busy(card_);
        CloseTransfer(true /* checkpoint for resume */);
    }
    if (dir_open_
        && static_cast<uint32_t>(now_ms - dir_last_ms_) > kXferTimeoutMs)
    {
        SdCard::BusyGuard busy(card_);
        CloseDirCursor();
    }
}

/* ── FS_INFO ────────────────────────────────────────────────────────────── */

void FsExtension::OnInfo(const ParsedFrame& f, FrameWriter& w,
                         uint32_t now_ms)
{
    (void)f;
    if (!card_.EnsureMounted(now_ms))
    {
        /* Diagnostic trailer (§8.3): the failed mount's FRESULT plus
         * raw first-sector evidence, so the host can render a verdict
         * ("card is exFAT", "reads return garbage", …) instead of a
         * blind "no card". */
        SdCard::MediaDiagnostics d;
        card_.Diagnose(d);

        auto probe = [&](const SdCard::SectorProbe& p)
        {
            w.U8(p.read_result);
            w.Bytes(p.sig, 2u);
            w.U8(p.first);
            w.Bytes(p.oem, 8u);
            w.Bytes(p.fstype, 8u);
        };

        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        w.U8(d.mount_fr);
        w.U8(d.disk_init);
        probe(d.sector0);
        w.Bytes(d.pt_types, 4u);
        w.U32(d.pt1_lba);
        probe(d.part1);
        return;
    }

    uint32_t total = 0u, free_kib = 0u;
    bool     free_valid = false;
    card_.VolumeKib(total, free_kib, free_valid);

    uint8_t flags = 0u;
    if (free_valid)          flags |= kFsInfoFreeValid;
    if (card_.AnyLocked())   flags |= kFsInfoFwBusy;

    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U8(1u);                       /* card_state: mounted */
    w.U8(card_.FsType());
    w.U8(flags);
    w.U32(total);
    w.U32(free_kib);
    w.U16(static_cast<uint16_t>(kFsMaxSegment));
}

/* ── FS_LIST ────────────────────────────────────────────────────────────── */

void FsExtension::OnList(const ParsedFrame& f, FrameWriter& w,
                         uint32_t now_ms)
{
    BodyReader r(f);
    const uint32_t cookie = r.U32();
    char path[kFsMaxPath + 1u];
    if (!r.Str(path, sizeof(path)) || !ValidatePath(path, true))
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (!card_.EnsureMounted(now_ms))
    {
        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        return;
    }

    /* Cursor (re)establishment.  A fresh listing (cookie 0), a path
     * change, or a cookie that doesn't match the live cursor all mean
     * "reopen and skip" — stale cookies stay serviceable (§8), merely
     * paying the skip. */
    const bool cursor_matches = dir_open_
                                && cookie != 0u
                                && cookie == dir_emitted_
                                && PathEqCI(path, s_dir_path);
    if (!cursor_matches)
    {
        CloseDirCursor();

        char fat[kFsMaxPath + 4u];
        FatPath(fat, path);
        const FRESULT fr = f_opendir(&s_dir, fat);
        if (fr != FR_OK)
        {
            w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
            return;
        }
        dir_open_  = true;
        std::strcpy(s_dir_path, path);
        dir_index_   = 0u;
        dir_emitted_ = 0u;
        pending_valid_ = false;

        /* Skip entries the host has already consumed (stale cookie).
         * dir_index_ counts consumed-from-DIR; emitted == consumed here
         * because the lookahead is empty after a reopen. */
        while (dir_emitted_ < cookie && cookie != kFsListEnd)
        {
            FILINFO fno;
            if (f_readdir(&s_dir, &fno) != FR_OK || fno.fname[0] == '\0')
                break;                       /* dir shrank; emit from here */
            const size_t n = std::strlen(fno.fname);
            if (n == 0u || n > kFsMaxSegment || !SegmentOk(fno.fname, n))
                continue;                    /* skipped in original pass too */
            dir_index_++;
            dir_emitted_++;
        }
    }
    dir_last_ms_ = now_ms;

    /* Pack entries into the SDRAM staging area first (count precedes the
     * entries on the wire, and FrameWriter cannot back-patch). */
    const size_t budget = static_cast<size_t>(kMaxBody) - 6u; /* st+cookie+count */
    size_t  used  = 0u;
    uint8_t count = 0u;
    bool    end   = false;

    while (count < 255u)
    {
        /* One-entry lookahead: an entry that doesn't fit stays pending
         * for the next request instead of being lost (the DIR cursor
         * has already moved past it). */
        if (!pending_valid_)
        {
            FILINFO fno;
            const FRESULT fr = f_readdir(&s_dir, &fno);
            if (fr != FR_OK)
            {
                CloseDirCursor();
                w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
                return;
            }
            if (fno.fname[0] == '\0') { end = true; break; }

            const size_t n = std::strlen(fno.fname);
            /* Names outside the §8.1 charset (or overlong) cannot be
             * addressed through this protocol at all — skip rather than
             * advertise something stat/open would then reject. */
            if (n == 0u || n > kFsMaxSegment || !SegmentOk(fno.fname, n))
            {
                dir_index_++;
                continue;
            }

            pending_flags_ = 0u;
            if (fno.fattrib & AM_DIR) pending_flags_ |= kFsEntryDir;
            if (fno.fattrib & AM_RDO) pending_flags_ |= kFsEntryReadOnly;
            char child[kFsMaxPath + kFsMaxSegment + 2u];
            if (JoinChild(child, sizeof(child), s_dir_path, fno.fname)
                && card_.IsLocked(child))
                pending_flags_ |= kFsEntryLocked;
            pending_size_  = static_cast<uint32_t>(fno.fsize);
            pending_mtime_ = FatMtime(fno);
            std::strcpy(pending_name_, fno.fname);
            pending_valid_ = true;
            dir_index_++;
        }

        const size_t name_len   = std::strlen(pending_name_);
        const size_t entry_size = 1u + 4u + 4u + 1u + name_len;
        if (used + entry_size > budget) break;    /* pending carries over */

        s_staging[used++] = pending_flags_;
        WrU32(&s_staging[used], pending_size_);  used += 4u;
        WrU32(&s_staging[used], pending_mtime_); used += 4u;
        s_staging[used++] = static_cast<uint8_t>(name_len);
        std::memcpy(&s_staging[used], pending_name_, name_len);
        used += name_len;
        count++;
        dir_emitted_++;
        pending_valid_ = false;
    }

    uint32_t next_cookie;
    if (end && !pending_valid_)
    {
        CloseDirCursor();
        next_cookie = kFsListEnd;
    }
    else
    {
        next_cookie = dir_emitted_;
    }

    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U32(next_cookie);
    w.U8(count);
    w.Bytes(s_staging, used);
}

/* ── FS_STAT ────────────────────────────────────────────────────────────── */

void FsExtension::OnStat(const ParsedFrame& f, FrameWriter& w,
                         uint32_t now_ms)
{
    BodyReader r(f);
    char path[kFsMaxPath + 1u];
    if (!r.Str(path, sizeof(path)) || !ValidatePath(path, true))
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (!card_.EnsureMounted(now_ms))
    {
        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        return;
    }

    if (path[1] == '\0')   /* "/" — FatFS won't stat the root; synthesize */
    {
        w.U8(static_cast<uint8_t>(Status::Ok));
        w.U8(kFsEntryDir);
        w.U32(0u);
        w.U32(0u);
        return;
    }

    char fat[kFsMaxPath + 4u];
    FatPath(fat, path);
    FILINFO fno;
    const FRESULT fr = f_stat(fat, &fno);
    if (fr != FR_OK)
    {
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }

    uint8_t flags = 0u;
    if (fno.fattrib & AM_DIR)  flags |= kFsEntryDir;
    if (fno.fattrib & AM_RDO)  flags |= kFsEntryReadOnly;
    if (card_.IsLocked(path))  flags |= kFsEntryLocked;

    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U8(flags);
    w.U32(static_cast<uint32_t>(fno.fsize));
    w.U32(FatMtime(fno));
}

/* ── FS_OPEN_READ / FS_READ ─────────────────────────────────────────────── */

void FsExtension::OnOpenRead(const ParsedFrame& f, FrameWriter& w,
                             uint32_t now_ms)
{
    BodyReader r(f);
    char path[kFsMaxPath + 1u];
    if (!r.Str(path, sizeof(path)) || !ValidatePath(path, false))
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (!card_.EnsureMounted(now_ms))
    {
        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        return;
    }

    CloseTransfer(true);   /* opening aborts any prior transfer (§8.2) */

    char fat[kFsMaxPath + 4u];
    FatPath(fat, path);

    FILINFO fno;
    FRESULT fr = f_stat(fat, &fno);
    if (fr != FR_OK)
    {
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }
    if (fno.fattrib & AM_DIR)
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }

    fr = f_open(&s_fil, fat, FA_READ);
    if (fr != FR_OK)
    {
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }

    mode_         = XferMode::Read;
    xfer_size_    = static_cast<uint32_t>(f_size(&s_fil));
    xfer_pos_     = 0u;
    xfer_last_ms_ = now_ms;
    std::strcpy(s_xfer_path, path);

    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U32(xfer_size_);
    w.U32(FatMtime(fno));
}

void FsExtension::OnRead(const ParsedFrame& f, FrameWriter& w,
                         uint32_t now_ms)
{
    if (mode_ != XferMode::Read)
    {
        w.U8(static_cast<uint8_t>(Status::BadState));
        return;
    }

    BodyReader r(f);
    const uint32_t offset  = r.U32();
    uint16_t       max_len = r.U16();
    if (!r.ok)
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    xfer_last_ms_ = now_ms;

    /* Response header is 7 bytes (status + offset + n). */
    const uint16_t cap = static_cast<uint16_t>(kMaxBody - 7u);
    if (max_len > cap) max_len = cap;

    uint32_t n = 0u;
    if (offset < xfer_size_)
    {
        const uint32_t remain = xfer_size_ - offset;
        n = (max_len < remain) ? max_len : remain;
    }

    const uint32_t pad = SdDmaStagingPad(offset);
    if (n > 0u)
    {
        FRESULT fr = FR_OK;
        if (f_tell(&s_fil) != offset) fr = f_lseek(&s_fil, offset);
        UINT br = 0u;
        if (fr == FR_OK) fr = f_read(&s_fil, s_staging + pad, n, &br);
        if (fr != FR_OK || br != n)
        {
            const Status st = (fr == FR_OK) ? Status::FsIo : MapFr(card_, fr);
            CloseTransfer(false);
            w.U8(static_cast<uint8_t>(st));
            return;
        }
    }

    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U32(offset);
    w.U16(static_cast<uint16_t>(n));
    w.Bytes(s_staging + pad, n);
}

/* ── FS_CLOSE ───────────────────────────────────────────────────────────── */

void FsExtension::OnClose(const ParsedFrame& f, FrameWriter& w)
{
    (void)f;
    CloseTransfer(true);
    w.U8(static_cast<uint8_t>(Status::Ok));
}

/* ── FS_OPEN_WRITE / FS_WRITE / FS_COMMIT ───────────────────────────────── */

void FsExtension::OnOpenWrite(const ParsedFrame& f, FrameWriter& w,
                              uint32_t now_ms)
{
    BodyReader r(f);
    const uint8_t  flags = r.U8();
    const uint32_t total = r.U32();
    char path[kFsMaxPath + 1u];
    if (!r.Str(path, sizeof(path)) || !ValidatePath(path, false)
        || HasSuffixCI(path, ".part") || HasSuffixCI(path, ".partmeta"))
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (total > 0xFFF00000u)   /* FAT32 file-size ceiling with margin */
    {
        w.U8(static_cast<uint8_t>(Status::TooLarge));
        return;
    }
    if (!card_.EnsureMounted(now_ms))
    {
        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        return;
    }
    if (card_.IsLocked(path))
    {
        w.U8(static_cast<uint8_t>(Status::FsLocked));
        return;
    }

    CloseTransfer(true);

    char fat[kFsMaxPath + kMetaSuffixLen + 3u];
    FatPath(fat, path);

    /* Final-target check: refuse silent replacement without `overwrite`,
     * and never target a directory. */
    FILINFO fno;
    FRESULT fr = f_stat(fat, &fno);
    if (fr == FR_OK)
    {
        if (fno.fattrib & AM_DIR)
        {
            w.U8(static_cast<uint8_t>(Status::BadArgs));
            return;
        }
        if (!(flags & kFsWriteOverwrite))
        {
            w.U8(static_cast<uint8_t>(Status::FsExists));
            return;
        }
    }
    else if (fr != FR_NO_FILE && fr != FR_NO_PATH)
    {
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }
    else if (fr == FR_NO_PATH || (flags & kFsWriteMkdirs))
    {
        /* Parent creation on request.  Walk prefixes; FR_EXIST is fine. */
        if (flags & kFsWriteMkdirs)
        {
            char prefix[kFsMaxPath + 4u];
            FatPath(prefix, path);
            for (char* p = prefix + 3u; *p; p++)
            {
                if (*p != '/') continue;
                *p = '\0';
                const FRESULT mk = f_mkdir(prefix);
                if (mk != FR_OK && mk != FR_EXIST)
                {
                    w.U8(static_cast<uint8_t>(MapFr(card_, mk)));
                    return;
                }
                *p = '/';
            }
        }
        else if (fr == FR_NO_PATH)
        {
            w.U8(static_cast<uint8_t>(Status::FsNoFile));
            return;
        }
    }

    std::strcpy(s_xfer_path, path);
    std::strcpy(s_part_path, path);  std::strcat(s_part_path, ".part");
    std::strcpy(s_meta_path, path);  std::strcat(s_meta_path, ".partmeta");

    /* Resume: a consistent checkpoint re-anchors the transfer (§8.4);
     * anything inconsistent falls through to a clean restart. */
    uint32_t resume_off = 0u;
    uint32_t resume_crc = 0u;
    if (flags & kFsWriteResume)
    {
        char meta_fat[sizeof(s_meta_path) + 2u];
        FatPath(meta_fat, s_meta_path);
        if (f_open(&s_meta_fil, meta_fat, FA_READ) == FR_OK)
        {
            uint8_t meta[kMetaBytes];
            UINT    br = 0u;
            const bool read_ok =
                f_read(&s_meta_fil, s_staging, kMetaBytes, &br) == FR_OK
                && br == kMetaBytes;
            f_close(&s_meta_fil);
            if (read_ok)
            {
                std::memcpy(meta, s_staging, kMetaBytes);
                const uint32_t magic    = RdU32(meta + 0u);
                const uint32_t m_total  = RdU32(meta + 4u);
                const uint32_t received = RdU32(meta + 8u);
                const uint32_t crc      = RdU32(meta + 12u);

                char part_fat[sizeof(s_part_path) + 2u];
                FatPath(part_fat, s_part_path);
                FILINFO pf;
                if (magic == kMetaMagic && m_total == total
                    && received <= total
                    && f_stat(part_fat, &pf) == FR_OK
                    && static_cast<uint32_t>(pf.fsize) >= received)
                {
                    resume_off = received;
                    resume_crc = crc;
                }
            }
        }
    }

    char part_fat[sizeof(s_part_path) + 2u];
    FatPath(part_fat, s_part_path);
    fr = f_open(&s_fil, part_fat,
                resume_off ? (FA_WRITE | FA_OPEN_ALWAYS)
                           : (FA_WRITE | FA_CREATE_ALWAYS));
    if (fr == FR_OK && resume_off)
    {
        /* Drop any staged bytes beyond the checkpoint — they were never
         * covered by a meta sync and the host will resend them. */
        fr = f_lseek(&s_fil, resume_off);
        if (fr == FR_OK) fr = f_truncate(&s_fil);
    }
    if (fr != FR_OK)
    {
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }

    mode_         = XferMode::Write;
    write_flags_  = flags;
    xfer_size_    = total;
    xfer_pos_     = resume_off;
    xfer_crc_     = resume_crc;
    meta_synced_  = resume_off;
    xfer_last_ms_ = now_ms;

    /* Checkpoint now: the pair exists on disk before any data, so even
     * an immediate power cut leaves a resumable (or cleanly deletable)
     * staging state, and the directory entry is findable. */
    if (!WriteMeta() || f_sync(&s_fil) != FR_OK)
    {
        CloseTransfer(false);
        w.U8(static_cast<uint8_t>(Status::FsIo));
        return;
    }

    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U32(resume_off);
}

void FsExtension::OnWrite(const ParsedFrame& f, FrameWriter& w,
                          uint32_t now_ms)
{
    if (mode_ != XferMode::Write)
    {
        w.U8(static_cast<uint8_t>(Status::BadState));
        return;
    }

    BodyReader r(f);
    const uint32_t offset = r.U32();
    uint16_t       n      = 0u;
    const uint8_t* data   = r.Rest(n);
    if (!r.ok)
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    xfer_last_ms_ = now_ms;

    if (offset != xfer_pos_)
    {
        /* Strict in-order (§8): session aborts, staging survives for a
         * clean restart/resume. */
        CloseTransfer(true);
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (xfer_pos_ + n > xfer_size_)
    {
        CloseTransfer(true);
        w.U8(static_cast<uint8_t>(Status::TooLarge));
        return;
    }

    if (n > 0u)
    {
        /* Frame body (engine-side) → staging → disk, with the staging
         * pointer padded so FatFS's direct-sector DMA source is always
         * word-aligned (sd_card.h placement rule 3).  The upload chunk size happens
         * to keep this zero today; resume offsets and future chunk
         * sizes must not be able to regress it. */
        const uint32_t pad = SdDmaStagingPad(xfer_pos_);
        std::memcpy(s_staging + pad, data, n);
        UINT bw = 0u;
        const FRESULT fr = f_write(&s_fil, s_staging + pad, n, &bw);
        if (fr != FR_OK || bw != n)
        {
            const Status st = (fr == FR_OK) ? Status::FsFull
                                            : MapFr(card_, fr);
            CloseTransfer(true);
            w.U8(static_cast<uint8_t>(st));
            return;
        }
        xfer_crc_ = Crc32(s_staging + pad, n, xfer_crc_);
        xfer_pos_ += n;
    }

    if (xfer_pos_ - meta_synced_ >= kMetaIntervalBytes)
    {
        if (f_sync(&s_fil) != FR_OK || !WriteMeta())
        {
            CloseTransfer(false);
            w.U8(static_cast<uint8_t>(Status::FsIo));
            return;
        }
        meta_synced_ = xfer_pos_;
    }

    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U32(xfer_pos_);
}

void FsExtension::OnCommit(const ParsedFrame& f, FrameWriter& w)
{
    if (mode_ != XferMode::Write)
    {
        w.U8(static_cast<uint8_t>(Status::BadState));
        return;
    }

    BodyReader r(f);
    const uint32_t want_crc = r.U32();
    if (!r.ok)
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }

    if (xfer_pos_ != xfer_size_)
    {
        w.U8(static_cast<uint8_t>(Status::BadState));
        return;
    }
    if (xfer_crc_ != want_crc)
    {
        /* Keep staging (host may resume / re-verify); session ends. */
        CloseTransfer(true);
        w.U8(static_cast<uint8_t>(Status::BadCrc));
        return;
    }

    FRESULT fr = f_close(&s_fil);
    if (fr != FR_OK)
    {
        mode_ = XferMode::None;
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }
    mode_ = XferMode::None;

    char final_fat[kFsMaxPath + 4u];
    char part_fat [sizeof(s_part_path) + 2u];
    char meta_fat [sizeof(s_meta_path) + 2u];
    FatPath(final_fat, s_xfer_path);
    FatPath(part_fat,  s_part_path);
    FatPath(meta_fat,  s_meta_path);

    /* Re-check the target: it may have appeared since FS_OPEN_WRITE. */
    FILINFO fno;
    fr = f_stat(final_fat, &fno);
    if (fr == FR_OK)
    {
        if (!(write_flags_ & kFsWriteOverwrite) || (fno.fattrib & AM_DIR))
        {
            w.U8(static_cast<uint8_t>(Status::FsExists));
            return;
        }
        fr = f_unlink(final_fat);
        if (fr != FR_OK)
        {
            w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
            return;
        }
    }
    else if (fr != FR_NO_FILE && fr != FR_NO_PATH)
    {
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }

    fr = f_rename(part_fat, final_fat);
    if (fr != FR_OK)
    {
        w.U8(static_cast<uint8_t>(MapFr(card_, fr)));
        return;
    }
    f_unlink(meta_fat);   /* best-effort; an orphan meta is harmless */

    w.U8(static_cast<uint8_t>(Status::Ok));
}

/* ── FS_DELETE / FS_MKDIR / FS_RENAME ───────────────────────────────────── */

void FsExtension::OnDelete(const ParsedFrame& f, FrameWriter& w,
                           uint32_t now_ms)
{
    BodyReader r(f);
    char path[kFsMaxPath + 1u];
    if (!r.Str(path, sizeof(path)) || !ValidatePath(path, false))
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (!card_.EnsureMounted(now_ms))
    {
        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        return;
    }
    if (card_.IsLocked(path))
    {
        w.U8(static_cast<uint8_t>(Status::FsLocked));
        return;
    }
    if (CollidesWithTransfer(path))
    {
        w.U8(static_cast<uint8_t>(Status::BadState));
        return;
    }

    char fat[kFsMaxPath + 4u];
    FatPath(fat, path);
    w.U8(static_cast<uint8_t>(MapFr(card_, f_unlink(fat))));
}

void FsExtension::OnMkdir(const ParsedFrame& f, FrameWriter& w,
                          uint32_t now_ms)
{
    BodyReader r(f);
    char path[kFsMaxPath + 1u];
    if (!r.Str(path, sizeof(path)) || !ValidatePath(path, false))
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (!card_.EnsureMounted(now_ms))
    {
        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        return;
    }

    char fat[kFsMaxPath + 4u];
    FatPath(fat, path);
    w.U8(static_cast<uint8_t>(MapFr(card_, f_mkdir(fat))));
}

void FsExtension::OnRename(const ParsedFrame& f, FrameWriter& w,
                           uint32_t now_ms)
{
    BodyReader r(f);
    char from[kFsMaxPath + 1u];
    char to  [kFsMaxPath + 1u];
    if (!r.Str(from, sizeof(from)) || !ValidatePath(from, false)
        || !r.Str(to, sizeof(to))  || !ValidatePath(to, false))
    {
        w.U8(static_cast<uint8_t>(Status::BadArgs));
        return;
    }
    if (!card_.EnsureMounted(now_ms))
    {
        w.U8(static_cast<uint8_t>(Status::FsNoCard));
        return;
    }
    if (card_.IsLocked(from) || card_.IsLocked(to))
    {
        w.U8(static_cast<uint8_t>(Status::FsLocked));
        return;
    }
    if (CollidesWithTransfer(from) || CollidesWithTransfer(to))
    {
        w.U8(static_cast<uint8_t>(Status::BadState));
        return;
    }

    char fat_from[kFsMaxPath + 4u];
    char fat_to  [kFsMaxPath + 4u];
    FatPath(fat_from, from);
    FatPath(fat_to,   to);
    w.U8(static_cast<uint8_t>(MapFr(card_, f_rename(fat_from, fat_to))));
}

/* ── Lifecycle helpers ──────────────────────────────────────────────────── */

bool FsExtension::WriteMeta()
{
    uint8_t meta[kMetaBytes];
    WrU32(meta + 0u,  kMetaMagic);
    WrU32(meta + 4u,  xfer_size_);
    WrU32(meta + 8u,  xfer_pos_);
    WrU32(meta + 12u, xfer_crc_);
    std::memcpy(s_staging, meta, kMetaBytes);

    char meta_fat[sizeof(s_meta_path) + 2u];
    FatPath(meta_fat, s_meta_path);

    FRESULT fr = f_open(&s_meta_fil, meta_fat, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) return false;
    UINT bw = 0u;
    fr = f_write(&s_meta_fil, s_staging, kMetaBytes, &bw);
    const FRESULT frc = f_close(&s_meta_fil);
    return fr == FR_OK && bw == kMetaBytes && frc == FR_OK;
}

void FsExtension::CloseTransfer(bool checkpoint_meta)
{
    if (mode_ == XferMode::None) return;

    if (mode_ == XferMode::Write)
    {
        f_sync(&s_fil);
        if (checkpoint_meta) WriteMeta();
    }
    f_close(&s_fil);
    mode_ = XferMode::None;
}

void FsExtension::CloseDirCursor()
{
    if (!dir_open_) return;
    f_closedir(&s_dir);
    dir_open_      = false;
    pending_valid_ = false;
    dir_index_     = 0u;
    dir_emitted_   = 0u;
}

bool FsExtension::CollidesWithTransfer(const char* path) const
{
    if (mode_ == XferMode::None) return false;
    return PathEqCI(path, s_xfer_path)
        || PathEqCI(path, s_part_path)
        || PathEqCI(path, s_meta_path);
}

} // namespace hostlink
} // namespace alchemy
