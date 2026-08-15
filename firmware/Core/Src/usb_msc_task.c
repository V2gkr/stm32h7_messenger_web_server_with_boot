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

/* Declared the same way usbd_conf.c declares it (no main.h include there
 * either), needed by MSC_PCD_ResetCallback() below. */
void Error_Handler(void);

/* Diagnostic only: counts UsbStageEvents dropped because the queue was full
 * (osMessageQueuePut() failing from ISR context). Not expected in normal
 * operation - the BOT protocol keeps at most a couple of stages in flight -
 * but there's nothing better an ISR can do about a full queue than count it. */
static volatile uint32_t s_usbEventDropCount = 0;

/* Counts events discarded by StartUsbMscTask() because their stamped epoch
 * was stale (a USB reset happened after they were queued) - see the
 * "Stale-event hazard" note in usb_msc_task.h. Diagnostic only. */
static volatile uint32_t s_usbEventStaleCount = 0;

/* Incremented (from ISR, in MSC_PCD_ResetCallback) on every USB bus reset.
 * A plain volatile write is enough here - no RTOS primitive needed, and
 * osMessageQueueReset() can't be called from ISR context anyway. See the
 * "Stale-event hazard" note in usb_msc_task.h. */
static volatile uint32_t s_usbEpoch = 0;

static void MSC_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
  UsbStageEvent evt;
  evt.stage = USB_STAGE_SETUP;
  evt.epnum = 0U;
  evt.epoch = s_usbEpoch;
  memcpy(evt.setup_data, hpcd->Setup, sizeof(evt.setup_data));

  if (osMessageQueuePut(UsbEventQueueHandle, &evt, 0, 0) != osOK)
  {
    s_usbEventDropCount++;
  }
}

static void MSC_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  /* Capture xfer_buff synchronously, here in the ISR - see "Buffer-pointer
   * staleness hazard" in usb_msc_task.h. Do NOT re-read hpcd->OUT_ep[]
   * later in UsbMscTask. */
  UsbStageEvent evt = { .stage = USB_STAGE_DATA_OUT, .epnum = epnum,
                         .pdata = hpcd->OUT_ep[epnum].xfer_buff, .epoch = s_usbEpoch };

  if (osMessageQueuePut(UsbEventQueueHandle, &evt, 0, 0) != osOK)
  {
    s_usbEventDropCount++;
  }
}

static void MSC_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
  /* Capture xfer_buff synchronously, here in the ISR - see "Buffer-pointer
   * staleness hazard" in usb_msc_task.h. Do NOT re-read hpcd->IN_ep[]
   * later in UsbMscTask. */
  UsbStageEvent evt = { .stage = USB_STAGE_DATA_IN, .epnum = epnum,
                         .pdata = hpcd->IN_ep[epnum].xfer_buff, .epoch = s_usbEpoch };

  if (osMessageQueuePut(UsbEventQueueHandle, &evt, 0, 0) != osOK)
  {
    s_usbEventDropCount++;
  }
}

/* Replaces usbd_conf.c's (now-unregistered, static) PCD_ResetCallback - same
 * body, plus the epoch bump. Kept synchronous in the ISR (not deferred like
 * the stage callbacks above): it never touches the SD card, so there's no
 * blocking-wait hazard, and it needs to run immediately so the epoch bump is
 * ordered correctly against whatever Setup/DataOut/DataIn callbacks the USB
 * core fires right after it. */
static void MSC_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
  USBD_SpeedTypeDef speed = USBD_SPEED_FULL;

  s_usbEpoch++;

  if (hpcd->Init.speed == PCD_SPEED_HIGH)
  {
    speed = USBD_SPEED_HIGH;
  }
  else if (hpcd->Init.speed == PCD_SPEED_FULL)
  {
    speed = USBD_SPEED_FULL;
  }
  else
  {
    Error_Handler();
  }

  USBD_LL_SetSpeed(&hUsbDeviceFS, speed);
  USBD_LL_Reset(&hUsbDeviceFS);
}

void UsbMscTask_RegisterCallbacks(void)
{
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_FS, HAL_PCD_SETUPSTAGE_CB_ID, MSC_PCD_SetupStageCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_OTG_FS, HAL_PCD_RESET_CB_ID, MSC_PCD_ResetCallback);
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

    if (evt.epoch != s_usbEpoch)
    {
      /* A USB reset happened after this event was queued - it refers to a
       * session that no longer exists. See "Stale-event hazard" in
       * usb_msc_task.h. */
      s_usbEventStaleCount++;
      continue;
    }

    /* See "PCD register concurrency hazard" in usb_msc_task.h: USBD_LL_*Stage()
     * ends up touching the same OTG_FS peripheral registers HAL_PCD_IRQHandler
     * (OTG_FS_IRQHandler) does. Mask only that one IRQ line - SDMMC1_IRQn
     * (which MemoryTask's DMA-completion wait, reached from inside this same
     * dispatch via STORAGE_Read_FS/Write_FS, depends on) stays live. */
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
    switch (evt.stage)
    {
      case USB_STAGE_SETUP:
        USBD_LL_SetupStage(&hUsbDeviceFS, evt.setup_data);
        break;
      case USB_STAGE_DATA_OUT:
        USBD_LL_DataOutStage(&hUsbDeviceFS, evt.epnum, evt.pdata);
        break;
      case USB_STAGE_DATA_IN:
        USBD_LL_DataInStage(&hUsbDeviceFS, evt.epnum, evt.pdata);
        break;
    }
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
  }
}
