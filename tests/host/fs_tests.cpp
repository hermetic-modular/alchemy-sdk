/**
 * @file tests/host/fs_tests.cpp
 * @brief Protocol §8 filesystem block — host-side integration tests.
 *
 * The full production stack runs in-process: HostLink engine →
 * FsExtension → SdCard → real ff.c → RAM diskio.  Scenarios cover the
 * whole command matrix plus the failure modes that only fault injection
 * can reach — card yank mid-transfer, power-cut/resume against the
 * .partmeta checkpoint, stale listing cursors, and lock enforcement.
 */

#include "fs_tests.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "alchemy/host_link/crc32.h"
#include "alchemy/host_link/frame.h"
#include "alchemy/host_link/host_link.h"
#include "alchemy/host_link/wire.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/presets.h"
#include "ram_diskio.h"

using namespace alchemy;
using namespace alchemy::hostlink;

/* ── Local check harness (totals returned to main) ─────────────────── */

static int s_checks   = 0;
static int s_failures = 0;

#define FCHECK(cond)                                                       \
    do {                                                                   \
        s_checks++;                                                        \
        if (!(cond)) {                                                     \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define FCHECK_EQ(a, b)                                                    \
    do {                                                                   \
        s_checks++;                                                        \
        const auto va = (a);                                               \
        const auto vb = (b);                                               \
        if (!(va == vb)) {                                                 \
            s_failures++;                                                  \
            std::printf("FAIL %s:%d  %s == %s  (%llu != %llu)\n",          \
                        __FILE__, __LINE__, #a, #b,                        \
                        (unsigned long long)va, (unsigned long long)vb);   \
        }                                                                  \
    } while (0)

namespace {

/* ── Fixture ───────────────────────────────────────────────────────── */

struct FsLoopTransport : IHostTransport
{
    std::deque<uint8_t>  rx;
    std::vector<uint8_t> tx;

    size_t Read(uint8_t* dst, size_t max) override
    {
        size_t n = 0;
        while (!rx.empty() && n < max) { dst[n++] = rx.front(); rx.pop_front(); }
        return n;
    }
    size_t Write(const uint8_t* src, size_t n) override
    {
        tx.insert(tx.end(), src, src + n);
        return n;
    }
    size_t WriteSpace() const override { return 1u << 16; }
};

static daisy::QSPIHandle g_fs_qspi;   /* stubbed on host */

/** Format the RAM disk FAT32 through a scratch mount. */
bool FormatRamDisk()
{
    ramdisk::Reset(131072u);   /* 64 MiB → FAT32 with 512 B clusters */
    static FATFS scratch;
    std::memset(&scratch, 0, sizeof(scratch));
    if (f_mount(&scratch, "0:", 0) != FR_OK) return false;
    std::vector<uint8_t> work(4096u);
    const FRESULT fr = f_mkfs("0:", FM_FAT32 | FM_SFD, 512u,
                              work.data(), static_cast<UINT>(work.size()));
    f_mount(nullptr, "0:", 0);
    return fr == FR_OK;
}

struct FsFixture
{
    FsLoopTransport t;
    Presets         presets{g_fs_qspi};
    SdCard          sd;
    FsExtension     fs{sd};
    std::vector<uint8_t> staging  = std::vector<uint8_t>(1024u);
    std::vector<uint8_t> snapshot = std::vector<uint8_t>(1024u);
    HostLink        link;
    uint16_t        next_seq = 1u;
    uint32_t        now_ms   = 1000u;

    explicit FsFixture(bool format = true)
        : link(t, presets,
               HostLink::Info{"fsmod", "FS Module", "0.1.0", "cafe123",
                              "1.2.0-beta", 2u, 0u},
               staging.data(), snapshot.data(), staging.size())
    {
        if (format) FCHECK(FormatRamDisk());
        sd.Init();
        FCHECK(link.Extend(fs));
    }

    /** Simulated reboot: services restart, the card image persists. */
    void Reboot()
    {
        sd.Init();
        fs.Reset();
    }

    void Advance(uint32_t ms) { now_ms += ms; }
};

struct FsResp
{
    bool                 got = false;
    uint8_t              type = 0;
    std::vector<uint8_t> body;

    uint8_t  Status() const { return body.empty() ? 0xFE : body[0]; }
    uint8_t  U8(size_t off)  const { return body[off]; }
    uint16_t U16(size_t off) const { return RdU16(body.data() + off); }
    uint32_t U32(size_t off) const { return RdU32(body.data() + off); }
};

FsResp Transact(FsFixture& fx, Cmd cmd, const std::vector<uint8_t>& body,
                std::vector<uint8_t>* wire_req  = nullptr,
                std::vector<uint8_t>* wire_resp = nullptr)
{
    uint8_t dec[kMaxDecoded], wire[kMaxWire];
    FrameWriter w(dec);
    w.Begin(static_cast<uint8_t>(cmd), fx.next_seq++);
    w.Bytes(body.data(), body.size());
    const size_t n = w.Encode(wire);
    if (wire_req) wire_req->assign(wire, wire + n);
    fx.t.rx.insert(fx.t.rx.end(), wire, wire + n);

    fx.t.tx.clear();
    fx.link.Poll(fx.now_ms);
    if (wire_resp) wire_resp->assign(fx.t.tx.begin(), fx.t.tx.end());

    FsResp r;
    FrameParser p;
    for (uint8_t b : fx.t.tx)
    {
        ParsedFrame f;
        if (p.Push(b, f) && f.ok)
        {
            r.got  = true;
            r.type = f.type;
            r.body.assign(f.body, f.body + f.len);
        }
    }
    return r;
}

/* ── Body builders ─────────────────────────────────────────────────── */

void PutStr(std::vector<uint8_t>& b, const char* s)
{
    const size_t n = std::strlen(s);
    b.push_back(static_cast<uint8_t>(n));
    b.insert(b.end(), s, s + n);
}

std::vector<uint8_t> PathBody(const char* path)
{
    std::vector<uint8_t> b;
    PutStr(b, path);
    return b;
}

std::vector<uint8_t> ListBody(uint32_t cookie, const char* path)
{
    std::vector<uint8_t> b(4);
    WrU32(b.data(), cookie);
    PutStr(b, path);
    return b;
}

std::vector<uint8_t> OpenWriteBody(uint8_t flags, uint32_t total,
                                   const char* path)
{
    std::vector<uint8_t> b(5);
    b[0] = flags;
    WrU32(b.data() + 1, total);
    PutStr(b, path);
    return b;
}

std::vector<uint8_t> WriteBody(uint32_t offset,
                               const uint8_t* data, size_t n)
{
    std::vector<uint8_t> b(4 + n);
    WrU32(b.data(), offset);
    std::memcpy(b.data() + 4, data, n);
    return b;
}

std::vector<uint8_t> ReadBody(uint32_t offset, uint16_t max_len)
{
    std::vector<uint8_t> b(6);
    WrU32(b.data(), offset);
    WrU16(b.data() + 4, max_len);
    return b;
}

std::vector<uint8_t> U32Only(uint32_t v)
{
    std::vector<uint8_t> b(4);
    WrU32(b.data(), v);
    return b;
}

std::vector<uint8_t> Pattern(size_t n, uint8_t seed = 0u)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = static_cast<uint8_t>((i * 7u + seed) & 0xFFu);
    return v;
}

/** Upload @p data to @p path in @p chunk-sized FS_WRITEs and commit. */
bool Upload(FsFixture& fx, const char* path,
            const std::vector<uint8_t>& data,
            uint8_t flags = 0u, size_t chunk = 500u)
{
    auto r = Transact(fx, Cmd::FsOpenWrite,
                      OpenWriteBody(flags, uint32_t(data.size()), path));
    if (r.Status() != uint8_t(Status::Ok)) return false;
    for (size_t off = 0; off < data.size(); off += chunk)
    {
        const size_t n = (data.size() - off < chunk) ? data.size() - off : chunk;
        r = Transact(fx, Cmd::FsWrite, WriteBody(uint32_t(off), data.data() + off, n));
        if (r.Status() != uint8_t(Status::Ok)) return false;
    }
    r = Transact(fx, Cmd::FsCommit,
                 U32Only(Crc32(data.data(), data.size())));
    return r.Status() == uint8_t(Status::Ok);
}

/** Download the open-read transfer fully, using the web client's chunk
 *  size (max_body − 7 — deliberately odd, cycling every direct-DMA
 *  alignment phase like the real download path). */
constexpr uint16_t kReadChunk = kMaxBody - 7u;

std::vector<uint8_t> ReadAll(FsFixture& fx, uint32_t size)
{
    std::vector<uint8_t> out;
    while (out.size() < size)
    {
        auto r = Transact(fx, Cmd::FsRead,
                          ReadBody(uint32_t(out.size()), kReadChunk));
        if (r.Status() != uint8_t(Status::Ok)) break;
        const uint16_t n = r.U16(5);
        if (n == 0u) break;
        out.insert(out.end(), r.body.begin() + 7, r.body.begin() + 7 + n);
    }
    return out;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

void TestInfoAndProbe(FsFixture& fx)
{
    /* Unknown command past the claimed range → engine UNSUPPORTED. */
    auto r = Transact(fx, static_cast<Cmd>(0x5Cu), {});
    FCHECK_EQ(r.Status(), uint8_t(Status::Unsupported));

    r = Transact(fx, Cmd::FsInfo, {});
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U8(1), 1u);                    /* mounted           */
    FCHECK_EQ(r.U8(2), uint8_t(FS_FAT32));     /* fs_type           */
    FCHECK(r.U8(3) & kFsInfoFreeValid);        /* FSINFO seeded     */
    FCHECK(r.U32(4) > 60000u);                 /* total ≈ 64 MiB    */
    FCHECK(r.U32(8) > 0u && r.U32(8) <= r.U32(4));
    FCHECK_EQ(r.U16(12), uint16_t(kFsMaxSegment));
}

void TestPathValidation(FsFixture& fx)
{
    for (const char* bad : {"noleading", "/tr/", "/a//b", "/../x", "/a/../b",
                            "/.", "/a\\b", "/a:b", "/a?b", "/end.", "/end "})
    {
        auto r = Transact(fx, Cmd::FsStat, PathBody(bad));
        FCHECK_EQ(r.Status(), uint8_t(Status::BadArgs));
    }

    /* Overlong: 195-char path. */
    std::string longpath = "/";
    while (longpath.size() < 195u) longpath += "abcdefghij/";
    auto r = Transact(fx, Cmd::FsStat,
                      PathBody(longpath.substr(0, 195).c_str()));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadArgs));

    /* Root stat synthesizes a directory. */
    r = Transact(fx, Cmd::FsStat, PathBody("/"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK(r.U8(1) & kFsEntryDir);
}

void TestMkdirDelete(FsFixture& fx)
{
    FCHECK_EQ(Transact(fx, Cmd::FsMkdir, PathBody("/A")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsMkdir, PathBody("/A/B")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsMkdir, PathBody("/A")).Status(),
              uint8_t(Status::FsExists));
    FCHECK_EQ(Transact(fx, Cmd::FsMkdir, PathBody("/no/parent")).Status(),
              uint8_t(Status::FsNoFile));

    /* Non-empty dir refuses deletion; leaf-first works. */
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/A")).Status(),
              uint8_t(Status::BadState));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/A/B")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/A")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/A")).Status(),
              uint8_t(Status::FsNoFile));
}

