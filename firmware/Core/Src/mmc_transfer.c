/**
  ******************************************************************************
  * @file    mmc_transfer.c
  * @brief   See mmc_transfer.h for the rationale.
  ******************************************************************************
  */
#include "mmc_transfer.h"
#include <string.h>

extern MMC_HandleTypeDef hmmc1;

/* DMA bounce buffer: 16 blocks (8 KB) transferred per HAL_MMC_*Blocks_DMA
 * call. Placed in .mmc_dma_sec (STM32H750XX_FLASH.ld), inside the
 * non-cacheable MPU region set up in MPU_Config() (main.c) - so it is never
 * cached and needs no manual Clean/InvalidateDCache_by_Addr() calls at all.
 * Both the USB MSC path (usbd_storage_if.c) and the FatFs path
 * (user_diskio.c) share this single buffer; s_xferBusy below ensures only
 * one transfer uses it at a time. 16 is a multiple of 8, so chunking here
 * never breaks the 4K-native-sector alignment rule some eMMC parts enforce
 * (HAL_MMC_*Blocks_DMA requires BlockAdd/NumberOfBlocks % 8 == 0 in that
 * case) as long as the caller's blk_addr was already sector-aligned. */
#define MMC_BOUNCE_BLOCKS   16U
#define MMC_BOUNCE_BYTES    (MMC_BOUNCE_BLOCKS * MMC_TRANSFER_BLOCK_SIZE)

static uint8_t s_bounce[MMC_BOUNCE_BYTES]
  __attribute__((section(".mmc_dma_sec"), aligned(32)));

/* Set from HAL_MMC_RxCpltCallback/TxCpltCallback/ErrorCallback, which run
 * from SDMMC1_IRQn. Plain volatile flags are enough here: there is a single
 * in-flight transfer at a time (enforced by s_xferBusy below), and each flag
 * is only ever written by the ISR and read by the waiter below. */
static volatile uint8_t s_xferDone  = 0;
static volatile uint8_t s_xferError = 0;

/* Simple re-entrancy guard: hmmc1 (and s_bounce) are shared between the USB
 * MSC path (called from OTG_FS_IRQHandler) and the FatFs path (called from
 * a FreeRTOS task), and only one transfer can use the bounce buffer / HAL_MMC
 * state machine at a time. This is IRQ-mask based on purpose, NOT a FreeRTOS
 * mutex: the USB MSC path runs inside a hardware ISR, where taking a real
 * RTOS mutex is not valid usage (no ISR-safe blocking semantics for
 * priority-inheritance mutexes). If a task-level FreeRTOS mutex is added for
 * extra safety between multiple tasks, keep this guard underneath it - it's
 * still the only thing that can exclude the ISR path. */
static volatile uint8_t s_xferBusy = 0;

static HAL_StatusTypeDef MMC_AcquireBus(void)
{
  HAL_StatusTypeDef result = HAL_OK;

  /* Check-and-set must be atomic with respect to OTG_FS_IRQn, which can
   * preempt a FatFs-path transfer already in progress in a task. */
  __disable_irq();
  if (s_xferBusy != 0U)
  {
    result = HAL_BUSY;
  }
  else
  {
    s_xferBusy = 1U;
  }
  __enable_irq();

  return result;
}

static void MMC_ReleaseBus(void)
{
  s_xferBusy = 0U;
}

/**
  * @brief  Waits for the completion/error callback to fire, or times out.
  *
  * TODO(RTOS): once FreeRTOS is available, replace the spin below with
  * osSemaphoreAcquire(s_xferDoneSem, timeout_ms), and have
  * HAL_MMC_RxCpltCallback/TxCpltCallback/ErrorCallback below give that
  * semaphore instead of setting s_xferDone. No caller of MMC_ReadBlocks()/
  * MMC_WriteBlocks() needs to change. Do NOT do this for the USB MSC path -
  * it runs inside OTG_FS_IRQHandler (hardware ISR), where a blocking RTOS
  * wait is not legal; only FatFs-path callers (running in task/thread
  * context) could use it.
  */
