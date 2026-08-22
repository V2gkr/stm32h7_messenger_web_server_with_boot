/**
  ******************************************************************************
  * @file    rtc_time.c
  * @brief   See rtc_time.h for the rationale.
  ******************************************************************************
  */
#include "rtc_time.h"
#include "cmsis_os2.h"

extern RTC_HandleTypeDef hrtc;
extern osMutexId_t rtcMutexHandle;

/** Bounded wait, not osWaitForever - both functions here may run on
 * tcpip_thread (see rtc_time.h), which must never block for long. */
#define RTC_MUTEX_TIMEOUT_MS   50U

/* Civil calendar <-> days-since-epoch conversion (proleptic Gregorian,
 * Howard Hinnant's well-known algorithm - http://howardhinnant.github.io/date_algorithms.html).
 * Used instead of <time.h>'s mktime()/timegm() to avoid pulling in a
 * timezone-aware C library path for what is, here, always a UTC-only
 * conversion. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
  y -= (m <= 2);
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);                 /* [0, 399] */
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      /* [0, 146096] */
  return era * 146097 + (int64_t)doe - 719468;               /* days since 1970-01-01 */
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);               /* [0, 146096] */
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
  int64_t y_ = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);    /* [0, 365] */
  unsigned mp = (5 * doy + 2) / 153;                         /* [0, 11] */
  *d = doy - (153 * mp + 2) / 5 + 1;                         /* [1, 31] */
  *m = mp + (mp < 10 ? 3 : -9);                               /* [1, 12] */
  *y = (int)(y_ + (*m <= 2));
}

/** RTC_WEEKDAY_MONDAY(1)..RTC_WEEKDAY_SUNDAY(7) from days-since-epoch
 * (1970-01-01 was a Thursday). Only used to keep the calendar's WeekDay
 * field self-consistent - nothing reads it back. */
static uint8_t weekday_from_days(int64_t z)
{
  /* z=0 (1970-01-01, Thursday) -> 4. Normalize into [0,6] with Sunday=0. */
  int wd = (int)(((z % 7) + 7 + 4) % 7);
  return (wd == 0) ? RTC_WEEKDAY_SUNDAY : (uint8_t)wd;
}

uint32_t RTC_GetUnixTime(void)
{
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  if (osMutexAcquire(rtcMutexHandle, RTC_MUTEX_TIMEOUT_MS) != osOK)
    return 0U;

  /* HAL requires GetTime then GetDate, in that order, to unlock the
   * calendar shadow registers - see RM0433. */
  HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

  osMutexRelease(rtcMutexHandle);

  int64_t days = days_from_civil(2000 + sDate.Year, sDate.Month, sDate.Date);
  return (uint32_t)(days * 86400
                     + sTime.Hours * 3600
                     + sTime.Minutes * 60
                     + sTime.Seconds);
}

HAL_StatusTypeDef RTC_SetUnixTime(uint32_t epoch)
{
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};
  HAL_StatusTypeDef status;
  int64_t days = (int64_t)(epoch / 86400U);
  uint32_t rem = epoch % 86400U;
  int year;
  unsigned month, day;

  civil_from_days(days, &year, &month, &day);

  sTime.Hours          = (uint8_t)(rem / 3600U);
  sTime.Minutes        = (uint8_t)((rem % 3600U) / 60U);
  sTime.Seconds        = (uint8_t)(rem % 60U);
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;

  sDate.Year    = (uint8_t)(year - 2000);
  sDate.Month   = (uint8_t)month;
  sDate.Date    = (uint8_t)day;
  sDate.WeekDay = weekday_from_days(days);

  if (osMutexAcquire(rtcMutexHandle, RTC_MUTEX_TIMEOUT_MS) != osOK)
    return HAL_TIMEOUT;

  /* SetTime then SetDate - same ordering as the read side. */
  status = HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  if (status == HAL_OK)
    status = HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

  osMutexRelease(rtcMutexHandle);
  return status;
}