void TestUploadDownloadRoundtrip(FsFixture& fx)
{
    const auto data = Pattern(1234u);
    FCHECK(Upload(fx, "/round.bin", data));

    /* Staging files must be gone after commit. */
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/round.bin.part")).Status(),
              uint8_t(Status::FsNoFile));
    FCHECK_EQ(Transact(fx, Cmd::FsStat,
                       PathBody("/round.bin.partmeta")).Status(),
              uint8_t(Status::FsNoFile));

    auto r = Transact(fx, Cmd::FsStat, PathBody("/round.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U32(2), 1234u);
    FCHECK(r.U32(6) != 0u);   /* deterministic fattime, non-zero */

    r = Transact(fx, Cmd::FsOpenRead, PathBody("/round.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U32(1), 1234u);
    const auto back = ReadAll(fx, 1234u);
    FCHECK(back == data);

    /* Reads are seekable + idempotent; beyond-EOF reads n = 0. */
    r = Transact(fx, Cmd::FsRead, ReadBody(100u, 16u));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U32(1), 100u);
    FCHECK_EQ(r.U16(5), 16u);
    FCHECK(std::memcmp(r.body.data() + 7, data.data() + 100, 16) == 0);
    r = Transact(fx, Cmd::FsRead, ReadBody(5000u, 16u));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U16(5), 0u);

    FCHECK_EQ(Transact(fx, Cmd::FsClose, {}).Status(), uint8_t(Status::Ok));
    /* Closed session: FS_READ now illegal. */
    FCHECK_EQ(Transact(fx, Cmd::FsRead, ReadBody(0u, 16u)).Status(),
              uint8_t(Status::BadState));

    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/round.bin")).Status(),
              uint8_t(Status::Ok));
}