static HAL_StatusTypeDef MMC_WaitForComplete(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();

  while (s_xferDone == 0U)
  {
    if ((HAL_GetTick() - start) >= timeout_ms)
    {
      return HAL_TIMEOUT;
    }
  }

  return (s_xferError != 0U) ? HAL_ERROR : HAL_OK;
}

void HAL_MMC_RxCpltCallback(MMC_HandleTypeDef *hmmc)
{
  (void)hmmc;
  s_xferDone = 1U;
}

void HAL_MMC_TxCpltCallback(MMC_HandleTypeDef *hmmc)
{
  (void)hmmc;
  s_xferDone = 1U;
}

void HAL_MMC_ErrorCallback(MMC_HandleTypeDef *hmmc)
{
  (void)hmmc;
  s_xferError = 1U;
  s_xferDone  = 1U;
}

HAL_StatusTypeDef MMC_ReadBlocks(uint8_t *buf, uint32_t blk_addr, uint32_t blk_len, uint32_t timeout_ms)
{
  HAL_StatusTypeDef status = MMC_AcquireBus();

  if (status != HAL_OK)
  {
    return status;
  }

  uint32_t remaining = blk_len;
  uint32_t addr = blk_addr;
  uint8_t *dst = buf;

  while (remaining > 0U)
  {
    uint32_t chunk = (remaining > MMC_BOUNCE_BLOCKS) ? MMC_BOUNCE_BLOCKS : remaining;

    s_xferDone  = 0U;
    s_xferError = 0U;

    status = HAL_MMC_ReadBlocks_DMA(&hmmc1, s_bounce, addr, chunk);
    if (status == HAL_OK)
    {
      status = MMC_WaitForComplete(timeout_ms);
    }
    if (status != HAL_OK)
    {
      break;
    }

    /* s_bounce lives in non-cacheable memory (.mmc_dma_sec) - no
     * SCB_InvalidateDCache_by_Addr() needed, IDMA's writes are already
     * visible to the CPU. */
    memcpy(dst, s_bounce, chunk * MMC_TRANSFER_BLOCK_SIZE);

    dst      += chunk * MMC_TRANSFER_BLOCK_SIZE;
    addr     += chunk;
    remaining -= chunk;
  }

  MMC_ReleaseBus();
  return status;
}

HAL_StatusTypeDef MMC_WriteBlocks(const uint8_t *buf, uint32_t blk_addr, uint32_t blk_len, uint32_t timeout_ms)
{
  HAL_StatusTypeDef status = MMC_AcquireBus();

  if (status != HAL_OK)
  {
    return status;
  }

  uint32_t remaining = blk_len;
  uint32_t addr = blk_addr;
  const uint8_t *src = buf;

  while (remaining > 0U)
  {
    uint32_t chunk = (remaining > MMC_BOUNCE_BLOCKS) ? MMC_BOUNCE_BLOCKS : remaining;

    /* s_bounce lives in non-cacheable memory - no SCB_CleanDCache_by_Addr()
     * needed, IDMA will read exactly what memcpy() just wrote. */
    memcpy(s_bounce, src, chunk * MMC_TRANSFER_BLOCK_SIZE);

    s_xferDone  = 0U;
    s_xferError = 0U;

    status = HAL_MMC_WriteBlocks_DMA(&hmmc1, s_bounce, addr, chunk);
    if (status == HAL_OK)
    {
      status = MMC_WaitForComplete(timeout_ms);
    }
    if (status != HAL_OK)
    {
      break;
    }

    src      += chunk * MMC_TRANSFER_BLOCK_SIZE;
    addr     += chunk;
    remaining -= chunk;
  }

  MMC_ReleaseBus();
  return status;
}
