/**
  ******************************************************************************
  * @file    uart_transfer.c
  * @brief   See uart_transfer.h for the rationale.
  ******************************************************************************
  */
#include "uart_transfer.h"
#include "cmsis_os.h"
#include "cmsis_os2.h"
#include <string.h>

extern UART_HandleTypeDef huart3;
extern osSemaphoreId_t uartTxSemHandle;

/* 32-byte aligned and sized so SCB_CleanDCache_by_Addr() below always
 * operates on whole cache lines only - see uart_transfer.h for why that
 * matters here. */
__attribute__((aligned(32)))
static uint8_t s_uartTxBuf[UART_MESSAGE_MAX_LEN];

/* Set from HAL_UART_ErrorCallback, which runs from USART3_IRQn/DMA1_Stream0_IRQn.
 * A plain volatile flag is enough: UartTask is the only caller of
 * UART_SendBlocking() (see uart_transfer.h), so there is always exactly one
 * in-flight transfer, and the flag is only ever written by the ISR and read
 * by the waiter below - same reasoning as mmc_transfer.c's s_xferError. */
static volatile uint8_t s_txError = 0;

HAL_StatusTypeDef UART_SendBlocking(const char *data, uint16_t len, uint32_t timeout_ms)
{
  HAL_StatusTypeDef status;

  if (len > UART_MESSAGE_MAX_LEN)
    len = UART_MESSAGE_MAX_LEN;

  memcpy(s_uartTxBuf, data, len);

  /* Push the memcpy() above out of the D-cache before DMA reads it - the
   * buffer's fixed 32-byte alignment/size guarantees this never touches a
   * neighboring object (see uart_transfer.h). */
  SCB_CleanDCache_by_Addr((uint32_t *)s_uartTxBuf, sizeof(s_uartTxBuf));

  s_txError = 0U;

  status = HAL_UART_Transmit_DMA(&huart3, s_uartTxBuf, len);
  if (status != HAL_OK)
    return status;

  if (osSemaphoreAcquire(uartTxSemHandle, timeout_ms) != osOK)
    return HAL_TIMEOUT;

  if (s_txError)
    return HAL_ERROR;

  return HAL_OK;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART3)
    return;
  osSemaphoreRelease(uartTxSemHandle);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART3)
    return;
  s_txError = 1U;
  osSemaphoreRelease(uartTxSemHandle);
}
