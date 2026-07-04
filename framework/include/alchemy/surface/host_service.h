/**
 * @file alchemy/surface/host_service.h
 * @brief alchemy::HostService — role interface for a host-communication
 *        service attached to the ControlLoop (the HostLink pattern).
 *
 * Like KnobStorage / LockSource / CvSource, this is one of the loop's
 * fixed roles: `loop.Use(service)` slots it into the canonical frame and
 * the loop calls Poll() from its inner 1 ms poll — the same thread as
 * panel gestures, so host operations and on-device saves can never
 * interleave mid-operation.
 */

#pragma once

#include <cstdint>

namespace alchemy {

class ControlLoop;

class HostService
{
  public:
    virtual ~HostService() = default;

    /** Called at the loop's inner poll cadence (1 ms by default). */
    virtual void Poll(uint32_t t_ms) = 0;

    /** Called once, from ControlLoop::Use(*this). */
    virtual void OnAttach(ControlLoop&) {}
};

} // namespace alchemy
