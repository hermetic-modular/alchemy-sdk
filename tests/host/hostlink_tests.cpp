/**
 * @file tests/host/hostlink_tests.cpp
 * @brief Host-side unit + integration tests for the HostLink protocol,
 *        the Presets host-access API, and the descriptor builder.
 *
 * Also the golden-vector generator for cross-language interop:
 *
 *   hostlink_tests --emit-golden <path.json>
 *
 * The emitted vectors (CRC, COBS, whole frames, a full write-slot
 * exchange, a real descriptor) are consumed by the website's TypeScript
 * test suite, pinning both implementations to identical bytes.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "alchemy/host_link/cobs.h"
#include "alchemy/host_link/crc32.h"
#include "alchemy/host_link/describe.h"
#include "alchemy/host_link/descriptor.h"
#include "alchemy/host_link/frame.h"
#include "alchemy/host_link/host_link.h"
#include "alchemy/host_link/wire.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/preset_name.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_knob.h"

using namespace alchemy;
using namespace alchemy::hostlink;

/* ── Tiny test harness ─────────────────────────────────────────────── */

static int g_checks   = 0;
static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        g_checks++;                                                        \
        const auto va = (a);                                               \
        const auto vb = (b);                                               \
        if (!(va == vb)) {                                                 \
            g_failures++;                                                  \
            std::printf("FAIL %s:%d  %s == %s  (%llu != %llu)\n",          \
                        __FILE__, __LINE__, #a, #b,                        \
                        (unsigned long long)va, (unsigned long long)vb);   \
        }                                                                  \
    } while (0)

/* ── Fakes ─────────────────────────────────────────────────────────── */

/** In-memory transport: tests inject host→device bytes into rx and read
 *  device→host bytes out of tx. */
struct LoopTransport : IHostTransport
{
    std::deque<uint8_t>  rx;
    std::vector<uint8_t> tx;

    size_t Read(uint8_t* dst, size_t max) override
    {
        size_t n = 0;
        while (!rx.empty() && n < max)
        {
            dst[n++] = rx.front();
            rx.pop_front();
        }
        return n;
    }
    size_t Write(const uint8_t* src, size_t n) override
    {
        tx.insert(tx.end(), src, src + n);
        return n;
    }
    size_t WriteSpace() const override { return 1u << 16; }

    void Inject(const uint8_t* p, size_t n) { rx.insert(rx.end(), p, p + n); }
};

/** RAM-backed flash: 16 slots × 2 sides × 5 × 4 KiB, erased to 0xFF. */
struct RamFlash
{
    static constexpr size_t kSize =
        size_t(16) * 2u * kPresetSectorsPerSide * kPresetSectorSize;

    std::vector<uint8_t> mem;

    RamFlash() : mem(kSize, 0xFF) {}

    uintptr_t Base() const { return reinterpret_cast<uintptr_t>(mem.data()); }

    bool InBounds(uintptr_t a, size_t len) const
    {
        return a >= Base() && a + len <= Base() + kSize;
    }

    static bool Erase(void* ctx, uintptr_t start, uintptr_t end)
    {
        auto* self = static_cast<RamFlash*>(ctx);
        if (!self->InBounds(start, end - start)) return false;
        std::memset(reinterpret_cast<void*>(start), 0xFF, end - start);
        return true;
    }
    static bool Write(void* ctx, uintptr_t addr, const uint8_t* buf, uint32_t len)
    {
        auto* self = static_cast<RamFlash*>(ctx);
        if (!self->InBounds(addr, len)) return false;
        std::memcpy(reinterpret_cast<void*>(addr), buf, len);
        return true;
    }
    static void Invalidate(void*, uintptr_t, uint32_t) {}

    FlashOps Ops() { return {&RamFlash::Erase, &RamFlash::Write,
                             &RamFlash::Invalidate, this}; }
};

/** Minimal managed component: N floats. */
template <uint8_t N>
struct TestComp : Serializable
{
    float    vals[N] = {};
    uint32_t hash;

    explicit TestComp(uint32_t h) : hash(h) {}

    size_t SerializedSize() const override { return N * sizeof(float); }
    void   Serialize(uint8_t* out) const override
    {
        std::memcpy(out, vals, sizeof(vals));
    }
    bool Deserialize(const uint8_t* in) override
    {
        std::memcpy(vals, in, sizeof(vals));
        return true;
    }
    uint32_t SchemaHash() const override { return hash; }
};

/* ── Device fixture ────────────────────────────────────────────────── */

static daisy::QSPIHandle g_dummy_qspi;   /* stubbed on host */

struct Fixture
{
    RamFlash      flash;
    LoopTransport t;
    Presets       presets{g_dummy_qspi};
    TestComp<4>   comp_a{0x11111111u};
    TestComp<4>   comp_b{0x22222222u};
    std::vector<uint8_t> staging  = std::vector<uint8_t>(kPresetBlobCapacity);
    std::vector<uint8_t> snapshot = std::vector<uint8_t>(kPresetBlobCapacity);
    HostLink      link;
    uint16_t      next_seq = 1u;
    uint32_t      now_ms   = 1000u;

    Fixture()
        : link(t, presets,
               HostLink::Info{"testmod", "Test Module", "1.2.3", "abc1234",
                              "1.0.0-beta", /*board=*/1u, /*boot_slot=*/0u},
               staging.data(), snapshot.data(), staging.size())
    {
        presets.Manage(comp_a);
        presets.Manage(comp_b);
        presets.Init(flash.Ops(), flash.Base());

        comp_a.vals[0] = 0.25f; comp_a.vals[1] = 0.5f;
        comp_a.vals[2] = 0.75f; comp_a.vals[3] = 1.0f;
        comp_b.vals[0] = 0.0f;  comp_b.vals[1] = 0.125f;
        comp_b.vals[2] = 0.375f; comp_b.vals[3] = 0.625f;
    }
};

struct Resp
{
    bool                 got = false;
    bool                 ok  = false;
    uint8_t              type = 0;
    uint16_t             seq  = 0;
    std::vector<uint8_t> body;
};

