#include "alchemy/host_link/diagnostics.h"

#include <cstdio>
#include <cstring>
#include "daisy_seed.h"

namespace alchemy {
namespace hostlink {
namespace {
void Increment(uint32_t& value) { if (value != UINT32_MAX) ++value; }
bool TextFits(const char* s, size_t max, bool empty = false)
{
    if (!s || (!empty && !*s)) return false;
    for (size_t n = 0; n <= max; ++n) if (!s[n]) return true;
    return false;
}
void StatusByte(FrameWriter& w, Status status) { w.U8(static_cast<uint8_t>(status)); }
} // namespace

uint32_t Diagnostics::DefaultClock() { return daisy::System::GetNow(); }

#define ALCHEMY_LOG_METHOD(name, level) \
void Diagnostics::name(const char* format, ...) \
{ \
    va_list args; va_start(args, format); \
    LogV(LogLevel::level, format, args); va_end(args); \
}
ALCHEMY_LOG_METHOD(Debug, Debug)
ALCHEMY_LOG_METHOD(Info, Info)
ALCHEMY_LOG_METHOD(Warn, Warn)
ALCHEMY_LOG_METHOD(Error, Error)
ALCHEMY_LOG_METHOD(PrintLine, Info)
#undef ALCHEMY_LOG_METHOD

void Diagnostics::LogV(LogLevel level, const char* format, va_list args)
{
    if (level < minimum_) return;
    if (!format) { Increment(format_errors_); return; }
    // Begin a new diagnostic epoch before sequence numbers wrap. An old
    // cursor will get BAD_STATE and re-discover, just as after reboot.
    if (next_ == UINT32_MAX)
    {
        session_ = 0u;
        head_ = count_ = 0u;
        next_ = 1u;
    }
    char text[kMaxText + 1u];
    const int n = std::vsnprintf(text, sizeof text, format, args);
    if (n < 0) { Increment(format_errors_); return; }
    if (count_ == kCapacity)
    {
        head_ = (head_ + 1u) % kCapacity;
        --count_;
        Increment(dropped_);
    }
    Record& r = records_[(head_ + count_++) % kCapacity];
    r.sequence = next_++;
    r.time_ms = clock_();
    r.level = level;
    r.flags = static_cast<size_t>(n) > kMaxText ? 1u : 0u;
    if (r.flags) Increment(truncated_);
    r.length = static_cast<uint16_t>(n > kMaxText ? kMaxText : n);
    while (r.length && (text[r.length - 1u] == '\r' || text[r.length - 1u] == '\n'))
        --r.length;
    std::memcpy(r.text, text, r.length);
    r.text[r.length] = '\0';
}

bool Diagnostics::Watch(GaugeBase& gauge)
{
    for (uint8_t i = 0; i < gauge_count_; ++i)
        if (gauges_[i] == &gauge) return true;
    bool ok = !described_ && gauge_count_ < kMaxGauges
        && TextFits(gauge.Id(), 48u) && TextFits(gauge.Label(), 64u)
        && TextFits(gauge.UnitName(), 16u, true);
    for (uint8_t i = 0; ok && i < gauge_count_; ++i)
        if (std::strcmp(gauges_[i]->Id(), gauge.Id()) == 0) ok = false;
    if (!ok) { configuration_ok_ = false; return false; }
    gauges_[gauge_count_++] = &gauge;
    return true;
}

void Diagnostics::Handle(const ParsedFrame& f, FrameWriter& w, uint32_t now_ms)
{
    if (f.type == static_cast<uint8_t>(Cmd::DiagInfo)) { InfoResponse(f, w); return; }
    const uint16_t expected = f.type == static_cast<uint8_t>(Cmd::DiagRead) ? 10u
        : f.type == static_cast<uint8_t>(Cmd::DiagDescribe) ? 5u : 4u;
    if (f.len != expected) { StatusByte(w, Status::BadArgs); return; }
    if (!session_ || RdU32(f.body) != session_) { StatusByte(w, Status::BadState); return; }
    switch (static_cast<Cmd>(f.type))
    {
        case Cmd::DiagRead: ReadResponse(f, w); break;
        case Cmd::DiagDescribe: DescribeResponse(f, w); break;
        case Cmd::DiagValues: ValuesResponse(w, now_ms); break;
        default: StatusByte(w, Status::Unsupported); break;
    }
}

void Diagnostics::InfoResponse(const ParsedFrame& f, FrameWriter& w)
{
    if (f.len != 4u || !RdU32(f.body)) { StatusByte(w, Status::BadArgs); return; }
    // The first host supplies a random nonzero boot-session token. No RNG,
    // flash write, or clock assumption is needed on the device. Subsequent
    // discovery (including retries/reconnects) never changes the token.
    if (!session_) session_ = RdU32(f.body);
    described_ = true;
    StatusByte(w, Status::Ok);
    w.U8(1u);
    w.U8(3u | (configuration_ok_ ? 0u : 4u));
    w.U32(session_); w.U32(Oldest()); w.U32(next_);
    w.U32(dropped_); w.U32(truncated_); w.U32(format_errors_);
    w.U16(kCapacity); w.U16(kMaxText); w.U8(gauge_count_);
}

void Diagnostics::ReadResponse(const ParsedFrame& f, FrameWriter& w)
{
    uint32_t cursor = RdU32(f.body + 4u);
    const uint16_t max_records = RdU16(f.body + 8u);
    if (!max_records || cursor > next_) { StatusByte(w, Status::BadArgs); return; }
    if (!cursor) cursor = Oldest();
    const uint32_t lost = cursor < Oldest() ? Oldest() - cursor : 0u;
    if (lost) cursor = Oldest();
    const uint16_t start = static_cast<uint16_t>(cursor - Oldest());
    uint16_t n = 0u;
    size_t bytes = 15u;
    while (start + n < count_ && n < max_records)
    {
        const Record& r = At(start + n);
        if (bytes + 12u + r.length > kMaxBody) break;
        bytes += 12u + r.length;
        ++n;
    }
    StatusByte(w, Status::Ok);
    w.U32(session_); w.U32(cursor + n); w.U32(lost); w.U16(n);
    for (uint16_t i = 0u; i < n; ++i)
    {
        const Record& r = At(start + i);
        w.U32(r.sequence); w.U32(r.time_ms);
        w.U8(static_cast<uint8_t>(r.level)); w.U8(r.flags); w.U16(r.length);
        w.Bytes(reinterpret_cast<const uint8_t*>(r.text), r.length);
    }
}

void Diagnostics::DescribeResponse(const ParsedFrame& f, FrameWriter& w)
{
    const uint8_t start = f.body[4];
    if (start > gauge_count_) { StatusByte(w, Status::BadArgs); return; }
    uint8_t n = 0;
    size_t bytes = 7u;
    while (start + n < gauge_count_)
    {
        const GaugeBase& g = *gauges_[start + n];
        const size_t size = 5u + std::strlen(g.Id()) + std::strlen(g.Label()) + std::strlen(g.UnitName());
        if (bytes + size > kMaxBody) break;
        bytes += size;
        ++n;
    }
    StatusByte(w, Status::Ok); w.U32(session_); w.U8(start + n); w.U8(n);
    for (uint8_t i = start; i < start + n; ++i)
    {
        const GaugeBase& g = *gauges_[i];
        w.U8(i); w.U8(static_cast<uint8_t>(g.Type()));
        w.Str(g.Id()); w.Str(g.Label()); w.Str(g.UnitName());
    }
}

void Diagnostics::ValuesResponse(FrameWriter& w, uint32_t now_ms)
{
    StatusByte(w, Status::Ok); w.U32(session_); w.U32(now_ms); w.U8(gauge_count_);
    for (uint8_t i = 0u; i < gauge_count_; ++i)
    {
        const GaugeBase& g = *gauges_[i];
        w.U8(i); w.U8(static_cast<uint8_t>(g.Type()));
        w.U8(g.Valid() ? 1u : 0u); w.U32(g.Bits());
    }
}

} // namespace hostlink
} // namespace alchemy
