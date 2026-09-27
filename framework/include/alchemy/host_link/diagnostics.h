/**
 * Optional diagnostics over the existing HostLink connection.
 *
 *   static hostlink::Diagnostics debug;
 *   host.Extend(debug);
 *   debug.Info("Ready");
 *
 * Logs run on the control-loop thread, after hardware initialization.
 * They format into fixed SRAM without USB writes or waiting for a host.
 */
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "alchemy/host_link/extension.h"

// Define consistently for the SDK and its consumers, like MAX_BODY.
#ifndef ALCHEMY_DIAGNOSTIC_RECORDS
#define ALCHEMY_DIAGNOSTIC_RECORDS 32
#endif
#ifndef ALCHEMY_DIAGNOSTIC_TEXT
#define ALCHEMY_DIAGNOSTIC_TEXT 160
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ALCHEMY_LOG_FORMAT __attribute__((format(printf, 2, 3)))
#else
#define ALCHEMY_LOG_FORMAT
#endif

namespace alchemy {
namespace hostlink {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };
enum class GaugeType : uint8_t { Float = 1, Int = 2, UInt = 3, Bool = 4 };

class GaugeBase
{
  public:
    const char* Id() const { return id_; }
    const char* Label() const { return label_; }
    const char* UnitName() const { return unit_; }
    GaugeType Type() const { return type_; }
    bool Valid() const { return valid_.load(std::memory_order_acquire); }
    uint32_t Bits() const { return bits_.load(std::memory_order_relaxed); }

  protected:
    GaugeBase(const char* id, const char* label, GaugeType type)
        : id_(id), label_(label), type_(type) {}
    void Publish(uint32_t bits)
    {
        bits_.store(bits, std::memory_order_relaxed);
        valid_.store(true, std::memory_order_release);
    }
    const char* unit_ = "";

  private:
    const char* id_;
    const char* label_;
    GaugeType type_;
    std::atomic<uint32_t> bits_{0u};
    std::atomic<bool> valid_{false};
};

/** Latest value, independent of presets. Use float, int32_t, uint32_t, bool. */
template <typename T>
class Gauge : public GaugeBase
{
    static_assert(std::is_same<T, float>::value
                  || std::is_same<T, int32_t>::value
                  || std::is_same<T, uint32_t>::value
                  || std::is_same<T, bool>::value, "unsupported gauge type");
    static_assert(std::atomic<uint32_t>::is_always_lock_free
                  && std::atomic<bool>::is_always_lock_free,
                  "gauges require lock-free scalar atomics");
  public:
    Gauge(const char* id, const char* label)
        : GaugeBase(id, label, std::is_same<T, float>::value ? GaugeType::Float
                    : std::is_same<T, int32_t>::value ? GaugeType::Int
                    : std::is_same<T, uint32_t>::value ? GaugeType::UInt
                    : GaugeType::Bool) {}
    Gauge& Unit(const char* unit) { unit_ = unit; return *this; }
    void Set(T value)
    {
        uint32_t bits = 0u;
        if constexpr (std::is_same<T, bool>::value) bits = value ? 1u : 0u;
        else std::memcpy(&bits, &value, sizeof bits);
        Publish(bits);
    }
};

class Diagnostics : public IHostlinkExtension
{
  public:
    static constexpr uint16_t kCapacity = ALCHEMY_DIAGNOSTIC_RECORDS;
    static constexpr uint16_t kMaxText = ALCHEMY_DIAGNOSTIC_TEXT;
    static constexpr uint8_t kMaxGauges = 16u;
    static_assert(ALCHEMY_DIAGNOSTIC_RECORDS > 0
                  && ALCHEMY_DIAGNOSTIC_RECORDS <= 1024, "invalid log capacity");
    static_assert(ALCHEMY_DIAGNOSTIC_TEXT >= 16
                  && ALCHEMY_DIAGNOSTIC_TEXT <= 240, "invalid log text limit");

    using Clock = uint32_t (*)();
    explicit Diagnostics(Clock clock = &DefaultClock) : clock_(clock ? clock : &DefaultClock) {}
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    /** printf-style, bounded output. %f requires newlib float formatting;
     * prefer a Gauge<float> for numeric readouts without that linker cost. */
    void Debug(const char* format, ...) ALCHEMY_LOG_FORMAT;
    void Info(const char* format, ...) ALCHEMY_LOG_FORMAT;
    void Warn(const char* format, ...) ALCHEMY_LOG_FORMAT;
    void Error(const char* format, ...) ALCHEMY_LOG_FORMAT;
    void PrintLine(const char* format, ...) ALCHEMY_LOG_FORMAT; // Info alias

    /** Setup-only, idempotent for the same object; false on a duplicate
     * id, invalid metadata, capacity exhaustion, or late registration. */
    bool Watch(GaugeBase& gauge);
    bool ConfigurationOk() const { return configuration_ok_; }
    void MinimumLevel(LogLevel level) { minimum_ = level; }

    uint8_t FirstCmd() const override { return 0x60u; }
    uint8_t LastCmd() const override { return 0x63u; }
    void Handle(const ParsedFrame&, FrameWriter&, uint32_t now_ms) override;
    const char* DescriptorRootJson() const override
    { return "\"diagnostics\":{\"version\":1,\"logs\":true,\"values\":true}"; }

  private:
    struct Record
    {
        uint32_t sequence = 0u;
        uint32_t time_ms = 0u;
        uint16_t length = 0u;
        LogLevel level = LogLevel::Info;
        uint8_t flags = 0u;
        char text[kMaxText + 1u] = {};
    };
    static uint32_t DefaultClock();
    void LogV(LogLevel level, const char* format, va_list args);
    uint32_t Oldest() const { return next_ - count_; }
    const Record& At(uint16_t offset) const { return records_[(head_ + offset) % kCapacity]; }
    void InfoResponse(const ParsedFrame&, FrameWriter&);
    void ReadResponse(const ParsedFrame&, FrameWriter&);
    void DescribeResponse(const ParsedFrame&, FrameWriter&);
    void ValuesResponse(FrameWriter&, uint32_t now_ms);

    Clock clock_;
    Record records_[kCapacity];
    uint16_t head_ = 0u, count_ = 0u;
    uint32_t next_ = 1u, session_ = 0u;
    uint32_t dropped_ = 0u, truncated_ = 0u, format_errors_ = 0u;
    LogLevel minimum_ = LogLevel::Debug;
    GaugeBase* gauges_[kMaxGauges] = {};
    uint8_t gauge_count_ = 0u;
    bool configuration_ok_ = true;
    bool described_ = false;
};

} // namespace hostlink
} // namespace alchemy

#undef ALCHEMY_LOG_FORMAT
