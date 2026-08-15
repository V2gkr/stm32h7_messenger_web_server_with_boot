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
  * Buffer-pointer staleness hazard (found the hard way, second bug behind
  * the same symptom as below): the original synchronous HAL_PCD_DataOutStage/
  * DataInStageCallback read hpcd->OUT_ep[epnum].xfer_buff /
  * IN_ep[epnum].xfer_buff and passed it to USBD_LL_DataOutStage/DataInStage
  * in the very same ISR call - the read and the use happened atomically. Once
  * that dispatch is deferred to UsbMscTask, there's a real (usually short,
  * but nonzero) delay between the event being queued and UsbMscTask getting
  * to it - and the BOT protocol re-arms the same endpoint for its next stage
  * (CBW -> data -> CSW) in rapid succession, so hpcd->OUT_ep[epnum].xfer_buff/
  * IN_ep[epnum].xfer_buff can already point somewhere else by the time
  * UsbMscTask would read it. Fix: MSC_PCD_DataOutStageCallback/
  * DataInStageCallback capture the pointer synchronously in the ISR (like the
  * original code did) and store it *in* the UsbStageEvent - UsbMscTask uses
  * the stored pointer, never re-reads hpcd->{OUT,IN}_ep[].xfer_buff itself.
  *
  * PCD register concurrency hazard (found the hard way, third bug behind the
  * same symptom - device enumerates, INQUIRY/READ CAPACITY succeed, but the
  * host resets the port a second or two after the first Read(10), no
  * filesystem ever found): in the original synchronous design, *all* PCD
  * hardware access - both HAL_PCD_IRQHandler's own low-level FIFO/register
  * handling and everything USBD_LL_*Stage()/USBD_LL_Transmit()/
  * PrepareReceive() (deep inside MSC_BOT/SCSI) do - happened inside
  * OTG_FS_IRQHandler, i.e. strictly serialized by construction. Once stage
  * dispatch moves to UsbMscTask, USBD_LL_Transmit()/PrepareReceive() (called
  * from deep inside SCSI_ProcessRead/ProcessWrite/MSC_BOT_SendCSW while
  * UsbMscTask runs USBD_LL_DataOutStage/DataInStage) now touch
  * HAL_PCD_EP_Transmit()/Receive() - and the shared OTG_FS peripheral
  * registers they write (e.g. the RX FIFO handling machinery
  * HAL_PCD_IRQHandler itself relies on) - from *task* context, while
  * OTG_FS_IRQHandler (a real, higher-priority interrupt) can still preempt
  * UsbMscTask at any point and touch the same registers concurrently. Fix:
  * StartUsbMscTask() brackets each USBD_LL_*Stage() dispatch with
  * HAL_NVIC_DisableIRQ(OTG_FS_IRQn)/EnableIRQ() - targeted at that one IRQ
  * line only, never __disable_irq(), so SDMMC1_IRQn stays live throughout
  * (MemoryTask's DMA-completion semaphore wait, inside the very same
  * dispatch when it blocks on STORAGE_Read_FS/Write_FS, depends on it).
  * OTG_FS interrupts that arrive while masked aren't lost - NVIC latches
  * them pending and they fire as soon as the IRQ is re-enabled - just
  * delayed for the (usually short, occasionally up to ~1s if the dispatch is
  * mid SD-card wait) duration of one dispatch.
  *
  * Stale-event hazard after a USB bus reset (found the hard way - MSC
  * enumerated and answered INQUIRY/READ CAPACITY, but never showed a
  * filesystem and the host kept resetting the port): a USB reset can arrive
  * and be handled while UsbEventQueueHandle still holds Setup/DataOut/DataIn
  * events queued *before* the reset. HAL_PCD_ResetCallback (usbd_conf.c)
  * runs synchronously in the ISR - it isn't deferred, since it never touches
  * the SD card - but USBD_LL_Reset()/SetSpeed() it calls do reset the BOT/
  * SCSI state machine and endpoint bookkeeping immediately. If UsbMscTask
  * later dequeues and dispatches one of those pre-reset events, it's now
  * operating on stale epnum/buffer state from a session that no longer
  * exists, which desyncs the BOT protocol. osMessageQueueReset() can't be
  * called from the ISR to purge the backlog (CMSIS-RTOS2 rejects it from
  * IS_IRQ() context), so instead: MSC_PCD_ResetCallback() (usb_msc_task.c)
  * increments a plain volatile epoch counter (safe from ISR - just a
  * variable write); every UsbStageEvent is stamped with the epoch at enqueue
  * time; UsbMscTask discards (does not dispatch) any event whose stamped
  * epoch doesn't match the current epoch when it's dequeued. Not handled the
  * same way: Suspend/Resume/Connect/Disconnect - only Reset was observed to
  * trigger this, and those are rarer/lower-stakes, but the same hazard could
  * in principle apply to them too if it ever comes up.
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
  uint8_t     *pdata;          /* valid only for USB_STAGE_DATA_OUT/DATA_IN -
                                   hpcd->OUT_ep[epnum].xfer_buff /
                                   IN_ep[epnum].xfer_buff, captured in the ISR
                                   at enqueue time (see "Buffer-pointer
                                   staleness hazard" above) - NOT re-read
                                   later at dispatch time. */
  uint8_t      setup_data[8];  /* valid only for USB_STAGE_SETUP - copied out
                                   of hpcd->Setup, which HAL reuses for the
                                   next SETUP packet, so it can't just be
                                   referenced by pointer here. */
  uint32_t     epoch;          /* stamped with the current USB-reset epoch at
                                   enqueue time; UsbMscTask discards the event
                                   instead of dispatching it if this doesn't
                                   match anymore - see "Stale-event hazard"
                                   above. */
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