/** Encode + inject a request, run Poll, parse the (single) response. */
static Resp Transact(Fixture& fx, Cmd cmd,
                     const std::vector<uint8_t>& body,
                     std::vector<uint8_t>* wire_req = nullptr,
                     std::vector<uint8_t>* wire_resp = nullptr)
{
    uint8_t dec[kMaxDecoded], wire[kMaxWire];
    FrameWriter w(dec);
    w.Begin(static_cast<uint8_t>(cmd), fx.next_seq++);
    w.Bytes(body.data(), body.size());
    const size_t n = w.Encode(wire);
    if (wire_req) wire_req->assign(wire, wire + n);
    fx.t.Inject(wire, n);

    fx.t.tx.clear();
    fx.link.Poll(fx.now_ms);

    if (wire_resp) wire_resp->assign(fx.t.tx.begin(), fx.t.tx.end());

    Resp r;
    FrameParser p;
    for (uint8_t b : fx.t.tx)
    {
        ParsedFrame f;
        if (p.Push(b, f))
        {
            r.got  = true;
            r.ok   = f.ok;
            r.type = f.type;
            r.seq  = f.seq;
            r.body.assign(f.body, f.body + f.len);
        }
    }
    return r;
}

static std::vector<uint8_t> U32Body(uint32_t a)
{
    std::vector<uint8_t> b(4);
    WrU32(b.data(), a);
    return b;
}

/* ── Wire primitive tests ──────────────────────────────────────────── */

