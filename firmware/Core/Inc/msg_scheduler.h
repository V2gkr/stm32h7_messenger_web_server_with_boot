/**
  ******************************************************************************
  * @file    msg_scheduler.h
  * @brief   Holds messages submitted with a future delivery time and fires
  *          them onto the existing UartMessageQueueHandle/UartTask path
  *          (uart_transfer.h) once their time is due.
  *
  * Not a FreeRTOS queue: httpd_post.c needs to insert a message tagged with
  * an arbitrary due time, and SchedTask needs to repeatedly scan for
  * "which entries are now due" - a FIFO queue only supports strict
  * ordered consume, not that kind of scan/remove-selectively access. A
  * small fixed-size array + mutex is the right shape instead, same idea as
  * the pending-POST slot table in httpd_post.c.
  *
  * Producer is httpd_post.c (tcpip_thread), consumer is StartSchedTask
  * (its own thread, ~1s scan period) - both guarded by SchedMutexHandle
  * (CubeMX FreeRTOS panel, static allocation like SdmmcMutex/rtcMutex).
  ******************************************************************************
  */
#ifndef MSG_SCHEDULER_H
#define MSG_SCHEDULER_H

#include "uart_transfer.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How many not-yet-due messages can be held at once. Generous for a device
  * with one user typing messages by hand. */
#define SCHED_MAX_PENDING   8U

typedef struct {
  UartMessage msg;
  uint32_t due_epoch;   /* UTC unix seconds */
  uint8_t valid;
} ScheduledMessage;

/**
  * @brief  Hold msg until due_epoch (UTC), then it's handed to
  *         UartMessageQueueHandle the same way an immediate message is.
  * @param  msg: copied in - caller's copy can be reused/discarded after this
  *              returns.
  * @param  due_epoch: UTC unix seconds this message should fire at.
  * @retval false if the pending table is full or the guarding mutex
  *         couldn't be acquired in time (bounded wait - see msg_scheduler.c);
  *         callers should fall back to sending immediately rather than
  *         silently lose the message, same fail-open philosophy as
  *         RTC_GetUnixTime()'s 0-on-timeout return.
  */
bool Scheduler_Submit(const UartMessage *msg, uint32_t due_epoch);

/**
  * @brief  One scan pass: fires every pending message whose due_epoch has
  *         arrived onto UartMessageQueueHandle. Call this periodically
  *         (main.c's StartSchedTask does, every ~1s) - it does one bounded
  *         pass and returns, it is not itself a loop.
  */
void Scheduler_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* MSG_SCHEDULER_H */
