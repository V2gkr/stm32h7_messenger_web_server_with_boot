/**
  ******************************************************************************
  * @file    mmc_transfer.c
  * @brief   See mmc_transfer.h for the rationale.
  ******************************************************************************
  */
#include "mmc_transfer.h"
#include "cmsis_os.h"
#include "cmsis_os2.h"
#include "diskio.h"
#include <string.h>

extern MMC_HandleTypeDef hmmc1;
extern osSemaphoreId_t sdmmc_semHandle;
/* DMA bounce buffer: 16 blocks (8 KB) transferred per HAL_MMC_*Blocks_DMA
 * call. Placed in .mmc_dma_sec (STM32H750XX_FLASH.ld), inside the
 * non-cacheable MPU region set up in MPU_Config() (main.c) - so it is never
 * cached and needs no manual Clean/InvalidateDCache_by_Addr() calls at all.
 * Both the USB MSC path (usbd_storage_if.c, via UsbMscTask) and the FatFs
 * path (user_diskio.c) funnel through MemoryTask/memoryqueueHandle, so there
 * is only ever one caller of this buffer at a time - see mmc_transfer.h.
 * 16 is a multiple of 8, so chunking here
 * never breaks the 4K-native-sector alignment rule some eMMC parts enforce
 * (HAL_MMC_*Blocks_DMA requires BlockAdd/NumberOfBlocks % 8 == 0 in that
 * case) as long as the caller's blk_addr was already sector-aligned. */
#define MMC_BOUNCE_BLOCKS   16U
#define MMC_BOUNCE_BYTES    (MMC_BOUNCE_BLOCKS * MMC_TRANSFER_BLOCK_SIZE)

__attribute__(( aligned(32))) static uint8_t s_bounce[MMC_BOUNCE_BYTES];
 

/* Set from HAL_MMC_RxCpltCallback/TxCpltCallback/ErrorCallback, which run
 * from SDMMC1_IRQn. A plain volatile flag is enough here: MemoryTask is the
 * only caller of MMC_ReadBlocks()/MMC_WriteBlocks() (see mmc_transfer.h), so
 * there is always exactly one in-flight transfer, and the flag is only ever
 * written by the ISR and read by the waiter below. */
static volatile uint8_t s_xferError = 0;

//this function will basically check datastuct data , start sdmmc r/w operation and then wait for semaphore from irq
void MMC_ProcessRequest(FsDataStruct *dataStruct){
  HAL_StatusTypeDef mem_status;
  //i think here idea with wait4timeout is deprecated since we can assume timeout from semaphore
  switch(dataStruct->operation_type){
    case MEM_READ:{
      //mem_status=HAL_MMC_ReadBlocks_DMA(&hmmc1, dataStruct->buf, dataStruct->addr, dataStruct->size);
      mem_status=MMC_ReadBlocks(dataStruct->buf, dataStruct->addr, dataStruct->size, 1000);
      break;
    }
    case MEM_WRITE:{
      //mem_status=HAL_MMC_WriteBlocks_DMA(&hmmc1, dataStruct->buf, dataStruct->addr, dataStruct->size);
      mem_status=MMC_WriteBlocks(dataStruct->buf, dataStruct->addr, dataStruct->size, 1000);
      break;
    }
    default:
      xTaskNotify(dataStruct->handle, RES_ERROR, eSetValueWithOverwrite);
      //error
      return;
  }
  if(mem_status!=HAL_OK){
    xTaskNotify(dataStruct->handle, RES_ERROR, eSetValueWithOverwrite);
    return;
  }
  //here we can be only if we are waiting for operation to end 
  
  // if(sem_status!=osOK){
  //   xTaskNotify(dataStruct->handle, RES_ERROR, eSetValueWithOverwrite);
  //   return;
  // }
  xTaskNotify(dataStruct->handle, RES_OK, eSetValueWithOverwrite);
}


void HAL_MMC_RxCpltCallback(MMC_HandleTypeDef *hmmc)
{
  (void)hmmc;
  // s_xferDone = 1U;
  osSemaphoreRelease(sdmmc_semHandle);
}

void HAL_MMC_TxCpltCallback(MMC_HandleTypeDef *hmmc)
{
  (void)hmmc;
  // s_xferDone = 1U;
  osSemaphoreRelease(sdmmc_semHandle);
}

void HAL_MMC_ErrorCallback(MMC_HandleTypeDef *hmmc)
{
  (void)hmmc;
  s_xferError = 1U;
  osSemaphoreRelease(sdmmc_semHandle);
  // s_xferDone  = 1U;
}

HAL_StatusTypeDef MMC_ReadBlocks(uint8_t *buf, uint32_t blk_addr, uint32_t blk_len, uint32_t timeout_ms)
{
  HAL_StatusTypeDef status = HAL_OK;
  uint32_t remaining = blk_len;
  uint32_t addr = blk_addr;
  uint8_t *dst = buf;

  while (remaining > 0U)
  {
    uint32_t chunk = (remaining > MMC_BOUNCE_BLOCKS) ? MMC_BOUNCE_BLOCKS : remaining;

    s_xferError = 0U;

    status = HAL_MMC_ReadBlocks_DMA(&hmmc1, s_bounce, addr, chunk);
    if (status != HAL_OK)
      break;
    if(osSemaphoreAcquire(sdmmc_semHandle, timeout_ms)!=osOK){
      status=HAL_ERROR;
      break;
    }
    if(s_xferError){
      status=HAL_ERROR;
      break;
    }
    /* s_bounce lives in non-cacheable memory (.mmc_dma_sec) - no
     * SCB_InvalidateDCache_by_Addr() needed, IDMA's writes are already
     * visible to the CPU. */
    SCB_InvalidateDCache_by_Addr(s_bounce,sizeof(s_bounce));
    memcpy(dst, s_bounce, chunk * MMC_TRANSFER_BLOCK_SIZE);

    dst      += chunk * MMC_TRANSFER_BLOCK_SIZE;
    addr     += chunk;
    remaining -= chunk;
  }

  return status;
}

HAL_StatusTypeDef MMC_WriteBlocks(const uint8_t *buf, uint32_t blk_addr, uint32_t blk_len, uint32_t timeout_ms)
{
  HAL_StatusTypeDef status = HAL_OK;
  uint32_t remaining = blk_len;
  uint32_t addr = blk_addr;
  const uint8_t *src = buf;

  while (remaining > 0U)
  {
    uint32_t chunk = (remaining > MMC_BOUNCE_BLOCKS) ? MMC_BOUNCE_BLOCKS : remaining;

    /* s_bounce lives in non-cacheable memory - no SCB_CleanDCache_by_Addr()
     * needed, IDMA will read exactly what memcpy() just wrote. */
     SCB_CleanDCache_by_Addr((uint32_t*)s_bounce,sizeof(s_bounce));
    memcpy(s_bounce, src, chunk * MMC_TRANSFER_BLOCK_SIZE);

    s_xferError = 0U;

    status = HAL_MMC_WriteBlocks_DMA(&hmmc1, s_bounce, addr, chunk);
    if (status != HAL_OK)
      break;
    if(osSemaphoreAcquire(sdmmc_semHandle, timeout_ms)!=osOK){
      status=HAL_ERROR;
      break;
    }
    if(s_xferError){
      status=HAL_ERROR;
      break;
    }

    src      += chunk * MMC_TRANSFER_BLOCK_SIZE;
    addr     += chunk;
    remaining -= chunk;
  }

  return status;
}