static void TestCrc32()
{
    const char* s = "123456789";
    CHECK_EQ(Crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
    CHECK_EQ(Crc32(nullptr, 0), 0u);

    /* Incremental == one-shot. */
    const uint8_t data[7] = {1, 2, 3, 4, 5, 250, 0};
    const uint32_t whole = Crc32(data, 7);
    uint32_t inc = Crc32(data, 3);
    inc = Crc32(data + 3, 4, inc);
    CHECK_EQ(whole, inc);
}

static void CobsRoundtrip(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> enc(CobsMaxEncoded(in.size()) + 1);
    const size_t en = CobsEncode(in.data(), in.size(), enc.data());
    for (size_t i = 0; i < en; i++) CHECK(enc[i] != 0u);

    std::vector<uint8_t> dec(en + 1);
    const size_t dn = CobsDecode(enc.data(), en, dec.data());
    CHECK_EQ(dn, in.size());
    CHECK(std::memcmp(dec.data(), in.data(), in.size()) == 0);
}

static void TestCobs()
{
    /* Known encodings. */
    {
        const uint8_t in[1] = {0x00};
        uint8_t out[8];
        const size_t n = CobsEncode(in, 1, out);
        CHECK_EQ(n, 2u);
        CHECK(out[0] == 0x01 && out[1] == 0x01);
    }
    {
        const uint8_t in[4] = {0x11, 0x22, 0x00, 0x33};
        uint8_t out[8];
        const size_t n = CobsEncode(in, 4, out);
        CHECK_EQ(n, 5u);
        CHECK(out[0] == 0x03 && out[1] == 0x11 && out[2] == 0x22
              && out[3] == 0x02 && out[4] == 0x33);
    }

    CobsRoundtrip({});
    CobsRoundtrip({0x01});
    CobsRoundtrip({0x00, 0x00, 0x00});
    std::vector<uint8_t> long_run;
    for (int i = 0; i < 600; i++) long_run.push_back(uint8_t(1 + (i % 255)));
    CobsRoundtrip(long_run);
    std::vector<uint8_t> exact254;
    for (int i = 0; i < 254; i++) exact254.push_back(0x42);
    CobsRoundtrip(exact254);

    /* Malformed: embedded zero, truncated group. */
    {
        uint8_t bad[3] = {0x03, 0x00, 0x11};
        uint8_t out[8];
        CHECK_EQ(CobsDecode(bad, 3, out), 0u);
        uint8_t trunc[2] = {0x05, 0x11};
        CHECK_EQ(CobsDecode(trunc, 2, out), 0u);
    }
}

static void TestFrameRoundtrip()
{
    uint8_t dec[kMaxDecoded], wire[kMaxWire];
    FrameWriter w(dec);
    w.Begin(0x11, 0xBEEF);
    w.U8(7);
    w.U16(0x1234);
    w.U32(0xDEADBEEF);
    w.Str("echoa");
    const size_t n = w.Encode(wire);
    CHECK(n > 0);

    FrameParser p;
    ParsedFrame f{};
    bool got = false;
    for (size_t i = 0; i < n; i++)
        if (p.Push(wire[i], f)) got = true;
    CHECK(got);
    CHECK(f.ok);
    CHECK_EQ(f.type, 0x11u);
    CHECK_EQ(f.seq, 0xBEEFu);
    CHECK_EQ(f.len, 1u + 2u + 4u + 1u + 5u);
    CHECK_EQ(f.body[0], 7u);
    CHECK_EQ(RdU16(f.body + 1), 0x1234u);
    CHECK_EQ(RdU32(f.body + 3), 0xDEADBEEFu);
    CHECK_EQ(f.body[7], 5u);
    CHECK(std::memcmp(f.body + 8, "echoa", 5) == 0);

    /* Corruption → ok=false (still delivered so device can NACK). */
    wire[3] ^= 0x40u;
    FrameParser p2;
    got = false;
    for (size_t i = 0; i < n; i++)
        if (p2.Push(wire[i], f)) got = true;
    CHECK(got);
    CHECK(!f.ok);

    /* Resync: garbage (with delimiter), then a valid frame. */
    FrameWriter w2(dec);
    w2.Begin(0x01, 1);
    const size_t n2 = w2.Encode(wire);
    FrameParser p3;
    const uint8_t junk[] = {0x99, 0x98, 0x00};
    got = false;
    for (uint8_t b : junk) p3.Push(b, f);
    for (size_t i = 0; i < n2; i++)
        if (p3.Push(wire[i], f)) got = true;
    CHECK(got && f.ok && f.type == 0x01);
}

/* ── Protocol integration tests ────────────────────────────────────── */

static void TestHello(Fixture& fx)
{
    const Resp r = Transact(fx, Cmd::Hello, {});
    CHECK(r.got && r.ok);
    CHECK_EQ(r.type, uint8_t(0x81));
    CHECK_EQ(r.body[0], uint8_t(Status::Ok));
    CHECK_EQ(r.body[1], kProtoVersion);
    CHECK_EQ(r.body[2], 1u);                       /* board v1 */
    CHECK_EQ(r.body[3], Presets::kNumSlots);
    CHECK_EQ(r.body[4], 0u);                       /* boot slot */
    /* uid = 12 zero bytes at 5..16 */
    CHECK_EQ(RdU32(r.body.data() + 17), fx.presets.LiveSchemaHash());
    CHECK_EQ(RdU32(r.body.data() + 21), uint32_t(kPresetBlobCapacity));
    /* descriptor_len/crc zero (not set in this fixture) */
    CHECK_EQ(RdU32(r.body.data() + 25), 0u);
    CHECK_EQ(RdU16(r.body.data() + 33), kMaxBody);
    CHECK_EQ(RdU16(r.body.data() + 35), uint16_t(32));  /* live size */
    /* strings */
    const uint8_t* p = r.body.data() + 37;
    CHECK_EQ(*p, 7u);
    CHECK(std::memcmp(p + 1, "testmod", 7) == 0);
}

static void TestDescriptorChunks(Fixture& fx)
{
    static const char kDesc[] =
        "{\"dv\":1,\"module\":{\"id\":\"testmod\"},\"padding\":"
        "\"0123456789012345678901234567890123456789\"}";
    const uint32_t dlen = uint32_t(sizeof(kDesc) - 1);
    fx.link.SetDescriptor(kDesc, dlen);

    /* HELLO advertises it. */
    Resp h = Transact(fx, Cmd::Hello, {});
    CHECK_EQ(RdU32(h.body.data() + 25), dlen);
    const uint32_t adv_crc = RdU32(h.body.data() + 29);
    CHECK_EQ(adv_crc, Crc32(reinterpret_cast<const uint8_t*>(kDesc), dlen));

    /* Fetch in 40-byte chunks and reassemble. */
    std::string got;
    uint32_t off = 0;
    while (true)
    {
        std::vector<uint8_t> body(6);
        WrU32(body.data(), off);
        WrU16(body.data() + 4, 40u);
        const Resp r = Transact(fx, Cmd::GetDescriptor, body);
        CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
        CHECK_EQ(RdU32(r.body.data() + 1), off);
        const uint16_t n = RdU16(r.body.data() + 5);
        if (n == 0) break;
        got.append(reinterpret_cast<const char*>(r.body.data() + 7), n);
        off += n;
    }
    CHECK_EQ(got.size(), size_t(dlen));
    CHECK(got == kDesc);

    fx.link.SetDescriptor(nullptr, 0);
}

static std::vector<uint8_t> LiveBytes(Fixture& fx)
{
    std::vector<uint8_t> out(fx.presets.LiveSize());
    fx.presets.SerializeLive(out.data(), out.size());
    return out;
}

static void TestGetLive(Fixture& fx)
{
    std::vector<uint8_t> body(6);
    WrU32(body.data(), 0u);
    WrU16(body.data() + 4, 500u);
    const Resp r = Transact(fx, Cmd::GetLive, body);
    CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
    CHECK_EQ(r.body[1], kTargetLive);
    CHECK_EQ(RdU32(r.body.data() + 2), 0u);
    const uint16_t n     = RdU16(r.body.data() + 6);
    const uint16_t total = RdU16(r.body.data() + 8);
    CHECK_EQ(total, uint16_t(32));
    CHECK_EQ(n, total);
    CHECK_EQ(RdU32(r.body.data() + 10), fx.presets.LiveSchemaHash());

    const auto live = LiveBytes(fx);
    CHECK(std::memcmp(r.body.data() + 14, live.data(), live.size()) == 0);
}

/** Full BLOB_BEGIN/DATA/COMMIT to @p target with @p payload. */
static Status BlobWrite(Fixture& fx, uint8_t target,
                        const std::vector<uint8_t>& payload,
                        uint32_t schema, size_t chunk = 400)
{
    std::vector<uint8_t> begin(9);
    begin[0] = target;
    WrU32(begin.data() + 1, uint32_t(payload.size()));
    WrU32(begin.data() + 5, schema);
    Resp r = Transact(fx, Cmd::BlobBegin, begin);
    if (!r.ok || r.body[0] != uint8_t(Status::Ok))
        return Status(r.body.empty() ? 255 : r.body[0]);

    size_t off = 0;
    while (off < payload.size())
    {
        const size_t n = std::min(chunk, payload.size() - off);
        std::vector<uint8_t> data(4 + n);
        WrU32(data.data(), uint32_t(off));
        std::memcpy(data.data() + 4, payload.data() + off, n);
        r = Transact(fx, Cmd::BlobData, data);
        if (!r.ok || r.body[0] != uint8_t(Status::Ok))
            return Status(r.body.empty() ? 255 : r.body[0]);
        off += n;
    }

    r = Transact(fx, Cmd::BlobCommit,
                 U32Body(Crc32(payload.data(), payload.size())));
    return Status(r.body.empty() ? 255 : r.body[0]);
}

static void TestSetLive(Fixture& fx)
{
    auto blob = LiveBytes(fx);
    /* Flip comp_a.vals[0] to 0.9f in the byte stream. */
    const float nv = 0.9f;
    std::memcpy(blob.data(), &nv, 4);

    CHECK_EQ(uint8_t(BlobWrite(fx, kTargetLive, blob,
                               fx.presets.LiveSchemaHash())),
             uint8_t(Status::Ok));
    CHECK(fx.comp_a.vals[0] == 0.9f);
}

static void TestSlotWriteReadList(Fixture& fx)
{
    const auto blob = LiveBytes(fx);

    CHECK_EQ(uint8_t(BlobWrite(fx, 3u, blob, fx.presets.LiveSchemaHash())),
             uint8_t(Status::Ok));

    /* LIST: slot 3 valid + schema match. */
    Resp r = Transact(fx, Cmd::ListSlots, {});
    CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
    CHECK_EQ(r.body[1], Presets::kNumSlots);
    const uint8_t* rec3 = r.body.data() + 2 + 3 * 11;
    CHECK_EQ(rec3[0], 0x03u);
    CHECK_EQ(RdU16(rec3 + 5), uint16_t(blob.size()));
    CHECK_EQ(RdU32(rec3 + 7), fx.presets.LiveSchemaHash());
    /* slot 0 untouched */
    CHECK_EQ(r.body[2], 0x00u);

    /* READ_SLOT round-trips the exact bytes. */
    std::vector<uint8_t> body(7);
    body[0] = 3u;
    WrU32(body.data() + 1, 0u);
    WrU16(body.data() + 5, 500u);
    r = Transact(fx, Cmd::ReadSlot, body);
    CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
    CHECK_EQ(r.body[1], 3u);
    CHECK_EQ(RdU16(r.body.data() + 6), uint16_t(blob.size()));   /* n     */
    CHECK_EQ(RdU16(r.body.data() + 8), uint16_t(blob.size()));   /* total */
    CHECK(std::memcmp(r.body.data() + 14, blob.data(), blob.size()) == 0);

    /* LOAD_FROM_SLOT applies it. */
    fx.comp_a.vals[0] = -1.0f;
    r = Transact(fx, Cmd::LoadFromSlot, {3u});
    CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
    CHECK(fx.comp_a.vals[0] != -1.0f);

    /* SAVE_TO_SLOT captures current state. */
    fx.comp_b.vals[3] = 0.111f;
    r = Transact(fx, Cmd::SaveToSlot, {5u});
    CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
    CHECK(fx.presets.HasValid(5));
    const Presets::SlotInfo i5 = fx.presets.InfoFor(5);
    CHECK(i5.valid && i5.schema_match);

    /* ERASE_SLOT invalidates. */
    r = Transact(fx, Cmd::EraseSlot, {3u});
    CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
    CHECK(!fx.presets.HasValid(3));
    CHECK(fx.presets.HasValid(5));
}

static void TestErrorPaths(Fixture& fx)
{
    const auto blob = LiveBytes(fx);

    /* Schema mismatch at BEGIN. */
    CHECK_EQ(uint8_t(BlobWrite(fx, 2u, blob, 0xBAD5EEDu)),
             uint8_t(Status::SchemaMismatch));

    /* Wrong total (schema right, size wrong). */
    auto short_blob = blob;
    short_blob.pop_back();
    CHECK_EQ(uint8_t(BlobWrite(fx, 2u, short_blob,
                               fx.presets.LiveSchemaHash())),
             uint8_t(Status::SchemaMismatch));

    /* Bad slot. */
    std::vector<uint8_t> begin(9);
    begin[0] = 42u;
    WrU32(begin.data() + 1, uint32_t(blob.size()));
    WrU32(begin.data() + 5, fx.presets.LiveSchemaHash());
    Resp r = Transact(fx, Cmd::BlobBegin, begin);
    CHECK_EQ(r.body[0], uint8_t(Status::BadSlot));

    /* DATA with no transaction. */
    std::vector<uint8_t> data(8, 0);
    r = Transact(fx, Cmd::BlobData, data);
    CHECK_EQ(r.body[0], uint8_t(Status::BadState));

    /* Out-of-order DATA aborts. */
    begin[0] = 2u;
    r = Transact(fx, Cmd::BlobBegin, begin);
    CHECK_EQ(r.body[0], uint8_t(Status::Ok));
    std::vector<uint8_t> ooo(4 + 4);
    WrU32(ooo.data(), 99u);
    r = Transact(fx, Cmd::BlobData, ooo);
    CHECK_EQ(r.body[0], uint8_t(Status::BadArgs));
    r = Transact(fx, Cmd::BlobCommit, U32Body(0u));
    CHECK_EQ(r.body[0], uint8_t(Status::BadState));

    /* Bad payload CRC at COMMIT. */
    r = Transact(fx, Cmd::BlobBegin, begin);
    CHECK_EQ(r.body[0], uint8_t(Status::Ok));
    std::vector<uint8_t> all(4 + blob.size());
    WrU32(all.data(), 0u);
    std::memcpy(all.data() + 4, blob.data(), blob.size());
    r = Transact(fx, Cmd::BlobData, all);
    CHECK_EQ(r.body[0], uint8_t(Status::Ok));
    r = Transact(fx, Cmd::BlobCommit, U32Body(0x12345678u));
    CHECK_EQ(r.body[0], uint8_t(Status::BadCrc));
    CHECK(!fx.presets.HasValid(2));

    /* Transaction timeout. */
    r = Transact(fx, Cmd::BlobBegin, begin);
    CHECK_EQ(r.body[0], uint8_t(Status::Ok));
    fx.now_ms += 6000u;
    r = Transact(fx, Cmd::BlobData, all);
    CHECK_EQ(r.body[0], uint8_t(Status::BadState));

    /* Unknown command. */
    r = Transact(fx, Cmd(0x6E), {});
    CHECK_EQ(r.body[0], uint8_t(Status::Unsupported));

    /* Load from empty slot. */
    r = Transact(fx, Cmd::LoadFromSlot, {9u});
    CHECK_EQ(r.body[0], uint8_t(Status::BadSlot));

    /* Well-formed COBS chunk with a wrong CRC → ERR frame (FrameError).
     * Hand-built so the corruption is guaranteed decodable. */
    const uint8_t bad_dec[10] = {kProtoVersion, uint8_t(Cmd::Hello),
                                 0x34, 0x12,      /* seq 0x1234   */
                                 0x00, 0x00,      /* len 0        */
                                 0xDE, 0xAD, 0xBE, 0xEF};  /* bogus CRC */
    uint8_t bad_wire[kMaxWire];
    size_t  bn = CobsEncode(bad_dec, sizeof bad_dec, bad_wire);
    bad_wire[bn++] = 0x00u;
    fx.t.Inject(bad_wire, bn);
    fx.t.tx.clear();
    fx.link.Poll(fx.now_ms);
    FrameParser p;
    ParsedFrame f{};
    bool got = false;
    for (uint8_t b : fx.t.tx)
        if (p.Push(b, f)) got = true;
    CHECK(got && f.ok);
    CHECK_EQ(f.type, kErrType);
    CHECK_EQ(f.seq, 0x1234u);
    CHECK_EQ(f.body[0], uint8_t(Status::FrameError));
}

static uint8_t  g_reboot_mode  = 0xEE;
static int      g_reboot_calls = 0;
static void RebootSpy(uint8_t mode, void*) { g_reboot_mode = mode; g_reboot_calls++; }

static void TestReboot(Fixture& fx)
{
    fx.link.SetRebootHandler(&RebootSpy, nullptr);
    const Resp r = Transact(fx, Cmd::Reboot, {kRebootBootloader});
    CHECK(r.ok && r.body[0] == uint8_t(Status::Ok));
    CHECK_EQ(g_reboot_calls, 0);          /* deferred */
    fx.link.Poll(fx.now_ms + 50u);
    CHECK_EQ(g_reboot_calls, 0);          /* not yet  */
    fx.link.Poll(fx.now_ms + 150u);
    CHECK_EQ(g_reboot_calls, 1);
    CHECK_EQ(g_reboot_mode, kRebootBootloader);
}

static void TestTornWrite()
{
    Fixture fx;
    const auto blob1 = LiveBytes(fx);
    CHECK(fx.presets.WriteSlotRaw(0, fx.presets.LiveSchemaHash(),
                                  blob1.data(), uint16_t(blob1.size())));

    fx.comp_a.vals[0] = 0.42f;
    const auto blob2 = LiveBytes(fx);
    CHECK(fx.presets.WriteSlotRaw(0, fx.presets.LiveSchemaHash(),
                                  blob2.data(), uint16_t(blob2.size())));

    /* Newest side wins. */
    CHECK(fx.presets.Load(0));
    CHECK(fx.comp_a.vals[0] == 0.42f);

    /* Corrupt the newest side's payload (side 1 = second write target:
     * first write took the invalid side 0, second took side 1). */
    const size_t side_size = size_t(kPresetSectorsPerSide) * kPresetSectorSize;
    uint8_t* side1 = fx.flash.mem.data() + side_size;   /* slot 0, side 1 */
    side1[20 + 8] ^= 0xFF;                              /* payload byte   */

    /* Rescan (fresh boot): CRC kills side 1, side 0 (blob1) survives. */
    fx.presets.Init(fx.flash.Ops(), fx.flash.Base());
    CHECK(fx.presets.HasValid(0));
    CHECK(fx.presets.Load(0));
    CHECK(fx.comp_a.vals[0] == 0.25f);
}

/* ── Descriptor builder against real surfaces ──────────────────────── */

struct SurfaceFixture
{
    RamFlash    flash;
    AlchemyLab  hw;                      /* stub-backed on host */
    Pager       pager{hw.buttons[0], 3, 6};
    Settings    settings{hw, &pager};
    PresetName  name;
    Presets     presets{g_dummy_qspi};
    TestComp<2> motion{0x33333333u};     /* stands in for ParamLock */

    SelectorHandle mode;
    KnobHandle     knob;
    BrightnessHandle bright;
    BipolarHandle  bip;

    SurfaceFixture()
    {
        mode   = settings.Page(0).Pot(0).Selector(4).Default(2);
        knob   = settings.Page(0).Pot(1).Knob().Default(0.5f);
        bright = settings.Page(1).Pot(0).Brightness()
                         .Range(0.05f, 0.35f).Default(0.2f);
        bip    = settings.Page(3).Pot(2).Bipolar().Default(1.0f);

        presets.Manage(pager);
        presets.Manage(motion);
        presets.Manage(settings);
        presets.Manage(name);            /* last: offsets stay stable */
        presets.Init(flash.Ops(), flash.Base());

        static const float kPhys[8] = {};
        pager.SetStored(0, 0, 0.61f, kPhys);
        name.Set("Tape wash");
    }
};

static const char* const kAltIds[18] = {
    "a0", "a1", "a2", "a3", "a4", "a5",
    "b0", "b1", "b2", "b3", "b4", "b5",
    "e0", "e1", "e2", "e3", "e4", "e5",
};

static std::string BuildTestDescriptor(SurfaceFixture& sf, bool& ok)
{
    static char buf[8192];
    DescriptorBuilder db(buf, sizeof buf, sf.presets);
    ok = true;
    ok &= db.Begin({"testmod", "Test Module", "1.2.3", "abc1234",
                    "1.0.0-beta", "v1"});
    static const char* names[3] = {"Time", "Space", "Engine"};
    static const char* colors[3] = {"#86efac", "#fca5a5", "#67e8f9"};
    ok &= db.BeginPager("perf", sf.pager, names, colors);
    for (uint8_t pg = 0; pg < 3; pg++)
        for (uint8_t p = 0; p < 6; p++)
        {
            char fid[8], nm[16];
            std::snprintf(fid, sizeof fid, "p%u.%u", pg, p);
            std::snprintf(nm, sizeof nm, "%s %u", names[pg], p + 1);
            ok &= db.PagerField(pg, p, fid, nm,
                                (pg == 0 && p == 0)
                                    ? "{\"kind\":\"expTime\",\"minS\":0.001,"
                                      "\"maxFrom\":\"g.timeMax\",\"maxMap\":[2,4,8,12,16],"
                                      "\"curveFrom\":\"g.timeCurve\"}"
                                    : nullptr);
        }
    ok &= db.PagerAltMap("s.layout", kAltIds, 18);
    ok &= db.EndPager();
    ok &= db.Opaque("motion", sf.motion, "Motion recording");
    /* One nullptr entry — emitted as "" so hosts fall back per-page. */
    static const char* set_names[4] = {"Routing", "Global", nullptr, "Atten"};
    ok &= db.BeginSettings("settings", sf.settings, set_names, nullptr);
    ok &= db.SettingsField(0, 0, "s.mode", "Delay Mode",
                           "{\"kind\":\"enum\",\"labels\":[\"Series\","
                           "\"Parallel\",\"Isolated\",\"Spectral\"]}");
    ok &= db.SettingsField(0, 1, "s.knob", "Crossover", nullptr);
    ok &= db.SettingsField(1, 0, "s.bright", "Brightness", nullptr);
    ok &= db.SettingsField(3, 2, "s.atten", "Atten 3", nullptr);
    ok &= db.EndSettings();
    ok &= db.Name("name", sf.name);
    const uint32_t len = db.Finish();
    ok &= (len > 0);
    return ok ? std::string(buf, len) : std::string();
}

static void TestDescriptorBuilder()
{
    SurfaceFixture sf;
    bool ok = false;
    const std::string json = BuildTestDescriptor(sf, ok);
    CHECK(ok);
    CHECK(!json.empty());

    /* Spot checks: structural + drift-sensitive values. */
    CHECK(json.find("\"dv\":1") != std::string::npos);
    CHECK(json.find("\"id\":\"testmod\"") != std::string::npos);
    /* Pager: 3×6×4 bytes. */
    CHECK(json.find("\"kind\":\"pager\",\"hash\":") != std::string::npos);
    CHECK(json.find("\"pages\":3,\"pots\":6") != std::string::npos);
    /* Page tab metadata. */
    CHECK(json.find("\"pageNames\":[\"Time\",\"Space\",\"Engine\"]")
          != std::string::npos);
    CHECK(json.find("\"pageColors\":[\"#86efac\",\"#fca5a5\",\"#67e8f9\"]")
          != std::string::npos);
    CHECK(json.find("\"pageNames\":[\"Routing\",\"Global\",\"\",\"Atten\"]")
          != std::string::npos);
    /* Captured default. */
    CHECK(json.find("\"def\":0.61") != std::string::npos);
    /* Settings offsets: p0(sel 1B @0, knob 4B @1) p1(bright 4B @5)
     * p3(bipolar 4B @9); component size 13. */
    CHECK(json.find("\"id\":\"s.knob\",\"name\":\"Crossover\",\"off\":1")
          != std::string::npos);
    CHECK(json.find("\"id\":\"s.bright\",\"name\":\"Brightness\",\"off\":5")
          != std::string::npos);
    CHECK(json.find("\"id\":\"s.atten\",\"name\":\"Atten 3\",\"off\":9")
          != std::string::npos);
    CHECK(json.find("\"zones\":4") != std::string::npos);
    CHECK(json.find("\"kind\":\"brightness\",\"lo\":0.05,\"hi\":0.35")
          != std::string::npos);
    CHECK(json.find("\"layoutFrom\":\"s.layout\"") != std::string::npos);

    /* Settings component size must equal its SerializedSize (13). */
    CHECK_EQ(sf.settings.SerializedSize(), size_t(13));
    /* Total size = 72 + 8 + 13 + 24 (name) = 117. */
    CHECK(json.find("\"size\":117") != std::string::npos);
    CHECK(json.find("\"id\":\"name\",\"kind\":\"name\"") != std::string::npos);

    /* Declaring a field on a non-persisted slot must fail the build. */
    {
        static char buf2[8192];
        DescriptorBuilder db(buf2, sizeof buf2, sf.presets);
        db.Begin({"x", "x", "1", "g", "s", "v1"});
        db.BeginPager("perf", sf.pager);
        db.EndPager();
        db.Opaque("motion", sf.motion, "m");
        db.BeginSettings("settings", sf.settings);
        CHECK(!db.SettingsField(2, 3, "bad", "Bad", nullptr));
        db.EndSettings();
        CHECK_EQ(db.Finish(), 0u);
    }

    /* Describing components out of Manage() order must fail. */
    {
        static char buf3[8192];
        DescriptorBuilder db(buf3, sizeof buf3, sf.presets);
        db.Begin({"x", "x", "1", "g", "s", "v1"});
        CHECK(!db.Opaque("motion", sf.motion, "m"));   /* pager is first */
        CHECK_EQ(db.Finish(), 0u);
    }

    /* A PagerAltMap count that disagrees with pages×pots must fail the
     * build rather than read the id array out of bounds. */
    {
        static char buf4[8192];
        DescriptorBuilder db(buf4, sizeof buf4, sf.presets);
        db.Begin({"x", "x", "1", "g", "s", "v1"});
        db.BeginPager("perf", sf.pager);
        CHECK(!db.PagerAltMap("s.layout", kAltIds, 17));  /* want 18 */
        db.EndPager();
        CHECK_EQ(db.Finish(), 0u);
    }
}

/* ── Descriptor auto-derivation (describe.h) ───────────────────────── */

static const DescriptorBuilder::ModuleInfo kAutoInfo =
    {"automod", "Auto Module", "1.0.0", "abc1234", "1.0.0-beta", "v2"};

/** Custom Serializable that self-describes two fields. */
template <size_t N>
struct FieldedComp : TestComp<N>
{
    explicit FieldedComp(uint32_t hash) : TestComp<N>(hash) {}

    bool Describe(ComponentWriter& w) const override
    {
        w.Label("Gate Trim");
        bool ok = w.Field("trim.rise", "Rise", 0, FieldType::F32, 0.25f,
                          "{\"kind\":\"exp\",\"lo\":0.001,\"hi\":1,"
                          "\"unit\":\"s\"}");
        ok &= w.Field("trim.mode", "Mode", 4, FieldType::Enum, 1.f,
                      "{\"kind\":\"enum\",\"labels\":[\"Slow\",\"Fast\"]}",
                      /*zones=*/2);
        return ok;
    }
};

static void TestAutoDescribe()
{
    SurfaceFixture sf;
    sf.settings.Page(0).Name("Setup");
    sf.settings.Page(1).Name("Lights");

    /* Declared pages: knobs carry names, transforms, and metadata the
     * derivation should pick up.  Page 2 is deliberately undeclared —
     * its fields must fall back to positional ids and generic names. */
    static const char* kEngines[4] = {"Digi", "BBD", "Tape", "Vinyl"};
    VirtualKnob cutoff = VirtualKnob(0, "Cutoff")
                             .Exp(20.f, 12000.f)
                             .Unit("Hz")
                             .Ident("flt.cutoff");
    VirtualKnob drive  = VirtualKnob(1, "Drive").Linear(0.f, 24.f).Unit("dB");
    VirtualKnob engine = VirtualKnob(2, "Engine").Selector(4).Labels(kEngines, 4);
    VirtualKnob plain  = VirtualKnob(3, "Plain");
    VirtualKnob rawd   = VirtualKnob(0, "Raw")
                             .Disp("{\"kind\":\"percent\",\"scale\":110}");

    Page p0 = Page(0).Name("Time").Color("#86efac");
    p0.Knobs(cutoff, drive, engine, plain);
    Page p1 = Page(1).Name("Space");
    p1.Knobs(rawd);

    const Page*   page_refs[2] = {&p0, &p1};
    const PageSet pages{page_refs, 2};

    static char buf[16384];
    const uint32_t len = RenderDescriptor(buf, sizeof buf, kAutoInfo,
                                          sf.presets, &pages, nullptr, 0);
    CHECK(len > 0u);
    const std::string json(buf, len);

    /* Page tabs from Page declarations (sparse: page 2 unnamed → ""). */
    CHECK(json.find("\"pageNames\":[\"Time\",\"Space\",\"\"]") != std::string::npos);
    CHECK(json.find("\"pageColors\":[\"#86efac\",\"\",\"\"]") != std::string::npos);

    /* Knob-derived fields: Ident override, derived exp/linear/snap disp. */
    CHECK(json.find("\"id\":\"flt.cutoff\",\"name\":\"Cutoff\"") != std::string::npos);
    CHECK(json.find("{\"kind\":\"exp\",\"lo\":20,\"hi\":12000,\"unit\":\"Hz\"}")
          != std::string::npos);
    CHECK(json.find("{\"kind\":\"linear\",\"lo\":0,\"hi\":24,\"unit\":\"dB\"}")
          != std::string::npos);
    CHECK(json.find("{\"kind\":\"snap\",\"labels\":[\"Digi\",\"BBD\",\"Tape\","
                    "\"Vinyl\"]}")
          != std::string::npos);
    /* Positional id for the un-Ident()ed knob; raw .Disp() passthrough. */
    CHECK(json.find("\"id\":\"p0.1\",\"name\":\"Drive\"") != std::string::npos);
    CHECK(json.find("\"id\":\"p1.0\",\"name\":\"Raw\"") != std::string::npos);
    CHECK(json.find("{\"kind\":\"percent\",\"scale\":110}") != std::string::npos);
    /* Undeclared position → generic name, percent readout. */
    CHECK(json.find("\"id\":\"p2.0\",\"name\":\"P3 K1\"") != std::string::npos);

    /* Un-described component → opaque fallback with a generic label. */
    CHECK(json.find("\"kind\":\"opaque\"") != std::string::npos);
    CHECK(json.find("\"name\":\"Data 1\"") != std::string::npos);

    /* Settings: page names from the builder, kind-derived field names
     * (each kind unique in the fixture → plain names), positional ids. */
    CHECK(json.find("\"pageNames\":[\"Setup\",\"Lights\",\"\",\"\"]")
          != std::string::npos);
    CHECK(json.find("\"id\":\"s0.0\",\"name\":\"Mode\"") != std::string::npos);
    CHECK(json.find("\"id\":\"s0.1\",\"name\":\"Knob\"") != std::string::npos);
    CHECK(json.find("\"id\":\"s1.0\",\"name\":\"Brightness\"") != std::string::npos);
    CHECK(json.find("\"id\":\"s3.2\",\"name\":\"Amount\"") != std::string::npos);

    /* Name component described last (fixture manages it last). */
    CHECK(json.find("\"id\":\"name\",\"kind\":\"name\"") != std::string::npos);

    /* Whole thing parses as the same layout the manual builder pins:
     * schema hash + total size are properties of the fixture, and the
     * auto path must agree with them. */
    CHECK(json.find("\"schemaHash\":") != std::string::npos);

    /* Without a PageSet the pager still describes — positional only. */
    {
        static char buf2[16384];
        const uint32_t len2 = RenderDescriptor(buf2, sizeof buf2, kAutoInfo,
                                               sf.presets, nullptr, nullptr, 0);
        CHECK(len2 > 0u);
        const std::string j2(buf2, len2);
        /* No pager tab metadata (the settings component keeps its own). */
        CHECK(j2.find("\"pageNames\":[\"Time") == std::string::npos);
        CHECK(j2.find("\"id\":\"p0.0\",\"name\":\"P1 K1\"") != std::string::npos);
    }
}

static void TestAutoDescribeGenericAndOverrides()
{
    RamFlash flash;
    Presets  presets{g_dummy_qspi};

    FieldedComp<8> trim{0x51515151u};
    TestComp<4>    blob{0x62626262u};

    presets.Manage(trim);
    presets.Manage(blob);
    presets.Init(flash.Ops(), flash.Base());

    /* Self-describing custom component + opaque fallback. */
    {
        static char buf[8192];
        const uint32_t len = RenderDescriptor(buf, sizeof buf, kAutoInfo,
                                              presets, nullptr, nullptr, 0);
        CHECK(len > 0u);
        const std::string json(buf, len);
        CHECK(json.find("\"kind\":\"fields\"") != std::string::npos);
        CHECK(json.find("\"name\":\"Gate Trim\"") != std::string::npos);
        CHECK(json.find("\"id\":\"trim.rise\",\"name\":\"Rise\",\"off\":0")
              != std::string::npos);
        CHECK(json.find("\"id\":\"trim.mode\",\"name\":\"Mode\",\"off\":4,"
                        "\"type\":\"enum\",\"zones\":2,\"def\":1")
              != std::string::npos);
        /* Generic fields carry no page/pot keys. */
        CHECK(json.find("\"off\":0,\"type\":\"f32\"") != std::string::npos);
        CHECK(json.find("\"kind\":\"opaque\"") != std::string::npos);
        CHECK(json.find("\"name\":\"Data 1\"") != std::string::npos);
    }

    /* An override beats the component's own Describe(). */
    {
        const DescribeOverride ovr[1] = {{
            &trim,
            [](ComponentWriter& w, void*) -> bool
            {
                w.Kind("opaque").Label("Motion loops");
                return true;
            },
            nullptr,
        }};
        static char buf[8192];
        const uint32_t len = RenderDescriptor(buf, sizeof buf, kAutoInfo,
                                              presets, nullptr, ovr, 1);
        CHECK(len > 0u);
        const std::string json(buf, len);
        CHECK(json.find("\"name\":\"Motion loops\"") != std::string::npos);
        CHECK(json.find("trim.rise") == std::string::npos);
    }

    /* A describer that writes out of bounds fails the whole build —
     * "no descriptor" beats a wrong one. */
    {
        const DescribeOverride ovr[1] = {{
            &blob,
            [](ComponentWriter& w, void*) -> bool
            {
                /* blob is 16 bytes (TestComp<4> = 4 floats). */
                return w.Field("x", "X", 16, FieldType::F32, 0.f);
            },
            nullptr,
        }};
        static char buf[8192];
        CHECK_EQ(RenderDescriptor(buf, sizeof buf, kAutoInfo, presets,
                                  nullptr, ovr, 1),
                 0u);
    }

    /* Metadata after the first field is a misuse, not a silent no-op. */
    {
        const DescribeOverride ovr[1] = {{
            &trim,
            [](ComponentWriter& w, void*) -> bool
            {
                bool ok = w.Field("a", "A", 0, FieldType::F32, 0.f);
                w.Grid(2, 6);   /* too late — must latch failure */
                return ok && w.Ok();
            },
            nullptr,
        }};
        static char buf[8192];
        CHECK_EQ(RenderDescriptor(buf, sizeof buf, kAutoInfo, presets,
                                  nullptr, ovr, 1),
                 0u);
    }
}

static void TestUseNames()
{
    RamFlash    flash;
    TestComp<4> a{0x0A0A0A0Au};
    TestComp<2> b{0x0B0B0B0Bu};

    /* UseNames() before any Manage(): later registrations slot in above
     * the name component. */
    Presets p1{g_dummy_qspi};
    p1.UseNames().Set("Init");
    p1.Manage(a);
    p1.Manage(b);
    p1.Init(flash.Ops(), flash.Base());
    CHECK_EQ(p1.NumManaged(), 3u);
    CHECK(p1.ManagedAt(0) == &a);
    CHECK(p1.ManagedAt(1) == &b);
    CHECK(p1.ManagedAt(2)->DescribeKind() == Serializable::DescKind::Name);

    /* UseNames() after Manage(): same final order, same schema hash. */
    Presets p2{g_dummy_qspi};
    p2.Manage(a);
    p2.Manage(b);
    p2.UseNames();
    CHECK_EQ(p2.NumManaged(), 3u);
    CHECK(p2.ManagedAt(2)->DescribeKind() == Serializable::DescKind::Name);
    CHECK_EQ(p1.LiveSchemaHash(), p2.LiveSchemaHash());
    CHECK_EQ(p1.LiveSize(), p2.LiveSize());
}

/* ── Golden vector emission ────────────────────────────────────────── */

static std::string Hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++)
    {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xF]);
    }
    return s;
}
static std::string Hex(const std::vector<uint8_t>& v)
{
    return Hex(v.data(), v.size());
}