void TestOverwriteSemantics(FsFixture& fx)
{
    FCHECK(Upload(fx, "/ow.bin", Pattern(64u)));

    /* Existing target without `overwrite` refuses at open. */
    auto r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 10u, "/ow.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::FsExists));

    /* With `overwrite` the new content replaces the old. */
    const auto v2 = Pattern(32u, 9u);
    FCHECK(Upload(fx, "/ow.bin", v2, kFsWriteOverwrite));
    r = Transact(fx, Cmd::FsStat, PathBody("/ow.bin"));
    FCHECK_EQ(r.U32(2), 32u);

    /* Reserved staging suffixes refuse as upload targets. */
    r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 4u, "/x.part"));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadArgs));
    r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 4u, "/x.partmeta"));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadArgs));

    /* Parent handling: absent parent fails plain, mkdirs creates it. */
    r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 4u, "/deep/f.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::FsNoFile));
    FCHECK(Upload(fx, "/deep/f.bin", Pattern(4u), kFsWriteMkdirs));
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/deep")).Status(),
              uint8_t(Status::Ok));

    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/deep/f.bin")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/deep")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/ow.bin")).Status(),
              uint8_t(Status::Ok));

    /* Zero-byte upload is legal (CRC of nothing = 0). */
    FCHECK(Upload(fx, "/empty.bin", {}));
    r = Transact(fx, Cmd::FsStat, PathBody("/empty.bin"));
    FCHECK_EQ(r.U32(2), 0u);
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/empty.bin")).Status(),
              uint8_t(Status::Ok));
}

