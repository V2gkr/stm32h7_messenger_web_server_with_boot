/**
  ******************************************************************************
  * @file    rtc_time.h
  * @brief   UTC Unix-epoch <-> RTC calendar conversion, for the scheduled-
  *          message feature (msg_scheduler.h) and the lwIP SNTP integration
  *          (SNTP_SET_SYSTEM_TIME in lwipopts.h).
  *
  * Everything here is UTC, always - the RTC calendar is set from SNTP (UTC by
  * definition) and read back to compare against the "when" epoch the browser
  * sends (computed client-side via Date.getTime(), which is already UTC).
  * No timezone/DST conversion exists anywhere in this firmware - keeping it
  * that way avoids a whole class of off-by-one-hour bugs.
  *
  * Thread safety: HAL_RTC_GetTime/GetDate/SetTime/SetDate are plain register
  * accesses (RTC_TimeTypeDef/RTC_DateTypeDef, calendar shadow registers, the
  * write-protect key sequence on Set*) with no protection of their own
  * against two RTOS threads touching the RTC at once. In this project that
  * happens for real: SNTP writes the time from tcpip_thread (its UDP recv
  * callback, whenever a sync response arrives) while SchedTask reads it once
  * a second from its own thread (msg_scheduler.c) and httpd_post.c reads it
  * from tcpip_thread too (to decide "now" vs "later"). Both functions here
  * take rtcMutexHandle (CubeMX FreeRTOS panel, same static-allocation shape
  * as sdmmc_semHandle/uartTxSemHandle) internally - callers don't need to.
  *
  * The mutex wait is bounded (RTC_MUTEX_TIMEOUT_MS), not indefinite: both
  * functions may be called from tcpip_thread (via httpd_post.c and the SNTP
  * callback), which must never block for long. On a timeout,
  * RTC_GetUnixTime() returns 0 - callers already treat 0/unknown as "time
  * not known, send immediately" rather than risk a message never firing.
  ******************************************************************************
  */
#ifndef RTC_TIME_H
#define RTC_TIME_H

#include "stm32h7xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Current RTC calendar time as a UTC Unix epoch (seconds since
  *         1970-01-01T00:00:00Z).
  * @retval Epoch seconds, or 0 if the RTC mutex couldn't be acquired in time
  *         (treat as "unknown" - see thread-safety note above).
  */
uint32_t RTC_GetUnixTime(void);

/**
  * @brief  Set the RTC calendar from a UTC Unix epoch.
  * @param  epoch: seconds since 1970-01-01T00:00:00Z.
  * @retval HAL_OK / HAL_ERROR / HAL_TIMEOUT (mutex not acquired in time).
  */
HAL_StatusTypeDef RTC_SetUnixTime(uint32_t epoch);

#ifdef __cplusplus
}
#endif

#endif /* RTC_TIME_H */
