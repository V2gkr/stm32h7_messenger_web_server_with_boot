/**
  ******************************************************************************
  * @file    usb_msc_task.c
  * @brief   See usb_msc_task.h for the rationale.
  ******************************************************************************
  */
#include "usb_msc_task.h"
#include "usbd_core.h"
#include <string.h>

/* hpcd_USB_OTG_FS is defined (non-static) in USB_DEVICE/Target/usbd_conf.c;
 * hUsbDeviceFS is defined (non-static) in USB_DEVICE/App/usb_device.c.
 * Neither header exposes an extern for them - usbd_storage_if.c externs
 * hUsbDeviceFS the same way. */
extern PCD_HandleTypeDef hpcd_USB_OTG_FS;
extern USBD_HandleTypeDef hUsbDeviceFS;

/* Created in main.c (USER CODE PV/RTOS_QUEUES) - see usb_msc_task.h for why
 * this is hand-written there instead of CubeMX-generated for now. */
extern osMessageQueueId_t UsbEventQueueHandle;

/* Diagnostic only: counts UsbStageEvents dropped because the queue was full
 * (osMessageQueuePut() failing from ISR context). Not expected in normal
 * operation - the BOT protocol keeps at most a couple of stages in flight -
 * but there's nothing better an ISR can do about a full queue than count it. */
static volatile uint32_t s_usbEventDropCount = 0;

static void MSC_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
  UsbStageEvent evt;
  evt.stage = USB_STAGE_SETUP;
  evt.epnum = 0U;
  memcpy(evt.setup_data, hpcd->Setup, sizeof(evt.setup_data));

  if (osMessageQueuePut(UsbEventQueueHandle, &evt, 0, 0) != osOK)
  {
    s_usbEventDropCount++;
  }
}

static void MSC_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  (void)hpcd;
  UsbStageEvent evt = { .stage = USB_STAGE_DATA_OUT, .epnum = epnum };

  if (osMessageQueuePut(UsbEventQueueHandle, &evt, 0, 0) != osOK)
  {
    s_usbEventDropCount++;
  }
}

static void MSC_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  (void)hpcd;
  UsbStageEvent evt = { .stage = USB_STAGE_DATA_IN, .epnum = epnum };

  if (osMessageQueuePut(UsbEventQueueHandle, &evt, 0, 0) != osOK)
  {
    s_usbEventDropCount++;
  }
}

void UsbMscTask_RegisterCallbacks(void)
{
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_FS, HAL_PCD_SETUPSTAGE_CB_ID, MSC_PCD_SetupStageCallback);
  HAL_PCD_RegisterDataOutStageCallback(&hpcd_USB_OTG_FS, MSC_PCD_DataOutStageCallback);
  HAL_PCD_RegisterDataInStageCallback(&hpcd_USB_OTG_FS, MSC_PCD_DataInStageCallback);
}

void StartUsbMscTask(void *argument)
{
  (void)argument;
  UsbStageEvent evt;

  for (;;)
  {
    if (osMessageQueueGet(UsbEventQueueHandle, &evt, 0, portMAX_DELAY) != osOK)
    {
      continue;
    }

    switch (evt.stage)
    {
      case USB_STAGE_SETUP:
        USBD_LL_SetupStage(&hUsbDeviceFS, evt.setup_data);
        break;
      case USB_STAGE_DATA_OUT:
        USBD_LL_DataOutStage(&hUsbDeviceFS, evt.epnum, hpcd_USB_OTG_FS.OUT_ep[evt.epnum].xfer_buff);
        break;
      case USB_STAGE_DATA_IN:
        USBD_LL_DataInStage(&hUsbDeviceFS, evt.epnum, hpcd_USB_OTG_FS.IN_ep[evt.epnum].xfer_buff);
        break;
    }
  }
}