void TestStrictOrderAndBadCrc(FsFixture& fx)
{
    const auto data = Pattern(600u);
    auto r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 600u, "/ord.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    r = Transact(fx, Cmd::FsWrite, WriteBody(0u, data.data(), 300u));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));

    /* Out-of-order offset kills the session but keeps staging. */
    r = Transact(fx, Cmd::FsWrite, WriteBody(600u, data.data(), 100u));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadArgs));
    r = Transact(fx, Cmd::FsWrite, WriteBody(300u, data.data() + 300u, 100u));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadState));
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/ord.bin.part")).Status(),
              uint8_t(Status::Ok));

    /* Bad commit CRC: session ends, staging survives, final absent. */
    r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 600u, "/ord.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    r = Transact(fx, Cmd::FsWrite, WriteBody(0u, data.data(), 500u));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    r = Transact(fx, Cmd::FsWrite, WriteBody(500u, data.data() + 500u, 100u));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    r = Transact(fx, Cmd::FsCommit, U32Only(0xDEADBEEFu));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadCrc));
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/ord.bin")).Status(),
              uint8_t(Status::FsNoFile));
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/ord.bin.part")).Status(),
              uint8_t(Status::Ok));

    /* Clean retry succeeds and clears staging. */
    FCHECK(Upload(fx, "/ord.bin", data));
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/ord.bin.part")).Status(),
              uint8_t(Status::FsNoFile));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/ord.bin")).Status(),
              uint8_t(Status::Ok));

    /* Commit with staged != total → BAD_STATE. */
    r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 100u, "/short.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    r = Transact(fx, Cmd::FsCommit, U32Only(0u));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadState));
    Transact(fx, Cmd::FsClose, {});
    Transact(fx, Cmd::FsDelete, PathBody("/short.bin.part"));
    Transact(fx, Cmd::FsDelete, PathBody("/short.bin.partmeta"));
}

