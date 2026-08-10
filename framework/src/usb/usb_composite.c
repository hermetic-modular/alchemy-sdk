/**
 * @file usb/usb_composite.c
 * @brief One USBD class serving both functions of the composite.
 *
 * The CDC function is delegated to ST's shipped USBD_CDC handlers, so
 * the serial path is behaviorally identical to the single-class device.
 * This class only routes: interfaces 0/1 and endpoints 0x01/0x81/0x82
 * to CDC, interfaces 2/3 and endpoints 0x02/0x83 to the MIDI handlers.
 */

#include <string.h>

#include "usbd_core.h"
#include "usbd_cdc.h"

#include "alchemy/usb/usb_descriptors.h"
#include "alchemy/usb/usb_device.h"

#define MIDI_IN_IDX (ALCHEMY_MIDI_IN_EP & 0x0FU)
#define MIDI_OUT_IDX (ALCHEMY_MIDI_OUT_EP & 0x0FU)

static USBD_HandleTypeDef* s_pdev = NULL;

/* DWC2 moves FIFO data by word — keep endpoint buffers aligned. */
__ALIGN_BEGIN static uint8_t s_midi_rx_buf[ALCHEMY_MIDI_EP_PACKET_SIZE]
    __ALIGN_END;
__ALIGN_BEGIN static uint8_t s_midi_tx_buf[ALCHEMY_MIDI_EP_PACKET_SIZE]
    __ALIGN_END;

static volatile uint8_t s_midi_tx_busy = 0U;

static AlchemyUsbMidiRxFn s_rx_fn = NULL;
static void* s_rx_ctx = NULL;

static uint8_t Composite_Init(USBD_HandleTypeDef* pdev, uint8_t cfgidx)
{
    const uint8_t ret = USBD_CDC.Init(pdev, cfgidx);
    if (ret != (uint8_t)USBD_OK)
    {
        return ret;
    }

    (void)USBD_LL_OpenEP(pdev, ALCHEMY_MIDI_IN_EP, USBD_EP_TYPE_BULK,
                         ALCHEMY_MIDI_EP_PACKET_SIZE);
    pdev->ep_in[MIDI_IN_IDX].is_used = 1U;

    (void)USBD_LL_OpenEP(pdev, ALCHEMY_MIDI_OUT_EP, USBD_EP_TYPE_BULK,
                         ALCHEMY_MIDI_EP_PACKET_SIZE);
    pdev->ep_out[MIDI_OUT_IDX].is_used = 1U;

    s_midi_tx_busy = 0U;
    (void)USBD_LL_PrepareReceive(pdev, ALCHEMY_MIDI_OUT_EP, s_midi_rx_buf,
                                 ALCHEMY_MIDI_EP_PACKET_SIZE);
    return (uint8_t)USBD_OK;
}

static uint8_t Composite_DeInit(USBD_HandleTypeDef* pdev, uint8_t cfgidx)
{
    (void)USBD_LL_CloseEP(pdev, ALCHEMY_MIDI_IN_EP);
    pdev->ep_in[MIDI_IN_IDX].is_used = 0U;

    (void)USBD_LL_CloseEP(pdev, ALCHEMY_MIDI_OUT_EP);
    pdev->ep_out[MIDI_OUT_IDX].is_used = 0U;

    s_midi_tx_busy = 0U;
    return USBD_CDC.DeInit(pdev, cfgidx);
}