static int EmitGolden(const char* path)
{
    FILE* f = std::fopen(path, "w");
    if (!f)
    {
        std::printf("cannot open %s\n", path);
        return 1;
    }

    std::fprintf(f, "{\n  \"crc32\": [\n");
    {
        const char*   s1 = "123456789";
        const uint8_t b2[] = {0x00, 0xFF, 0x10, 0x20};
        std::fprintf(f, "    {\"hex\": \"%s\", \"value\": %u},\n",
                     Hex(reinterpret_cast<const uint8_t*>(s1), 9).c_str(),
                     Crc32(reinterpret_cast<const uint8_t*>(s1), 9));
        std::fprintf(f, "    {\"hex\": \"%s\", \"value\": %u},\n",
                     Hex(b2, 4).c_str(), Crc32(b2, 4));
        std::fprintf(f, "    {\"hex\": \"\", \"value\": %u}\n", Crc32(nullptr, 0));
    }
    std::fprintf(f, "  ],\n  \"cobs\": [\n");
    {
        const std::vector<std::vector<uint8_t>> cases = {
            {},
            {0x00},
            {0x11, 0x22, 0x00, 0x33},
            {0x01, 0x02, 0x03},
            std::vector<uint8_t>(254, 0x41),
            std::vector<uint8_t>(300, 0x07),
        };
        for (size_t i = 0; i < cases.size(); i++)
        {
            std::vector<uint8_t> enc(CobsMaxEncoded(cases[i].size()) + 1);
            const size_t n = CobsEncode(cases[i].data(), cases[i].size(),
                                        enc.data());
            enc.resize(n);
            std::fprintf(f, "    {\"dec\": \"%s\", \"enc\": \"%s\"}%s\n",
                         Hex(cases[i]).c_str(), Hex(enc).c_str(),
                         i + 1 < cases.size() ? "," : "");
        }
    }

    /* Full stop-and-wait exchange against a fresh fixture, seq 1.. —
     * the TS client must reproduce the h2d bytes exactly and parse the
     * d2h bytes to the same values. */
    std::fprintf(f, "  ],\n  \"exchange\": [\n");
    {
        Fixture fx;
        std::vector<std::pair<std::string, std::string>> log; /* dir, hex */

        auto record = [&](Cmd cmd, const std::vector<uint8_t>& body)
        {
            std::vector<uint8_t> req, resp;
            Transact(fx, cmd, body, &req, &resp);
            log.push_back({"h2d", Hex(req)});
            log.push_back({"d2h", Hex(resp)});
        };

        record(Cmd::Hello, {});

        const auto blob = LiveBytes(fx);
        std::vector<uint8_t> begin(9);
        begin[0] = 3u;
        WrU32(begin.data() + 1, uint32_t(blob.size()));
        WrU32(begin.data() + 5, fx.presets.LiveSchemaHash());
        record(Cmd::BlobBegin, begin);

        std::vector<uint8_t> data(4 + blob.size());
        WrU32(data.data(), 0u);
        std::memcpy(data.data() + 4, blob.data(), blob.size());
        record(Cmd::BlobData, data);

        record(Cmd::BlobCommit, U32Body(Crc32(blob.data(), blob.size())));
        record(Cmd::ListSlots, {});

        std::vector<uint8_t> rd(7);
        rd[0] = 3u;
        WrU32(rd.data() + 1, 0u);
        WrU16(rd.data() + 5, 498u);
        record(Cmd::ReadSlot, rd);

        for (size_t i = 0; i < log.size(); i++)
            std::fprintf(f, "    {\"dir\": \"%s\", \"hex\": \"%s\"}%s\n",
                         log[i].first.c_str(), log[i].second.c_str(),
                         i + 1 < log.size() ? "," : "");
    }

    /* Real descriptor built from real surfaces (hex-encoded JSON). */
    std::fprintf(f, "  ],\n  \"descriptor\": {\n");
    {
        SurfaceFixture sf;
        bool ok = false;
        const std::string json = BuildTestDescriptor(sf, ok);
        std::fprintf(f, "    \"hex\": \"%s\",\n",
                     Hex(reinterpret_cast<const uint8_t*>(json.data()),
                         json.size()).c_str());
        std::fprintf(f, "    \"crc\": %u,\n",
                     Crc32(reinterpret_cast<const uint8_t*>(json.data()),
                           json.size()));
        std::fprintf(f, "    \"schemaHash\": %u,\n", sf.presets.LiveSchemaHash());
        std::fprintf(f, "    \"size\": %u\n",
                     unsigned(sf.presets.LiveSize()));
    }
    std::fprintf(f, "  }\n}\n");
    std::fclose(f);
    std::printf("golden vectors written to %s\n", path);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--emit-golden")
        return EmitGolden(argv[2]);

    TestCrc32();
    TestCobs();
    TestFrameRoundtrip();

    {
        Fixture fx;
        TestHello(fx);
        TestDescriptorChunks(fx);
        TestGetLive(fx);
        TestSetLive(fx);
        TestSlotWriteReadList(fx);
        TestErrorPaths(fx);
        TestReboot(fx);
    }
    TestTornWrite();
    TestDescriptorBuilder();
    TestAutoDescribe();
    TestAutoDescribeGenericAndOverrides();
    TestUseNames();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
