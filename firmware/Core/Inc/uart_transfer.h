/**
  ******************************************************************************
  * @file    uart_transfer.h
  * @brief   DMA-backed, cache-safe blocking send over USART3, for the
  *          web-message-to-UART feature. Structured after mmc_transfer.h/.c
  *          (DMA kickoff + wait-for-completion-callback via a dedicated
  *          semaphore, single caller only, no re-entrancy guard).
  *
  * Why cache maintenance is needed here and how it differs from
  * mmc_transfer.c: mmc_transfer.c's DMA bounce buffer lives in a dedicated
  * non-cacheable MPU region (.mmc_bounce_sec, MPU region 3) because the eMMC
  * IDMA transfers are large/frequent and that region was already carved out.
  * For UART TX, s_uartTxBuf is a small (UART_MESSAGE_MAX_LEN-byte),
  * 32-byte-aligned static buffer sized to a whole number of D-cache lines,
  * living in ordinary (cacheable) RAM_D1 .bss. Before handing it to
  * HAL_UART_Transmit_DMA(), UART_SendBlocking() calls
  * SCB_CleanDCache_by_Addr() on it so the DMA engine reads what the CPU just
  * wrote. Because the buffer's address and size are both exact multiples of
  * 32 bytes, the clean can never spill onto a neighboring object the way it
  * did with the under-aligned lwIP heap buffer described in
  * docs/hardware-lessons-learned.md - no invalidate is needed afterward since
  * this is TX-only (the peripheral only ever reads this buffer).
  *
  * Single caller, task context only: UART_SendBlocking() is called exactly
  * one place - StartUartTask() in main.c, which dequeues UartMessage entries
  * from UartMessageQueueHandle (filled by httpd_post.c from the web form).
  * That's why there's no s_txBusy-style guard here, same reasoning as
  * mmc_transfer.h's MMC_ReadBlocks()/MMC_WriteBlocks().
  ******************************************************************************
  */
#ifndef UART_TRANSFER_H
#define UART_TRANSFER_H

#include "cmsis_os.h"
#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"
#include <stddef.h>

/** Fixed message size, also the exact byte size of one UartMessageQueueHandle
  * item (see UartMessageQueue_attributes in main.c: item size 128) - keep
  * these in sync if the queue is ever regenerated with a different size. */
#define UART_MESSAGE_MAX_LEN   128U

typedef struct {
  char text[UART_MESSAGE_MAX_LEN];
} UartMessage;

/**
  * @brief  Send len bytes from data out over USART3 via DMA and block until
  *         the transfer completes (or errors/times out).
  * @param  data: source buffer (any alignment - copied into the internal
  *              DMA-safe buffer via memcpy() before the transfer).
  * @param  len: number of bytes to send. Capped at UART_MESSAGE_MAX_LEN.
  * @param  timeout_ms: wall-clock timeout waiting for HAL_UART_TxCpltCallback.
  * @retval HAL_OK / HAL_ERROR / HAL_TIMEOUT
  */
HAL_StatusTypeDef UART_SendBlocking(const char *data, uint16_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* UART_TRANSFER_H */
