/**
 * @file usb/usb_device.c
 * @brief Device bring-up + descriptor table for the composite device.
 *        Reuses libDaisy's USBD handles and usbd_cdc_if fops, so the
 *        shared IRQ handlers and UsbHandle transmit path stay valid.
 */

#include <stdio.h>
#include <string.h>

#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

#include "alchemy/usb/usb_descriptors.h"
#include "alchemy/usb/usb_device.h"

extern USBD_HandleTypeDef hUsbDeviceFS;
extern USBD_HandleTypeDef hUsbDeviceHS;

void alchemy_usb_composite_bind(USBD_HandleTypeDef* pdev);
extern USBD_ClassTypeDef alchemy_usb_composite_class;

static bool s_started = false;
static const char* s_product = "Alchemy Lab";

static uint8_t s_str_buf[USBD_MAX_STR_DESC_SIZ];

static uint8_t* DeviceDesc(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    *length = (uint16_t)ALCHEMY_USB_DEV_DESC_SIZE;
    return (uint8_t*)(uintptr_t)alchemy_usb_dev_desc;
}

static uint8_t* LangIdDesc(USBD_SpeedTypeDef speed, uint16_t* length)
{
    static uint8_t lang_id[4] = {0x04, USB_DESC_TYPE_STRING, 0x09, 0x04};
    (void)speed;
    *length = 4U;
    return lang_id;
}

static uint8_t* StringDesc(const char* s, uint16_t* length)
{
    USBD_GetString((uint8_t*)(uintptr_t)s, s_str_buf, length);
    return s_str_buf;
}

static uint8_t* ManufacturerDesc(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    return StringDesc("Hermetic Modular", length);
}

static uint8_t* ProductDesc(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    return StringDesc(s_product, length);
}

static uint8_t* SerialDesc(USBD_SpeedTypeDef speed, uint16_t* length)
{
    static char serial[25];
    (void)speed;
    if (serial[0] == '\0')
    {
        snprintf(serial, sizeof serial, "%08lX%08lX%08lX",
                 (unsigned long)HAL_GetUIDw2(), (unsigned long)HAL_GetUIDw1(),
                 (unsigned long)HAL_GetUIDw0());
    }
    return StringDesc(serial, length);
}

static uint8_t* ConfigDesc(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    return StringDesc("Alchemy Config", length);
}

static uint8_t* InterfaceDesc(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    return StringDesc("Alchemy Interface", length);
}

static USBD_DescriptorsTypeDef AlchemyUsb_Desc = {
    .GetDeviceDescriptor = DeviceDesc,
    .GetLangIDStrDescriptor = LangIdDesc,
    .GetManufacturerStrDescriptor = ManufacturerDesc,
    .GetProductStrDescriptor = ProductDesc,
    .GetSerialStrDescriptor = SerialDesc,
    .GetConfigurationStrDescriptor = ConfigDesc,
    .GetInterfaceStrDescriptor = InterfaceDesc,
};

void alchemy_usb_start(AlchemyUsbPeriph periph, const char* product_name)
{
    if (s_started)
    {
        return;
    }
    s_started = true;

    if (product_name != NULL)
    {
        s_product = product_name;
    }

    USBD_HandleTypeDef* pdev;
    USBD_CDC_ItfTypeDef* fops;
    uint8_t dev_id;

    if (periph == ALCHEMY_USB_PERIPH_HS)
    {
        pdev = &hUsbDeviceHS;
        fops = &USBD_Interface_fops_HS;
        dev_id = DEVICE_HS;
    }
    else
    {
        pdev = &hUsbDeviceFS;
        fops = &USBD_Interface_fops_FS;
        dev_id = DEVICE_FS;
    }

    USBD_Init(pdev, &AlchemyUsb_Desc, dev_id);

    /* Re-sequence the FIFO map over libDaisy's two-FIFO default for the
     * two extra IN endpoints.  HAL_PCDEx_SetTxFiFo derives each offset
     * from the live registers, so a full in-order pass is consistent.
     * HS: 1008/1024 words.  FS: 320/320. */
    PCD_HandleTypeDef* hpcd = (PCD_HandleTypeDef*)pdev->pData;
    if (dev_id == DEVICE_HS)
    {
        HAL_PCDEx_SetRxFiFo(hpcd, 0x200);
        HAL_PCDEx_SetTxFiFo(hpcd, 0, 0x80);
        HAL_PCDEx_SetTxFiFo(hpcd, 1, 0x120);
        HAL_PCDEx_SetTxFiFo(hpcd, 2, 0x10);
        HAL_PCDEx_SetTxFiFo(hpcd, 3, 0x40);
    }
    else
    {
        HAL_PCDEx_SetRxFiFo(hpcd, 0x80);
        HAL_PCDEx_SetTxFiFo(hpcd, 0, 0x40);
        HAL_PCDEx_SetTxFiFo(hpcd, 1, 0x40);
        HAL_PCDEx_SetTxFiFo(hpcd, 2, 0x10);
        HAL_PCDEx_SetTxFiFo(hpcd, 3, 0x30);
    }

    USBD_RegisterClass(pdev, &alchemy_usb_composite_class);
    USBD_CDC_RegisterInterface(pdev, fops);
    alchemy_usb_composite_bind(pdev);
    USBD_Start(pdev);

    HAL_PWREx_EnableUSBVoltageDetector();
}