void TestTransferTimeout(FsFixture& fx)
{
    auto r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, 100u, "/tmo.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));

    fx.Advance(FsExtension::kXferTimeoutMs + 500u);
    /* Any poll expires the transfer; the next write is stateless. */
    r = Transact(fx, Cmd::FsInfo, {});
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    const auto d = Pattern(10u);
    r = Transact(fx, Cmd::FsWrite, WriteBody(0u, d.data(), d.size()));
    FCHECK_EQ(r.Status(), uint8_t(Status::BadState));

    /* Staging survives the timeout (resumable), and is deletable. */
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/tmo.bin.part")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/tmo.bin.part")).Status(),
              uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsDelete,
                       PathBody("/tmo.bin.partmeta")).Status(),
              uint8_t(Status::Ok));
}

void TestListingPagination(FsFixture& fx)
{
    FCHECK_EQ(Transact(fx, Cmd::FsMkdir, PathBody("/many")).Status(),
              uint8_t(Status::Ok));
    for (int i = 0; i < 120; i++)
    {
        char p[64];
        std::snprintf(p, sizeof(p), "/many/file_%03d.dat", i);
        FCHECK(Upload(fx, p, Pattern(8u, uint8_t(i))));
    }

    /* Collect the listing across however many pages it takes. */
    std::vector<std::string> names;
    uint32_t cookie = 0u;
    int      pages  = 0;
    while (true)
    {
        auto r = Transact(fx, Cmd::FsList, ListBody(cookie, "/many"));
        FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
        const uint32_t next  = r.U32(1);
        const uint8_t  count = r.U8(5);
        size_t off = 6u;
        for (uint8_t i = 0; i < count; i++)
        {
            const uint8_t flags = r.U8(off);
            const uint32_t size = r.U32(off + 1u);
            const uint8_t  nlen = r.U8(off + 9u);
            names.emplace_back(reinterpret_cast<const char*>(
                                   r.body.data() + off + 10u), nlen);
            FCHECK_EQ(flags & kFsEntryDir, 0u);
            FCHECK_EQ(size, 8u);
            off += 10u + nlen;
        }
        pages++;
        if (next == kFsListEnd) break;
        cookie = next;
        FCHECK(pages < 10);
        if (pages >= 10) break;
    }
    FCHECK_EQ(names.size(), size_t(120));
    FCHECK(pages >= 2);   /* 120 × ~22 B entries cannot fit one frame */

    /* Every file appears exactly once. */
    for (int i = 0; i < 120; i++)
    {
        char n[32];
        std::snprintf(n, sizeof(n), "file_%03d.dat", i);
        int found = 0;
        for (const auto& s : names) if (s == n) found++;
        FCHECK_EQ(found, 1);
    }

    /* Stale cookie after cursor eviction (listing "/" in between)
     * still completes via reopen-and-skip. */
    auto r1 = Transact(fx, Cmd::FsList, ListBody(0u, "/many"));
    const uint32_t resume_cookie = r1.U32(1);
    FCHECK(resume_cookie != kFsListEnd);
    Transact(fx, Cmd::FsList, ListBody(0u, "/"));        /* evicts cursor */
    size_t rest = 0u;
    cookie = resume_cookie;
    while (true)
    {
        auto r = Transact(fx, Cmd::FsList, ListBody(cookie, "/many"));
        FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
        rest += r.U8(5);
        if (r.U32(1) == kFsListEnd) break;
        cookie = r.U32(1);
    }
    FCHECK_EQ(rest + resume_cookie, size_t(120));

    for (int i = 0; i < 120; i++)
    {
        char p[64];
        std::snprintf(p, sizeof(p), "/many/file_%03d.dat", i);
        Transact(fx, Cmd::FsDelete, PathBody(p));
    }
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/many")).Status(),
              uint8_t(Status::Ok));
}

