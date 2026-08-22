/**
  ******************************************************************************
  * @file    httpd_post.c
  * @brief   See httpd_post.h for the rationale.
  ******************************************************************************
  */
#include "httpd_post.h"
#include "httpd.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "uart_transfer.h"
#include "rtc_time.h"
#include "msg_scheduler.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdlib.h>

extern osMessageQueueId_t UartMessageQueueHandle;

/** URI this feature reacts to; anything else is denied in httpd_post_begin(). */
#define POST_URI            "/message"
/** Static response body served back after a successful POST - see
  * README/plan for the note that this file must be copied onto the eMMC
  * card alongside index.html. */
#define POST_ACK_URI        "/ack.txt"

/** Cap on the raw (still URL-encoded) POST body. Comfortably covers a
  * UART_MESSAGE_MAX_LEN-1 character message even at 3x expansion (every
  * character percent-encoded) plus the "msg=" and "&when=<epoch>" fields. */
#define POST_MAX_BODY        512U
/** How many POST requests can be mid-flight at once. This device only ever
  * expects one browser talking to it, but a couple of spare slots avoids
  * needlessly denying a second tab/retry. */
#define POST_MAX_CONN        2U

typedef struct {
  void *connection;   /* NULL = free slot */
  char body[POST_MAX_BODY];
  uint16_t used;
} PostSlot;

static PostSlot s_slots[POST_MAX_CONN];

static PostSlot *post_slot_find(void *connection)
{
  for (uint32_t i = 0; i < POST_MAX_CONN; i++)
    if (s_slots[i].connection == connection)
      return &s_slots[i];
  return NULL;
}

static PostSlot *post_slot_alloc(void *connection)
{
  PostSlot *slot = post_slot_find(NULL);
  if (slot == NULL)
    return NULL;
  slot->connection = connection;
  slot->used = 0U;
  return slot;
}

static void post_slot_free(PostSlot *slot)
{
  slot->connection = NULL;
  slot->used = 0U;
}

/** In-place-ish '+' / '%XX' decode of an application/x-www-form-urlencoded
  * value. Copies at most dst_cap-1 decoded bytes into dst and NUL-terminates. */
static void url_decode(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
  size_t si = 0, di = 0;
  while (si < src_len && di + 1 < dst_cap)
  {
    char c = src[si];
    if (c == '+')
    {
      dst[di++] = ' ';
      si++;
    }
    else if (c == '%' && si + 2 < src_len)
    {
      char hex[3] = { src[si + 1], src[si + 2], '\0' };
      dst[di++] = (char)strtol(hex, NULL, 16);
      si += 3;
    }
    else
    {
      dst[di++] = c;
      si++;
    }
  }
  dst[di] = '\0';
}

/** Finds "msg=" in an accumulated application/x-www-form-urlencoded body and
  * URL-decodes its value into out (out_cap bytes, NUL-terminated). */
static void post_extract_msg(const char *body, uint16_t body_len, char *out, size_t out_cap)
{
  static const char key[] = "msg=";
  const size_t key_len = sizeof(key) - 1U;

  out[0] = '\0';
  for (uint16_t i = 0; i + key_len <= body_len; i++)
  {
    if (memcmp(&body[i], key, key_len) == 0)
    {
      uint16_t value_start = i + key_len;
      uint16_t value_end = value_start;
      while (value_end < body_len && body[value_end] != '&')
        value_end++;
      url_decode(&body[value_start], value_end - value_start, out, out_cap);
      return;
    }
  }
}

/** Finds "when=" (an optional UTC unix-epoch-seconds field) in an
  * accumulated application/x-www-form-urlencoded body. Returns 0 if the
  * field is absent, empty, or not a valid decimal number - callers already
  * treat 0 as "no scheduled time, send now" (see httpd_post_finished()). */
static uint32_t post_extract_epoch(const char *body, uint16_t body_len)
{
  static const char key[] = "when=";
  const size_t key_len = sizeof(key) - 1U;

  for (uint16_t i = 0; i + key_len <= body_len; i++)
  {
    if (memcmp(&body[i], key, key_len) == 0)
    {
      uint16_t value_start = i + key_len;
      char *endptr;
      unsigned long value = strtoul(&body[value_start], &endptr, 10);
      if (endptr == &body[value_start])
        return 0U;
      return (uint32_t)value;
    }
  }
  return 0U;
}

err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                        u16_t http_request_len, int content_len, char *response_uri,
                        u16_t response_uri_len, u8_t *post_auto_wnd)
{
  (void)http_request;
  (void)http_request_len;
  (void)response_uri;
  (void)response_uri_len;

  if (strcmp(uri, POST_URI) != 0)
    return ERR_ARG;

  if (content_len < 0 || (uint32_t)content_len >= POST_MAX_BODY)
    return ERR_VAL;

  if (post_slot_alloc(connection) == NULL)
    return ERR_VAL;

  *post_auto_wnd = 1U;
  return ERR_OK;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p)
{
  PostSlot *slot = post_slot_find(connection);

  if (slot == NULL)
  {
    pbuf_free(p);
    return ERR_VAL;
  }

  uint16_t room = (uint16_t)(POST_MAX_BODY - 1U - slot->used);
  uint16_t copy_len = (p->tot_len < room) ? p->tot_len : room;
  pbuf_copy_partial(p, &slot->body[slot->used], copy_len, 0);
  slot->used = (uint16_t)(slot->used + copy_len);

  pbuf_free(p);
  return ERR_OK;
}

void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len)
{
  PostSlot *slot = post_slot_find(connection);
  UartMessage msg;

  if (slot != NULL)
  {
    slot->body[slot->used] = '\0';
    post_extract_msg(slot->body, slot->used, msg.text, sizeof(msg.text));
    uint32_t when = post_extract_epoch(slot->body, slot->used);
    post_slot_free(slot);

    if (msg.text[0] != '\0')
    {
      /* when==0 (field absent) or already due (<= now, including the
       * desync case: a scheduled time that had already passed by the time
       * it got here, whether because it was in the past when submitted or
       * because the RTC wasn't caught up yet) both fall through to the
       * same immediate path as Phase 1 - no special-casing needed. */
      uint32_t now = RTC_GetUnixTime();
      if (when == 0U || when <= now)
      {
        /* Never block tcpip_thread: 0 timeout, drop the message if UartTask
         * has fallen behind and the queue is full. */
        (void)osMessageQueuePut(UartMessageQueueHandle, &msg, 0, 0);
      }
      else if (!Scheduler_Submit(&msg, when))
      {
        /* Pending table full or SchedMutex busy - fail open: send now
         * rather than silently lose the message. */
        (void)osMessageQueuePut(UartMessageQueueHandle, &msg, 0, 0);
      }
    }
  }

  strncpy(response_uri, POST_ACK_URI, response_uri_len - 1U);
  response_uri[response_uri_len - 1U] = '\0';
}
