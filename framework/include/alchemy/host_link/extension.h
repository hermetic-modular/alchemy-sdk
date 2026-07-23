/**
 * @file alchemy/host_link/extension.h
 * @brief IHostlinkExtension — pluggable command-block handlers for the
 *        HostLink protocol engine.
 *
 * The core engine owns the preset/blob/identity commands (protocol §3);
 * optional feature blocks — the filesystem family (§8) being the first —
 * plug in through this interface so the core never grows dependencies on
 * subsystems most modules don't ship (FatFS, sample engines, …).
 *
 * An extension claims a contiguous command range.  For every request in
 * that range the engine calls Handle() with the parsed frame and a
 * FrameWriter already positioned as the response (Begin(type|0x80, seq)
 * done); the extension writes the body — status byte first, per the
 * protocol — and returns.  The engine encodes and transmits.  Requests
 * outside every registered range keep answering UNSUPPORTED, so hosts
 * probe features exactly as §6 prescribes.
 *
 * Threading: Handle()/Tick() run in the control-loop poll, the same
 * context as every other HostLink command and the module's own storage
 * work — extensions never need locks against the panel or each other.
 */

#pragma once

#include <cstdint>

#include "alchemy/host_link/frame.h"

namespace alchemy {
namespace hostlink {

class IHostlinkExtension
{
  public:
    virtual ~IHostlinkExtension() = default;

    /** Claimed command range, inclusive.  Ranges of registered
     *  extensions must not overlap (first registered wins). */
    virtual uint8_t FirstCmd() const = 0;
    virtual uint8_t LastCmd() const  = 0;

    /**
     * Execute one request.  @p w is Begin()'d as the matching response;
     * write the body (u8 status first).  Runs on the control-loop
     * thread; keep each command's work bounded (one disk transfer of at
     * most max_body bytes is the intended budget).
     */
    virtual void Handle(const ParsedFrame& f, FrameWriter& w,
                        uint32_t now_ms) = 0;

    /** Called once per engine Poll() — expire cursors/transfers here. */
    virtual void Tick(uint32_t now_ms) { (void)now_ms; }

    /**
     * Optional descriptor advertisement: a `"key":value` fragment
     * (WITHOUT surrounding braces) merged into the descriptor's root
     * object, e.g. `"storage":{"sd":true,"fsv":1}`.  Return nullptr to
     * add nothing.  Must be a string literal or otherwise outlive the
     * link.
     */
    virtual const char* DescriptorRootJson() const { return nullptr; }
};

} // namespace hostlink
} // namespace alchemy
