/**
 * @file usb/usb_descriptors.c
 * @brief Composite descriptor bytes.  CDC interfaces/endpoints match the
 *        shipped single-class device byte-for-byte; MIDI follows the
 *        USB-MIDI 1.0 appendix B example, one virtual cable.
 */

#include "alchemy/usb/usb_descriptors.h"

#define U16_LE(v) (uint8_t)((v) & 0xFFu), (uint8_t)(((v) >> 8) & 0xFFu)

const uint8_t alchemy_usb_dev_desc[ALCHEMY_USB_DEV_DESC_SIZE] = {
    0x12,                            /* bLength */
    0x01,                            /* bDescriptorType: DEVICE */
    0x00, 0x02,                      /* bcdUSB 2.00 */
    0xEF,                            /* bDeviceClass: Miscellaneous */
    0x02,                            /* bDeviceSubClass: Common Class */
    0x01,                            /* bDeviceProtocol: IAD */
    0x40,                            /* bMaxPacketSize0 */
    U16_LE(ALCHEMY_USB_VID),         /* idVendor */
    U16_LE(ALCHEMY_USB_PID),         /* idProduct */
    U16_LE(ALCHEMY_USB_BCD_DEVICE),  /* bcdDevice */
    0x01,                            /* iManufacturer */
    0x02,                            /* iProduct */
    0x03,                            /* iSerialNumber */
    0x01,                            /* bNumConfigurations */
};

uint8_t alchemy_usb_cfg_desc[ALCHEMY_USB_CFG_DESC_SIZE] = {
    /* Configuration */
    0x09, 0x02, U16_LE(ALCHEMY_USB_CFG_DESC_SIZE),
    ALCHEMY_USB_NUM_INTERFACES,
    0x01,                            /* bConfigurationValue */
    0x04,                            /* iConfiguration */
    0xC0,                            /* bmAttributes: self-powered */
    0x32,                            /* bMaxPower: 100 mA */

    /* IAD: CDC function, interfaces 0..1 */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x01, 0x00,

    /* Interface 0: CDC Communications (ACM) */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    /* CS: Header, bcdCDC 1.10 */
    0x05, 0x24, 0x00, 0x10, 0x01,
    /* CS: Call Management, data interface 1 */
    0x05, 0x24, 0x01, 0x00, 0x01,
    /* CS: ACM capabilities */
    0x04, 0x24, 0x02, 0x02,
    /* CS: Union, master 0 slave 1 */
    0x05, 0x24, 0x06, 0x00, 0x01,
    /* EP 0x82: interrupt IN, 8 bytes, bInterval 16 */
    0x07, 0x05, ALCHEMY_CDC_CMD_EP, 0x03, 0x08, 0x00, 0x10,

    /* Interface 1: CDC Data */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    /* EP 0x01: bulk OUT, 64 */
    0x07, 0x05, ALCHEMY_CDC_OUT_EP, 0x02, 0x40, 0x00, 0x00,
    /* EP 0x81: bulk IN, 64 */
    0x07, 0x05, ALCHEMY_CDC_IN_EP, 0x02, 0x40, 0x00, 0x00,

    /* IAD: audio function, interfaces 2..3 */
    0x08, 0x0B, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,

    /* Interface 2: AudioControl */
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    /* CS AC: header, bcdADC 1.00, one streaming interface (#3) */
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x03,

    /* Interface 3: MIDIStreaming */
    0x09, 0x04, 0x03, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    /* CS MS: header, bcdMSC 1.00, wTotalLength 65 */
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
    /* MIDI IN jack, embedded, id 1 */
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
    /* MIDI IN jack, external, id 2 */
    0x06, 0x24, 0x02, 0x02, 0x02, 0x00,
    /* MIDI OUT jack, embedded, id 3, source jack 2 pin 1 */
    0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
    /* MIDI OUT jack, external, id 4, source jack 1 pin 1 */
    0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,
    /* EP 0x02: bulk OUT, 64 (audio-class 9-byte form) */
    0x09, 0x05, ALCHEMY_MIDI_OUT_EP, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    /* CS EP: 1 embedded jack, id 1 */
    0x05, 0x25, 0x01, 0x01, 0x01,
    /* EP 0x83: bulk IN, 64 */
    0x09, 0x05, ALCHEMY_MIDI_IN_EP, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    /* CS EP: 1 embedded jack, id 3 */
    0x05, 0x25, 0x01, 0x01, 0x03,
};

const uint8_t alchemy_usb_qualifier_desc[ALCHEMY_USB_QUALIFIER_DESC_SIZE] = {
    0x0A, 0x06, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0x01, 0x00,
};