static uint8_t MidiInterfaceSetup(USBD_HandleTypeDef* pdev,
                                  USBD_SetupReqTypedef* req)
{
    static uint8_t alt0 = 0U;
    static uint16_t status0 = 0U;
    USBD_StatusTypeDef ret = USBD_OK;

    switch (req->bmRequest & USB_REQ_TYPE_MASK)
    {
        case USB_REQ_TYPE_STANDARD:
            switch (req->bRequest)
            {
                case USB_REQ_GET_STATUS:
                    if (pdev->dev_state == USBD_STATE_CONFIGURED)
                    {
                        (void)USBD_CtlSendData(pdev, (uint8_t*)&status0, 2U);
                    }
                    else
                    {
                        USBD_CtlError(pdev, req);
                        ret = USBD_FAIL;
                    }
                    break;

                case USB_REQ_GET_INTERFACE:
                    if (pdev->dev_state == USBD_STATE_CONFIGURED)
                    {
                        (void)USBD_CtlSendData(pdev, &alt0, 1U);
                    }
                    else
                    {
                        USBD_CtlError(pdev, req);
                        ret = USBD_FAIL;
                    }
                    break;

                case USB_REQ_SET_INTERFACE:
                    if ((pdev->dev_state != USBD_STATE_CONFIGURED)
                        || (req->wValue != 0U))
                    {
                        USBD_CtlError(pdev, req);
                        ret = USBD_FAIL;
                    }
                    break;

                case USB_REQ_CLEAR_FEATURE:
                    break;

                default:
                    USBD_CtlError(pdev, req);
                    ret = USBD_FAIL;
                    break;
            }
            break;

        default:
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
            break;
    }

    return (uint8_t)ret;
}

static uint8_t Composite_Setup(USBD_HandleTypeDef* pdev,
                               USBD_SetupReqTypedef* req)
{
    const uint8_t recipient = req->bmRequest & USB_REQ_RECIPIENT_MASK;
    const uint8_t target = (uint8_t)(req->wIndex & 0xFFU);

    if ((recipient == USB_REQ_RECIPIENT_INTERFACE) && (target >= 2U))
    {
        return MidiInterfaceSetup(pdev, req);
    }
    if ((recipient == USB_REQ_RECIPIENT_ENDPOINT)
        && ((target == ALCHEMY_MIDI_OUT_EP)
            || (target == ALCHEMY_MIDI_IN_EP)))
    {
        USBD_CtlError(pdev, req);
        return (uint8_t)USBD_FAIL;
    }
    return USBD_CDC.Setup(pdev, req);
}

static uint8_t Composite_EP0_RxReady(USBD_HandleTypeDef* pdev)
{
    return USBD_CDC.EP0_RxReady(pdev);
}

static uint8_t Composite_DataIn(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if ((epnum & 0x0FU) == MIDI_IN_IDX)
    {
        PCD_HandleTypeDef* hpcd = (PCD_HandleTypeDef*)pdev->pData;

        if ((pdev->ep_in[epnum & 0x0FU].total_length > 0U)
            && ((pdev->ep_in[epnum & 0x0FU].total_length
                 % hpcd->IN_ep[epnum & 0x0FU].maxpacket)
                == 0U))
        {
            pdev->ep_in[epnum & 0x0FU].total_length = 0U;
            (void)USBD_LL_Transmit(pdev, epnum, NULL, 0U);
        }
        else
        {
            s_midi_tx_busy = 0U;
        }
        return (uint8_t)USBD_OK;
    }
    return USBD_CDC.DataIn(pdev, epnum);
}

static uint8_t Composite_DataOut(USBD_HandleTypeDef* pdev, uint8_t epnum)
{
    if (epnum == MIDI_OUT_IDX)
    {
        const uint32_t len = USBD_LL_GetRxDataSize(pdev, epnum);

        if (s_rx_fn != NULL)
        {
            for (uint32_t i = 0U; (i + 4U) <= len; i += 4U)
            {
                s_rx_fn(&s_midi_rx_buf[i], s_rx_ctx);
            }
        }

        (void)USBD_LL_PrepareReceive(pdev, ALCHEMY_MIDI_OUT_EP, s_midi_rx_buf,
                                     ALCHEMY_MIDI_EP_PACKET_SIZE);
        return (uint8_t)USBD_OK;
    }
    return USBD_CDC.DataOut(pdev, epnum);
}

static uint8_t* Composite_GetCfgDesc(uint16_t* length)
{
    *length = (uint16_t)ALCHEMY_USB_CFG_DESC_SIZE;
    return alchemy_usb_cfg_desc;
}

static uint8_t* Composite_GetDeviceQualifierDesc(uint16_t* length)
{
    *length = (uint16_t)ALCHEMY_USB_QUALIFIER_DESC_SIZE;
    return (uint8_t*)(uintptr_t)alchemy_usb_qualifier_desc;
}

