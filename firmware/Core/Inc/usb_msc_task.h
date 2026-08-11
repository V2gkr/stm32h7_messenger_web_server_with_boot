/**
  ******************************************************************************
  * @file    usb_msc_task.h
  * @brief   Defers USB Setup/DataOut/DataIn stage processing out of
  *          OTG_FS_IRQHandler into a dedicated FreeRTOS task (UsbMscTask), so
  *          STORAGE_Read_FS()/STORAGE_Write_FS() (usbd_storage_if.c) can
  *          submit requests to MemoryTask via memoryqueueHandle and block on
  *          xTaskNotifyWait() - exactly like user_diskio.c already does for
  *          FatFs - instead of calling MMC_ReadBlocks()/MMC_WriteBlocks()
  *          directly from ISR context.
  *
  * Why this exists: STORAGE_Read_FS()/STORAGE_Write_FS() used to call
  * MMC_ReadBlocks()/MMC_WriteBlocks() directly. Those wait on
  * osSemaphoreAcquire() with a non-zero timeout, which CMSIS-RTOS2 rejects
  * from ISR context (IS_IRQ() -> immediate osErrorParameter) - so every USB
  * MSC transfer failed. The fix isn't just "wait differently inside the
  * ISR": the entire BOT/SCSI call chain
  *   HAL_PCD_IRQHandler -> USBD_LL_DataOutStage/DataInStage ->
  *   MSC_BOT_DataOut/DataIn -> SCSI_ProcessCmd -> SCSI_Read10/Write10 ->
  *   SCSI_ProcessRead/ProcessWrite -> STORAGE_Read_FS/Write_FS
  * runs synchronously in one call stack, inside OTG_FS_IRQHandler. An ISR
  * cannot block waiting for a FreeRTOS task to make progress - Handler mode
  * blocks Thread-mode scheduling until the ISR returns, so a task can never
  * be scheduled while the ISR is still executing (only interrupt-to-interrupt
  * nesting by NVIC priority works, e.g. SDMMC1_IRQn preempting a spinning
  * OTG_FS_IRQn). So the whole stage dispatch has to move into a task
  * *before* it reaches STORAGE_Read_FS/Write_FS, not just at that last step.
  *
  * How: HAL_PCD_RegisterCallback(..., HAL_PCD_SETUPSTAGE_CB_ID, ...) /
  * HAL_PCD_RegisterDataOutStageCallback() / HAL_PCD_RegisterDataInStageCallback()
  * (called from UsbMscTask_RegisterCallbacks(), invoked from usb_device.c's
  * preserved USER CODE USB_DEVICE_Init_PostTreatment section, right after
  * MX_USB_DEVICE_Init() has installed the HAL defaults) replace the default
  * HAL PCD stage callbacks with the ISR-safe, enqueue-only versions in
  * usb_msc_task.c. UsbMscTask then dequeues each event and calls the real
  * USBD_LL_SetupStage()/DataOutStage()/DataInStage() (usbd_core.h - plain,
  * not ISR-only, functions) from task context. By the time execution reaches
  * STORAGE_Read_FS/Write_FS, it is already on UsbMscTask's stack, so blocking
  * on memoryqueueHandle/xTaskNotifyWait is safe.
  *
  * HAL_PCD_SOFCallback is deliberately left as the HAL default (still called
  * directly from OTG_FS_IRQHandler, unchanged) - USBD_MSC's
  * USBD_ClassTypeDef.SOF is NULL, so there is nothing to defer.
  *
  * Requires USE_HAL_PCD_REGISTER_CALLBACKS == 1 (Core/Inc/stm32h7xx_hal_conf.h)
  * - hand-flipped to 1U there for now since it isn't USER-CODE-protected;
  * when regenerating from the .ioc, also enable "Register Callback" for
  * USB_DEVICE/PCD in CubeMX (Project Manager > Advanced Settings) so it
  * survives regeneration.
  *
  * Regeneration note: usbd_conf.c / usbd_core.c / the MSC class sources
  * (Middlewares/ST/STM32_USB_Device_Library - gitignored, regenerated fresh
  * by STM32CubeMX, no USER CODE markers) are NOT touched. Only this
  * hand-written file plus one function call in usb_device.c's preserved
  * USER CODE marker are needed.
  *
  * Temporary hand-written status: UsbEventQueueHandle/UsbMscTaskHandle are
  * currently declared/created by hand in main.c's USER CODE PV/RTOS_QUEUES/
  * RTOS_THREADS sections (static CMSIS-RTOS2 allocation, matching the style
  * CubeMX generates for MemoryTask/memoryqueue). Once this design is
  * validated, add UsbEventQueue/UsbMscTask to the .ioc's FreeRTOS config and
  * regenerate - CubeMX will then emit the equivalent declarations outside
  * USER CODE markers with matching names; delete the hand-written USER CODE
  * copies in main.c at that point.
  ******************************************************************************
  */
#ifndef USB_MSC_TASK_H
#define USB_MSC_TASK_H

#include "cmsis_os.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  USB_STAGE_SETUP,
  USB_STAGE_DATA_OUT,
  USB_STAGE_DATA_IN,
} UsbStageType;

typedef struct {
  UsbStageType stage;
  uint8_t      epnum;          /* unused for USB_STAGE_SETUP */
  uint8_t      setup_data[8];  /* valid only for USB_STAGE_SETUP - copied out
                                   of hpcd->Setup, which HAL reuses for the
                                   next SETUP packet, so it can't just be
                                   referenced by pointer here. */
} UsbStageEvent;

/**
  * @brief  Overrides the default HAL PCD Setup/DataOut/DataIn stage callbacks
  *         on hpcd_USB_OTG_FS with the deferred (enqueue-only) versions in
  *         usb_msc_task.c. Call once, after MX_USB_DEVICE_Init() has already
  *         run USBD_LL_Init() (which installs the HAL defaults) - see
  *         usb_device.c's USB_DEVICE_Init_PostTreatment.
  */
void UsbMscTask_RegisterCallbacks(void);

/**
  * @brief  UsbMscTask entry point (osThreadNew target, created in main.c).
  *         Dequeues UsbStageEvents and dispatches them to the real
  *         USBD_LL_SetupStage()/DataOutStage()/DataInStage() in task context.
  */
void StartUsbMscTask(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* USB_MSC_TASK_H */
