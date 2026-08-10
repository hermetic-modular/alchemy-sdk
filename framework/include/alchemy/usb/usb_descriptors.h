/**
 * @file alchemy/usb/usb_descriptors.h
 * @brief Fixed descriptor set for the composite device (CDC + MIDI).
 *
 * Pure data — host tests pin these bytes.  The shape is platform-fixed;
 * any change to it is host-visible and must bump ALCHEMY_USB_BCD_DEVICE
 * so cached hosts re-read.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0x0483:0x5740 is the libDaisy CDC identity shipped firmware already
 * enumerates with; a dedicated PID lands here when assigned. */
#define ALCHEMY_USB_VID 0x0483u
#define ALCHEMY_USB_PID 0x5740u

/* 0x0200 was the CDC-only shape; 0x0210 is the composite shape. */
#define ALCHEMY_USB_BCD_DEVICE 0x0210u

#define ALCHEMY_CDC_CMD_EP 0x82u
#define ALCHEMY_CDC_OUT_EP 0x01u
#define ALCHEMY_CDC_IN_EP 0x81u
#define ALCHEMY_MIDI_OUT_EP 0x02u
#define ALCHEMY_MIDI_IN_EP 0x83u

#define ALCHEMY_USB_NUM_INTERFACES 4u
#define ALCHEMY_USB_CFG_DESC_SIZE 175u
#define ALCHEMY_USB_DEV_DESC_SIZE 18u
#define ALCHEMY_USB_QUALIFIER_DESC_SIZE 10u

#define ALCHEMY_MIDI_EP_PACKET_SIZE 64u

extern const uint8_t alchemy_usb_dev_desc[ALCHEMY_USB_DEV_DESC_SIZE];
/* Not const: the ST core stamps bDescriptorType into the served buffer
 * (usbd_ctlreq.c GET_DESCRIPTOR), so this must live in RAM. */
extern uint8_t alchemy_usb_cfg_desc[ALCHEMY_USB_CFG_DESC_SIZE];
extern const uint8_t
    alchemy_usb_qualifier_desc[ALCHEMY_USB_QUALIFIER_DESC_SIZE];

#ifdef __cplusplus
}
#endif
