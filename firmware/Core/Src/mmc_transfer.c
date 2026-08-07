/**
  ******************************************************************************
  * @file    mmc_transfer.c
  * @brief   See mmc_transfer.h for the rationale.
  ******************************************************************************
  */
#include "mmc_transfer.h"

extern MMC_HandleTypeDef hmmc1;

/* Set from HAL_MMC_RxCpltCallback/TxCpltCallback/ErrorCallback, which run
 * from SDMMC1_IRQn. Plain volatile flags are enough here: there is a single
 * in-flight transfer at a time (enforced by s_xferBusy below), and each flag
 * is only ever written by the ISR and read by the waiter below. */
static volatile uint8_t s_xferDone  = 0;
static volatile uint8_t s_xferError = 0;

/* Simple re-entrancy guard: hmmc1 is shared between the USB MSC path
 * (called from OTG_FS_IRQHandler) and the FatFs path (called from the main
 * super-loop / a future FreeRTOS task), and HAL_MMC only supports one
 * transfer in flight. This is a stand-in for a proper RTOS mutex - the
 * acquire/release pair below is what to replace with
 * osMutexAcquire()/osMutexRelease() once FreeRTOS is available. */
static volatile uint8_t s_xferBusy = 0;

static HAL_StatusTypeDef MMC_AcquireBus(void)
{
  HAL_StatusTypeDef result = HAL_OK;

  /* Check-and-set must be atomic with respect to OTG_FS_IRQn, which can
   * preempt a FatFs-path transfer already in progress in the main loop. */
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

/* Rounds [addr, addr+len) out to enclosing 32-byte cache-line boundaries,
 * as required by SCB_(Clean|Invalidate)DCache_by_Addr(). */
static void MMC_CacheRange(uint32_t addr, uint32_t len, uint32_t *out_addr, int32_t *out_len)
{
  uint32_t start = addr & ~0x1FUL;
  uint32_t end   = (addr + len + 0x1FUL) & ~0x1FUL;

  *out_addr = start;
  *out_len  = (int32_t)(end - start);
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

  s_xferDone  = 0U;
  s_xferError = 0U;

  status = HAL_MMC_ReadBlocks_DMA(&hmmc1, buf, blk_addr, blk_len);
  if (status == HAL_OK)
  {
    status = MMC_WaitForComplete(timeout_ms);

    /* IDMA (bus master) wrote directly to buf - invalidate so the CPU
     * doesn't read stale cached data. */
    uint32_t addr;
    int32_t len;
    MMC_CacheRange((uint32_t)buf, blk_len * MMC_TRANSFER_BLOCK_SIZE, &addr, &len);
    SCB_InvalidateDCache_by_Addr((uint32_t *)addr, len);
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

  s_xferDone  = 0U;
  s_xferError = 0U;

  /* Push CPU writes out to RAM before IDMA (bus master) reads them. */
  uint32_t addr;
  int32_t len;
  MMC_CacheRange((uint32_t)buf, blk_len * MMC_TRANSFER_BLOCK_SIZE, &addr, &len);
  SCB_CleanDCache_by_Addr((uint32_t *)addr, len);

  status = HAL_MMC_WriteBlocks_DMA(&hmmc1, buf, blk_addr, blk_len);
  if (status == HAL_OK)
  {
    status = MMC_WaitForComplete(timeout_ms);
  }

  MMC_ReleaseBus();
  return status;
}