USBD_ClassTypeDef alchemy_usb_composite_class = {
    .Init = Composite_Init,
    .DeInit = Composite_DeInit,
    .Setup = Composite_Setup,
    .EP0_TxSent = NULL,
    .EP0_RxReady = Composite_EP0_RxReady,
    .DataIn = Composite_DataIn,
    .DataOut = Composite_DataOut,
    .SOF = NULL,
    .IsoINIncomplete = NULL,
    .IsoOUTIncomplete = NULL,
    .GetHSConfigDescriptor = Composite_GetCfgDesc,
    .GetFSConfigDescriptor = Composite_GetCfgDesc,
    .GetOtherSpeedConfigDescriptor = Composite_GetCfgDesc,
    .GetDeviceQualifierDescriptor = Composite_GetDeviceQualifierDesc,
};

/* The stock core stalls interface-directed setup requests above
 * USBD_MAX_NUM_INTERFACES (1) before this class sees them.  The link
 * wraps USBD_StdItfReq instead of editing libDaisy: REQUIRES
 * -Wl,--wrap=USBD_StdItfReq (CMake attaches it to alchemy::usb;
 * Makefile builds add it to LDFLAGS).  The anchor in
 * alchemy_usb_composite_bind keeps the wrapper referenced, so a lost
 * flag fails the link on __real_USBD_StdItfReq instead of degrading
 * silently. */
USBD_StatusTypeDef USBD_StdItfReq(USBD_HandleTypeDef* pdev,
                                  USBD_SetupReqTypedef* req);
USBD_StatusTypeDef __real_USBD_StdItfReq(USBD_HandleTypeDef* pdev,
                                         USBD_SetupReqTypedef* req);

USBD_StatusTypeDef __wrap_USBD_StdItfReq(USBD_HandleTypeDef* pdev,
                                         USBD_SetupReqTypedef* req)
{
    const uint8_t itf = (uint8_t)(req->wIndex & 0xFFU);

    if ((itf >= 2U) && (itf < ALCHEMY_USB_NUM_INTERFACES))
    {
        switch (pdev->dev_state)
        {
            case USBD_STATE_DEFAULT:
            case USBD_STATE_ADDRESSED:
            case USBD_STATE_CONFIGURED:
            {
                USBD_StatusTypeDef ret = USBD_FAIL;
                if ((pdev->pClass[0] != NULL)
                    && (pdev->pClass[0]->Setup != NULL))
                {
                    pdev->classId = 0U;
                    ret = (USBD_StatusTypeDef)pdev->pClass[0]->Setup(pdev,
                                                                     req);
                }
                if ((req->wLength == 0U) && (ret == USBD_OK))
                {
                    (void)USBD_CtlSendStatus(pdev);
                }
                return ret;
            }
            default:
                USBD_CtlError(pdev, req);
                return USBD_FAIL;
        }
    }
    return __real_USBD_StdItfReq(pdev, req);
}

static volatile uintptr_t s_itf_wrap_anchor;

void alchemy_usb_composite_bind(USBD_HandleTypeDef* pdev)
{
    s_pdev = pdev;
    s_itf_wrap_anchor = (uintptr_t)&__wrap_USBD_StdItfReq;
}

void alchemy_usb_midi_set_rx(AlchemyUsbMidiRxFn fn, void* ctx)
{
    s_rx_ctx = ctx;
    s_rx_fn = fn;
}

bool alchemy_usb_configured(void)
{
    return (s_pdev != NULL) && (s_pdev->dev_state == USBD_STATE_CONFIGURED);
}

bool alchemy_usb_midi_tx_ready(void)
{
    return alchemy_usb_configured() && (s_midi_tx_busy == 0U);
}

uint16_t alchemy_usb_midi_write(const uint8_t* packets, uint16_t len)
{
    if (!alchemy_usb_configured())
    {
        return 0U;
    }
    if ((len == 0U) || (len > ALCHEMY_MIDI_EP_PACKET_SIZE)
        || ((len & 3U) != 0U))
    {
        return 0U;
    }
    if (s_midi_tx_busy != 0U)
    {
        return 0U;
    }

    s_midi_tx_busy = 1U;
    memcpy(s_midi_tx_buf, packets, len);
    s_pdev->ep_in[MIDI_IN_IDX].total_length = len;
    (void)USBD_LL_Transmit(s_pdev, ALCHEMY_MIDI_IN_EP, s_midi_tx_buf, len);
    return len;
}
