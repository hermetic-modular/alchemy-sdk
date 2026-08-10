/**
 * @file alchemy/usb/usb_device.h
 * @brief Composite USB device (CDC HostLink + class-compliant MIDI).
 *        Target-only.
 *
 * The CDC function behaves exactly like the shipped single-class device
 * (same endpoints and usbd_cdc_if plumbing).  MIDI RX delivers 4-byte
 * event packets in USB IRQ context — copy out and return.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alchemy/usb/usb_descriptors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    ALCHEMY_USB_PERIPH_FS = 0, /* Seed onboard connector (V1)           */
    ALCHEMY_USB_PERIPH_HS = 1, /* Seed external pins → panel USB-C (V2) */
} AlchemyUsbPeriph;

typedef void (*AlchemyUsbMidiRxFn)(const uint8_t packet[4], void* ctx);

/** Idempotent; the first caller's arguments win.  @p product_name must
 *  outlive the device (string literal); NULL keeps the default. */
void alchemy_usb_start(AlchemyUsbPeriph periph, const char* product_name);

/** True while the host has the device configured.  Falls false on
 *  suspend, reset and detach — poll it to catch cable pulls, which need
 *  not produce any other event with VBUS sense off. */
bool alchemy_usb_configured(void);

void alchemy_usb_midi_set_rx(AlchemyUsbMidiRxFn fn, void* ctx);

bool alchemy_usb_midi_tx_ready(void);

/** Queues one bulk-IN burst of whole 4-byte packets (len ≤ 64, len % 4
 *  == 0).  Returns bytes accepted: all of @p len, or 0 when busy /
 *  unconfigured / bad length.  Single caller. */
uint16_t alchemy_usb_midi_write(const uint8_t* packets, uint16_t len);

#ifdef __cplusplus
}
#endif