void TestLocks(FsFixture& fx)
{
    FCHECK(Upload(fx, "/rec.wav", Pattern(256u)));
    FCHECK(fx.sd.Lock("/rec.wav"));

    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/rec.wav")).Status(),
              uint8_t(Status::FsLocked));
    std::vector<uint8_t> ren;
    PutStr(ren, "/rec.wav");
    PutStr(ren, "/rec2.wav");
    FCHECK_EQ(Transact(fx, Cmd::FsRename, ren).Status(),
              uint8_t(Status::FsLocked));
    FCHECK_EQ(Transact(fx, Cmd::FsOpenWrite,
                       OpenWriteBody(kFsWriteOverwrite, 8u, "/rec.wav")).Status(),
              uint8_t(Status::FsLocked));

    /* Reads of firmware-held files are allowed. */
    auto r = Transact(fx, Cmd::FsOpenRead, PathBody("/rec.wav"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    Transact(fx, Cmd::FsClose, {});

    /* Locked + fw-busy surface in listings and FS_INFO. */
    r = Transact(fx, Cmd::FsList, ListBody(0u, "/"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    bool saw_locked = false;
    {
        const uint8_t count = r.U8(5);
        size_t off = 6u;
        for (uint8_t i = 0; i < count; i++)
        {
            const uint8_t flags = r.U8(off);
            const uint8_t nlen  = r.U8(off + 9u);
            const std::string name(
                reinterpret_cast<const char*>(r.body.data() + off + 10u), nlen);
            if (name == "rec.wav" && (flags & kFsEntryLocked)) saw_locked = true;
            off += 10u + nlen;
        }
    }
    FCHECK(saw_locked);
    r = Transact(fx, Cmd::FsInfo, {});
    FCHECK(r.U8(3) & kFsInfoFwBusy);

    fx.sd.Unlock("/rec.wav");
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/rec.wav")).Status(),
              uint8_t(Status::Ok));
}

void TestCardYankAndRecovery(FsFixture& fx)
{
    FCHECK(Upload(fx, "/yank.bin", Pattern(2048u)));
    auto r = Transact(fx, Cmd::FsOpenRead, PathBody("/yank.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));

    ramdisk::FailAll(true);
    r = Transact(fx, Cmd::FsRead, ReadBody(0u, 505u));
    FCHECK_EQ(r.Status(), uint8_t(Status::FsIo));

    /* The volume is flagged; FS_INFO reports no card while faulted,
     * carrying the media-diagnostic trailer (§8.3): mount FRESULT,
     * failed disk_init, both sector probes skipped. */
    fx.Advance(SdCard::kRemountBackoffMs + 100u);
    r = Transact(fx, Cmd::FsInfo, {});
    FCHECK_EQ(r.Status(), uint8_t(Status::FsNoCard));
    FCHECK_EQ(r.body.size(), size_t(51));
    FCHECK(r.U8(1) != 0u);    /* mount_fr: a real FRESULT      */
    FCHECK(r.U8(2) != 0u);    /* disk_init failed while yanked */
    FCHECK_EQ(r.U8(3), 0xFFu); /* sector-0 probe not attempted */

    /* Re-seat: backoff elapses, remount succeeds, data intact. */
    ramdisk::ClearFaults();
    fx.Advance(SdCard::kRemountBackoffMs + 100u);
    r = Transact(fx, Cmd::FsInfo, {});
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    r = Transact(fx, Cmd::FsOpenRead, PathBody("/yank.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK(ReadAll(fx, 2048u) == Pattern(2048u));
    Transact(fx, Cmd::FsClose, {});
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/yank.bin")).Status(),
              uint8_t(Status::Ok));
}

void TestPowerCutResume(FsFixture& fx)
{
    /* 1.5 MiB upload in 500-byte chunks; mirror the firmware's meta
     * cadence to predict the checkpoint. */
    const uint32_t total = 1536u * 1024u;
    const auto     data  = Pattern(total);

    auto r = Transact(fx, Cmd::FsOpenWrite, OpenWriteBody(0u, total, "/big.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U32(1), 0u);

    uint32_t pos = 0u, synced = 0u;
    bool     cut = false;
    while (pos < total)
    {
        const uint32_t n = (total - pos < 500u) ? (total - pos) : 500u;

        /* Pull the plug a bit past the first checkpoint. */
        if (!cut && synced > 0u && pos >= synced + 64u * 1024u)
        {
            ramdisk::FailAll(true);
            cut = true;
            r = Transact(fx, Cmd::FsWrite, WriteBody(pos, data.data() + pos, n));
            FCHECK(r.Status() != uint8_t(Status::Ok));
            break;
        }

        r = Transact(fx, Cmd::FsWrite, WriteBody(pos, data.data() + pos, n));
        FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
        pos += n;
        if (pos - synced >= FsExtension::kMetaIntervalBytes) synced = pos;
    }
    FCHECK(cut);
    FCHECK(synced > 0u);

    /* "Reboot": services restart, image persists, faults cleared. */
    ramdisk::ClearFaults();
    fx.Reboot();
    fx.Advance(SdCard::kRemountBackoffMs + 100u);

    r = Transact(fx, Cmd::FsOpenWrite,
                 OpenWriteBody(kFsWriteResume, total, "/big.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    const uint32_t resume = r.U32(1);
    FCHECK_EQ(resume, synced);

    for (pos = resume; pos < total;)
    {
        const uint32_t n = (total - pos < 500u) ? (total - pos) : 500u;
        r = Transact(fx, Cmd::FsWrite, WriteBody(pos, data.data() + pos, n));
        FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
        pos += n;
    }
    r = Transact(fx, Cmd::FsCommit, U32Only(Crc32(data.data(), data.size())));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));

    /* Whole-file verification after the splice. */
    r = Transact(fx, Cmd::FsOpenRead, PathBody("/big.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U32(1), total);
    FCHECK(ReadAll(fx, total) == data);
    Transact(fx, Cmd::FsClose, {});
    FCHECK_EQ(Transact(fx, Cmd::FsDelete, PathBody("/big.bin")).Status(),
              uint8_t(Status::Ok));

    /* Resume with a mismatched total restarts cleanly at 0. */
    FCHECK(Upload(fx, "/mism.bin", Pattern(600u)));
    r = Transact(fx, Cmd::FsOpenWrite,
                 OpenWriteBody(kFsWriteResume | kFsWriteOverwrite, 700u,
                               "/mism.bin"));
    FCHECK_EQ(r.Status(), uint8_t(Status::Ok));
    FCHECK_EQ(r.U32(1), 0u);
    Transact(fx, Cmd::FsClose, {});
    Transact(fx, Cmd::FsDelete, PathBody("/mism.bin.part"));
    Transact(fx, Cmd::FsDelete, PathBody("/mism.bin.partmeta"));
    Transact(fx, Cmd::FsDelete, PathBody("/mism.bin"));
}

void TestRename(FsFixture& fx)
{
    FCHECK(Upload(fx, "/rn_a.bin", Pattern(100u)));
    FCHECK_EQ(Transact(fx, Cmd::FsMkdir, PathBody("/rn_dir")).Status(),
              uint8_t(Status::Ok));

    std::vector<uint8_t> ren;
    PutStr(ren, "/rn_a.bin");
    PutStr(ren, "/rn_dir/rn_b.bin");
    FCHECK_EQ(Transact(fx, Cmd::FsRename, ren).Status(), uint8_t(Status::Ok));
    FCHECK_EQ(Transact(fx, Cmd::FsStat, PathBody("/rn_a.bin")).Status(),
              uint8_t(Status::FsNoFile));
    FCHECK_EQ(Transact(fx, Cmd::FsStat,
                       PathBody("/rn_dir/rn_b.bin")).Status(),
              uint8_t(Status::Ok));

    /* Existing target refuses. */
    FCHECK(Upload(fx, "/rn_c.bin", Pattern(10u)));
    ren.clear();
    PutStr(ren, "/rn_c.bin");
    PutStr(ren, "/rn_dir/rn_b.bin");
    FCHECK_EQ(Transact(fx, Cmd::FsRename, ren).Status(),
              uint8_t(Status::FsExists));

    Transact(fx, Cmd::FsDelete, PathBody("/rn_c.bin"));
    Transact(fx, Cmd::FsDelete, PathBody("/rn_dir/rn_b.bin"));
    Transact(fx, Cmd::FsDelete, PathBody("/rn_dir"));
}

} // namespace

/* ── Entry points ──────────────────────────────────────────────────── */

int RunFsTests(int& checks, int& failures)
{
    {
        FsFixture fx;
        TestInfoAndProbe(fx);
        TestPathValidation(fx);
        TestMkdirDelete(fx);
        TestUploadDownloadRoundtrip(fx);
        TestOverwriteSemantics(fx);
        TestStrictOrderAndBadCrc(fx);
        TestTransferTimeout(fx);
        TestListingPagination(fx);
        TestLocks(fx);
        TestCardYankAndRecovery(fx);
    }
    {
        FsFixture fx;
        TestPowerCutResume(fx);
    }

    /* No f_read/f_write may ever hand disk I/O an unaligned pointer —
     * hardware IDMA truncates those addresses and shifts the data (the
     * REC0001.WAV download corruption).  The odd-sized FS_READ chunks
     * above (505 bytes, like the web client's) exercise every phase. */
    FCHECK_EQ(ramdisk::UnalignedIo(), 0u);

    checks   += s_checks;
    failures += s_failures;
    return s_failures;
}

void EmitFsGolden(FILE* f)
{
    FsFixture fx;
    std::vector<std::pair<std::string, std::string>> log;

    auto hex = [](const std::vector<uint8_t>& v)
    {
        std::string s;
        s.reserve(v.size() * 2u);
        for (uint8_t b : v)
        {
            static const char* d = "0123456789abcdef";
            s.push_back(d[b >> 4]);
            s.push_back(d[b & 0xF]);
        }
        return s;
    };
    auto record = [&](Cmd cmd, const std::vector<uint8_t>& body)
    {
        std::vector<uint8_t> req, resp;
        Transact(fx, cmd, body, &req, &resp);
        log.push_back({"h2d", hex(req)});
        log.push_back({"d2h", hex(resp)});
    };

    const auto payload = Pattern(600u);

    record(Cmd::FsInfo, {});
    record(Cmd::FsMkdir, PathBody("/samples"));
    record(Cmd::FsOpenWrite, OpenWriteBody(0u, 600u, "/samples/kick.raw"));
    record(Cmd::FsWrite, WriteBody(0u, payload.data(), 508u));
    record(Cmd::FsWrite, WriteBody(508u, payload.data() + 508u, 92u));
    record(Cmd::FsCommit, U32Only(Crc32(payload.data(), payload.size())));
    record(Cmd::FsStat, PathBody("/samples/kick.raw"));
    record(Cmd::FsOpenRead, PathBody("/samples/kick.raw"));
    record(Cmd::FsRead, ReadBody(0u, 505u));
    record(Cmd::FsRead, ReadBody(505u, 505u));
    record(Cmd::FsClose, {});
    record(Cmd::FsList, ListBody(0u, "/samples"));
    record(Cmd::FsDelete, PathBody("/samples/kick.raw"));

    std::fprintf(f, "  \"fs_exchange\": [\n");
    for (size_t i = 0; i < log.size(); i++)
        std::fprintf(f, "    {\"dir\": \"%s\", \"hex\": \"%s\"}%s\n",
                     log[i].first.c_str(), log[i].second.c_str(),
                     i + 1 < log.size() ? "," : "");
    std::fprintf(f, "  ],\n");
}
