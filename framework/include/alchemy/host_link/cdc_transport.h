/**
 * @file alchemy/host_link/cdc_transport.h
 * @brief IHostTransport over libDaisy's USB CDC (FS peripheral).
 *
 * Target-only (includes libDaisy); host builds use a test transport.
 *
 * Receive: libDaisy's CDC callback fires at interrupt level with each
 * packet; we copy into a lock-free single-producer/single-consumer ring
 * (IRQ writes, Poll reads).  Overflow drops bytes — harmless, because
 * COBS framing + CRC turn any gap into a rejected frame and the host
 * retries.  Stop-and-wait hosts never get near the ring's capacity.
 *
 * Transmit: CDC_Transmit is zero-copy (the buffer must stay untouched
 * until the transfer completes) and reports BUSY while a transfer is in
 * flight.  We ping-pong two chunk buffers: a successful submit of chunk
 * B is the hardware's confirmation that chunk A's transfer finished, so
 * A becomes reusable exactly then.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/host_link/transport.h"
#include "hid/usb.h"

namespace alchemy {
namespace hostlink {

class CdcUsbTransport : public IHostTransport
{
  public:
    CdcUsbTransport() = default;

    /**
     * Initializes the CDC device on @p periph and installs the receive
     * callback.  One instance per system (the CDC callback has no
     * context arg).
     *
     * Pick the peripheral the board's USB connector is actually wired
     * to: Alchemy Lab routes the front-panel USB-C to the Seed2
     * DFM's EXTERNAL USB pins (OTG_HS in FS mode) — the same port its
     * bootloader serves DFU on (DSY_DFU_USE_EXT_USB=1) — so V2 firmware
     * must pass FS_EXTERNAL.  V1 boards expose the onboard port:
     * FS_INTERNAL.
     *
     * @p product_name, when non-null, replaces the USB product string
     * ("Daisy Seed …") the OS and browser port-picker display — e.g.
     * "Alchemy Lab".  Implemented by repointing the ST descriptor
     * table's GetProductStrDescriptor callback before the peripheral
     * initializes; stock libDaisy is untouched.  The pointer must stay
     * valid for the lifetime of the link (pass a string literal).
     */
    void Init(daisy::UsbHandle& usb,
              daisy::UsbHandle::UsbPeriph periph
              = daisy::UsbHandle::FS_INTERNAL,
              const char* product_name = nullptr);

    size_t Read(uint8_t* dst, size_t max) override;
    size_t Write(const uint8_t* src, size_t n) override;
    size_t WriteSpace() const override;
    void   Pump() override;

    /** Bytes dropped on RX overflow since boot (diagnostic). */
    uint32_t RxDropped() const { return rx_dropped_; }

  private:
    static void RxTrampoline(uint8_t* buf, uint32_t* len);
    void        OnRx(const uint8_t* buf, uint32_t len);

    static CdcUsbTransport* s_instance;

    daisy::UsbHandle*           usb_    = nullptr;
    daisy::UsbHandle::UsbPeriph periph_ = daisy::UsbHandle::FS_INTERNAL;

    /* Power-of-two rings; w/r indices wrap naturally via masking. */
    static constexpr size_t kRxRing = 2048u;
    static constexpr size_t kTxRing = 2048u;
    static constexpr size_t kChunk  = 256u;

    uint8_t           rx_[kRxRing];
    volatile uint16_t rx_w_      = 0u;   /* written at IRQ level  */
    uint16_t          rx_r_      = 0u;   /* consumed in Poll      */
    uint32_t          rx_dropped_ = 0u;

    uint8_t  tx_[kTxRing];
    uint16_t tx_w_ = 0u;
    uint16_t tx_r_ = 0u;

    uint8_t  chunk_[2][kChunk];
    uint8_t  chunk_sel_    = 0u;    /* buffer to prepare next        */
    uint16_t pending_len_  = 0u;    /* prepared-but-unaccepted bytes */
};

} // namespace hostlink
} // namespace alchemy
