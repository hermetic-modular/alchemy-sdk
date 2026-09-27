#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "alchemy/host_link/diagnostics.h"

using namespace alchemy::hostlink;
static unsigned checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
static uint32_t Clock() { return 1234u; }
static constexpr uint32_t kSession = 0x12345678u;

static std::vector<uint8_t> Request(uint32_t session, uint32_t cursor = 0, uint16_t n = 32)
{
    std::vector<uint8_t> req(10u);
    WrU32(req.data(), session); WrU32(req.data() + 4, cursor); WrU16(req.data() + 8, n);
    return req;
}

static std::string Hex(const uint8_t* bytes, size_t size)
{
    const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < size; ++i) { s += digits[bytes[i] >> 4]; s += digits[bytes[i] & 15]; }
    return s;
}

static std::vector<uint8_t> Call(Diagnostics& diag, Cmd cmd, std::vector<uint8_t> req,
                               bool golden = false, uint16_t seq = 1)
{
    uint8_t decoded[kMaxDecoded], wire[kMaxWire];
    FrameWriter writer(decoded);
    writer.Begin(static_cast<uint8_t>(cmd), seq);
    writer.Bytes(req.data(), req.size());
    const size_t rn = writer.Encode(wire);
    const auto request_hex = Hex(wire, rn);
    ParsedFrame f{kProtoVersion, static_cast<uint8_t>(cmd), seq,
                  static_cast<uint16_t>(req.size()), req.data(), true};
    writer.Begin(f.type | kRespFlag, seq);
    diag.Handle(f, writer, 5678u);
    const size_t n = writer.Encode(wire);
    if (!golden) CHECK(n > 0 && !writer.Overflowed());
    if (golden)
        std::printf("{\"command\":%u,\"request\":\"%s\",\"response\":\"%s\"}",
                    f.type, request_hex.c_str(), Hex(wire, n).c_str());
    FrameParser parser;
    std::vector<uint8_t> body;
    for (size_t i = 0; i < n; ++i)
        if (parser.Push(wire[i], f))
        {
            if (!golden) CHECK(f.ok && f.seq == seq);
            body.assign(f.body, f.body + f.len);
        }
    return body;
}

static std::vector<uint8_t> Info(Diagnostics& d, uint32_t session = kSession)
{
    auto req = Request(session); req.resize(4);
    return Call(d, Cmd::DiagInfo, req);
}

static void TestLogs()
{
    Diagnostics d(Clock);
    d.Info("startup before first poll");
    auto info = Info(d);
    CHECK(info.size() == 32 && info[0] == 0 && info[1] == 1);
    CHECK(RdU32(info.data() + 3) == kSession);
    CHECK(RdU32(info.data() + 7) == 1 && RdU32(info.data() + 11) == 2);
    CHECK(Info(d, 42) == info); // reconnect/probe cannot change the session

    auto req = Request(kSession, 1);
    auto first = Call(d, Cmd::DiagRead, req);
    CHECK(RdU16(first.data() + 13) == 1 && RdU32(first.data() + 5) == 2);
    CHECK(RdU32(first.data() + 19) == 1234u);
    CHECK(Call(d, Cmd::DiagRead, req) == first); // lost-response retry
    CHECK(Call(d, Cmd::DiagRead, Request(kSession, 2)).size() == 15);
    CHECK(Call(d, Cmd::DiagRead, Request(55, 1))[0] == uint8_t(Status::BadState));
    CHECK(Call(d, Cmd::DiagRead, Request(kSession, 999))[0] == uint8_t(Status::BadArgs));
    CHECK(Call(d, Cmd::DiagRead, Request(kSession, 1, 0))[0] == uint8_t(Status::BadArgs));
    CHECK(Call(d, Cmd::DiagRead, {})[0] == uint8_t(Status::BadArgs));
    CHECK(Call(d, Cmd::DiagInfo, std::vector<uint8_t>(4))[0] == uint8_t(Status::BadArgs));

    d.MinimumLevel(LogLevel::Warn);
    d.Info("filtered");
    d.Warn("warning %d\r\n", 7);
    auto warn = Call(d, Cmd::DiagRead, Request(kSession, 2));
    CHECK(warn[23] == uint8_t(LogLevel::Warn));
    CHECK(std::string(warn.begin() + 27, warn.end()) == "warning 7");
    d.MinimumLevel(LogLevel::Debug);
    const std::string long_message(Diagnostics::kMaxText + 20u, 'x');
    d.Error("%s", long_message.c_str());
    auto truncated = Call(d, Cmd::DiagRead, Request(kSession, 3));
    CHECK(truncated[24] == 1 && RdU16(truncated.data() + 25) == Diagnostics::kMaxText);
    CHECK(RdU32(Info(d).data() + 19) == 1);

    for (unsigned i = 0; i < Diagnostics::kCapacity + 5u; ++i) d.Info("%s", long_message.c_str());
    info = Info(d);
    const uint32_t oldest = RdU32(info.data() + 7), next = RdU32(info.data() + 11);
    CHECK(next - oldest == Diagnostics::kCapacity);
    CHECK(RdU32(info.data() + 15) == oldest - 1u);
    auto batch = Call(d, Cmd::DiagRead, Request(kSession, 1));
    CHECK(RdU32(batch.data() + 9) == oldest - 1u);
    CHECK(batch.size() <= kMaxBody);
    unsigned total = 0;
    uint32_t cursor = oldest;
    while (cursor < next)
    {
        batch = Call(d, Cmd::DiagRead, Request(kSession, cursor));
        const unsigned n = RdU16(batch.data() + 13);
        CHECK(n > 0);
        if (!n) break;
        CHECK(RdU32(batch.data() + 5) == cursor + n);
        cursor += n; total += n;
    }
    CHECK(total == Diagnostics::kCapacity);
    Diagnostics rebooted(Clock);
    CHECK(Call(rebooted, Cmd::DiagRead, Request(kSession, cursor))[0] == uint8_t(Status::BadState));
    CHECK(RdU32(Info(rebooted, 42).data() + 3) == 42u);
}

