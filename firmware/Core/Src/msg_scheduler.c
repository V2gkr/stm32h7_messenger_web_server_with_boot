/**
  ******************************************************************************
  * @file    msg_scheduler.c
  * @brief   See msg_scheduler.h for the rationale.
  ******************************************************************************
  */
#include "msg_scheduler.h"
#include "rtc_time.h"
#include "cmsis_os2.h"

extern osMutexId_t SchedMutexHandle;
extern osMessageQueueId_t UartMessageQueueHandle;

/** Bounded wait - Scheduler_Submit() may be called from tcpip_thread
 * (httpd_post.c), which must never block for long. */
#define SCHED_MUTEX_TIMEOUT_MS   50U

static ScheduledMessage s_pending[SCHED_MAX_PENDING];

bool Scheduler_Submit(const UartMessage *msg, uint32_t due_epoch)
{
  bool ok = false;

  if (osMutexAcquire(SchedMutexHandle, SCHED_MUTEX_TIMEOUT_MS) != osOK)
    return false;

  for (uint32_t i = 0; i < SCHED_MAX_PENDING; i++)
  {
    if (!s_pending[i].valid)
    {
      s_pending[i].msg = *msg;
      s_pending[i].due_epoch = due_epoch;
      s_pending[i].valid = 1U;
      ok = true;
      break;
    }
  }

  osMutexRelease(SchedMutexHandle);
  return ok;
}

void Scheduler_Poll(void)
{
  uint32_t now = RTC_GetUnixTime();

  if (osMutexAcquire(SchedMutexHandle, SCHED_MUTEX_TIMEOUT_MS) != osOK)
    return;

  for (uint32_t i = 0; i < SCHED_MAX_PENDING; i++)
  {
    if (s_pending[i].valid && s_pending[i].due_epoch <= now)
    {
      /* Same drop-if-full policy as the immediate path in httpd_post.c -
       * never block whatever thread this runs on. */
      (void)osMessageQueuePut(UartMessageQueueHandle, &s_pending[i].msg, 0, 0);
      s_pending[i].valid = 0U;
    }
  }

  osMutexRelease(SchedMutexHandle);
}
