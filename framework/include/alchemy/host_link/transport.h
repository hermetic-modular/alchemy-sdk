/**
 * @file alchemy/host_link/transport.h
 * @brief Byte-stream transport contract for HostLink.
 *
 * HostLink is transport-agnostic; the CDC USB implementation lives in
 * cdc_transport.h (target-only).  Host tests substitute an in-memory
 * loopback.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy {
namespace hostlink {

class IHostTransport
{
  public:
    virtual ~IHostTransport() = default;

    /** Drain up to @p max received bytes into @p dst; returns count. */
    virtual size_t Read(uint8_t* dst, size_t max) = 0;

    /** Enqueue @p n bytes for transmission; returns bytes accepted. */
    virtual size_t Write(const uint8_t* src, size_t n) = 0;

    /** Free space in the transmit queue. */
    virtual size_t WriteSpace() const = 0;

    /** Push queued bytes toward the hardware.  Called at 1 ms cadence. */
    virtual void Pump() {}

    /** True while a host connection is plausible (best effort). */
    virtual bool Connected() const { return true; }
};

} // namespace hostlink
} // namespace alchemy
