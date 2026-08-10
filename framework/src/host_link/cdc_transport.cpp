/**
 * @file host_link/cdc_transport.cpp
 * @brief USB CDC transport implementation (target-only).
 */

#include "alchemy/host_link/cdc_transport.h"

#include <cstring>

#include "alchemy/usb/usb_device.h"

namespace alchemy {
namespace hostlink {

CdcUsbTransport* CdcUsbTransport::s_instance = nullptr;

void CdcUsbTransport::Init(daisy::UsbHandle& usb,
                           daisy::UsbHandle::UsbPeriph periph,
                           const char* product_name)
{
    usb_       = &usb;
    periph_    = periph;
    s_instance = this;

    /* Composite bring-up replaces UsbHandle::Init; the CDC function
     * delegates to the same handlers, so the UsbHandle transmit path
     * and receive callback below are unchanged. */
    alchemy_usb_start(periph == daisy::UsbHandle::FS_INTERNAL
                          ? ALCHEMY_USB_PERIPH_FS
                          : ALCHEMY_USB_PERIPH_HS,
                      product_name);
    usb.SetReceiveCallback(&CdcUsbTransport::RxTrampoline, periph);
}

void CdcUsbTransport::RxTrampoline(uint8_t* buf, uint32_t* len)
{
    if (s_instance && buf && len)
        s_instance->OnRx(buf, *len);
}

void CdcUsbTransport::OnRx(const uint8_t* buf, uint32_t len)
{
    /* IRQ context: single producer.  rx_r_ may lag; on overflow, drop. */
    for (uint32_t i = 0u; i < len; i++)
    {
        const uint16_t next = static_cast<uint16_t>((rx_w_ + 1u) & (kRxRing - 1u));
        if (next == rx_r_)
        {
            rx_dropped_ += len - i;
            return;
        }
        rx_[rx_w_] = buf[i];
        rx_w_      = next;
    }
}

size_t CdcUsbTransport::Read(uint8_t* dst, size_t max)
{
    size_t         n = 0u;
    const uint16_t w = rx_w_;   /* snapshot the IRQ-side index once */
    while (rx_r_ != w && n < max)
    {
        dst[n++] = rx_[rx_r_];
        rx_r_    = static_cast<uint16_t>((rx_r_ + 1u) & (kRxRing - 1u));
    }
    return n;
}

size_t CdcUsbTransport::WriteSpace() const
{
    const uint16_t used = static_cast<uint16_t>((tx_w_ - tx_r_) & (kTxRing - 1u));
    return kTxRing - 1u - used;
}

size_t CdcUsbTransport::Write(const uint8_t* src, size_t n)
{
    size_t accepted = 0u;
    while (accepted < n)
    {
        const uint16_t next = static_cast<uint16_t>((tx_w_ + 1u) & (kTxRing - 1u));
        if (next == tx_r_) break;
        tx_[tx_w_] = src[accepted++];
        tx_w_      = next;
    }
    return accepted;
}

void CdcUsbTransport::Pump()
{
    if (!usb_) return;

    const auto transmit = [this](uint8_t* buf, size_t n) {
        return (periph_ == daisy::UsbHandle::FS_INTERNAL)
                   ? usb_->TransmitInternal(buf, n)
                   : usb_->TransmitExternal(buf, n);
    };

    /* Retry a prepared-but-unaccepted chunk first (zero-copy contract:
     * the other buffer may still be in flight until this submit lands). */
    if (pending_len_ > 0u)
    {
        if (transmit(chunk_[chunk_sel_], pending_len_)
            != daisy::UsbHandle::Result::OK)
            return;                     /* still busy — try next pump */
        chunk_sel_   ^= 1u;
        pending_len_  = 0u;
    }

    if (tx_w_ == tx_r_) return;

    /* Prepare the next chunk from the ring into the free buffer. */
    uint16_t n = 0u;
    while (tx_r_ != tx_w_ && n < kChunk)
    {
        chunk_[chunk_sel_][n++] = tx_[tx_r_];
        tx_r_ = static_cast<uint16_t>((tx_r_ + 1u) & (kTxRing - 1u));
    }
    pending_len_ = n;

    if (transmit(chunk_[chunk_sel_], pending_len_)
        == daisy::UsbHandle::Result::OK)
    {
        chunk_sel_   ^= 1u;
        pending_len_  = 0u;
    }
}

} // namespace hostlink
} // namespace alchemy