static void TestGauges()
{
    Diagnostics d(Clock);
    Gauge<float> cpu("audio.cpu", "Audio CPU"); cpu.Unit("%");
    Gauge<int32_t> integer("signed", "Signed");
    Gauge<uint32_t> counter("counter", "Counter");
    Gauge<bool> running("running", "Running");
    CHECK(d.Watch(cpu) && d.Watch(integer) && d.Watch(counter) && d.Watch(running));
    CHECK(d.Watch(cpu));
    Info(d);
    auto req = Request(kSession); req.resize(4);
    auto values = Call(d, Cmd::DiagValues, req);
    CHECK(values[9] == 4 && values[12] == 0); // unset value is not zero
    cpu.Set(23.5f); integer.Set(-7); counter.Set(UINT32_MAX); running.Set(true);
    values = Call(d, Cmd::DiagValues, req);
    CHECK(values.size() == 38 && values[12] == 1);
    CHECK(RdU32(values.data() + 13) == 0x41BC0000u);
    CHECK(RdU32(values.data() + 20) == 0xFFFFFFF9u);
    CHECK(RdU32(values.data() + 27) == UINT32_MAX);
    CHECK(RdU32(values.data() + 34) == 1);
    Gauge<bool> late("late", "Late");
    CHECK(!d.Watch(late) && !d.ConfigurationOk());
    CHECK(Info(d)[2] & 4u);

    Diagnostics duplicate;
    Gauge<float> a("same", "A"), b("same", "B");
    CHECK(duplicate.Watch(a) && !duplicate.Watch(b));
    Gauge<float> invalid("", "Invalid"); CHECK(!duplicate.Watch(invalid));

    Diagnostics many;
    std::vector<std::string> ids;
    std::vector<Gauge<uint32_t>*> gauges;
    ids.reserve(Diagnostics::kMaxGauges + 1u);
    for (unsigned i = 0; i <= Diagnostics::kMaxGauges; ++i)
    {
        ids.push_back(std::string(46, 'x') + std::to_string(i));
        gauges.push_back(new Gauge<uint32_t>(ids.back().c_str(), "A long enough label to force descriptor pagination at 512 bytes"));
        CHECK(many.Watch(*gauges.back()) == (i < Diagnostics::kMaxGauges));
    }
    Info(many);
    req.resize(5); req[4] = 0;
    unsigned seen = 0;
    while (req[4] < Diagnostics::kMaxGauges)
    {
        auto page = Call(many, Cmd::DiagDescribe, req);
        CHECK(page.size() <= kMaxBody && page[6] > 0);
        if (!page[6]) break;
        CHECK(page[5] == req[4] + page[6]);
        seen += page[6]; req[4] = page[5];
    }
    CHECK(seen == Diagnostics::kMaxGauges);
    req[4] = Diagnostics::kMaxGauges + 1u;
    CHECK(Call(many, Cmd::DiagDescribe, req)[0] == uint8_t(Status::BadArgs));
    for (auto* g : gauges) delete g;
}

static void Golden()
{
    Diagnostics d(Clock);
    Gauge<float> cpu("audio.cpu", "Audio CPU"); cpu.Unit("%"); cpu.Set(23.5f);
    Gauge<int32_t> signed_value("signed", "Signed"); signed_value.Set(-7);
    Gauge<uint32_t> counter("counter", "Counter"); counter.Set(UINT32_MAX);
    Gauge<bool> running("running", "Running"); running.Set(true);
    d.Watch(cpu); d.Watch(signed_value); d.Watch(counter); d.Watch(running);
    d.Info("Ready"); d.Warn("Preset %u used defaults", 3u);
    std::printf("{\"version\":1,\"exchange\":[");
    auto req = Request(kSession); req.resize(4);
    Call(d, Cmd::DiagInfo, req, true, 1); std::printf(",");
    Call(d, Cmd::DiagRead, Request(kSession, 1), true, 2); std::printf(",");
    req.push_back(0);
    Call(d, Cmd::DiagDescribe, req, true, 3); std::printf(",");
    req.resize(4);
    Call(d, Cmd::DiagValues, req, true, 4);
    std::printf("]}\n");
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--golden") == 0) { Golden(); return 0; }
    TestLogs(); TestGauges();
    std::printf("%u diagnostics checks, %u failures (max_body=%u)\n", checks, failures, kMaxBody);
    return failures ? 1 : 0;
}
