/**
  ******************************************************************************
  * @file    httpd_post.h
  * @brief   lwIP httpd POST hooks (httpd_post_begin/receive_data/finished)
  *          for the "send a text message" web form: POST /message with an
  *          application/x-www-form-urlencoded body field msg=... gets
  *          URL-decoded and pushed onto UartMessageQueueHandle, which
  *          StartUartTask() (main.c) drains and sends via
  *          UART_SendBlocking() (uart_transfer.h).
  *
  * These callbacks run on lwIP's tcpip_thread (httpd_init() is called from
  * main() before osKernelStart() - see the comment above MX_LWIP_Init() in
  * main.c). They must never block: no HAL_UART_Transmit here, and the queue
  * push uses a 0 timeout (drop the message rather than stall the network
  * stack if UartTask has fallen behind).
  *
  * No heap allocation: per-connection accumulation state lives in a small
  * fixed-size static table, not mem_malloc() - see
  * docs/hardware-lessons-learned.md for why this project is deliberately
  * cautious about adding new mem_malloc() traffic near the lwIP heap.
  ******************************************************************************
  */
#ifndef HTTPD_POST_H
#define HTTPD_POST_H

#ifdef __cplusplus
extern "C" {
#endif

/* httpd_post_begin/httpd_post_receive_data/httpd_post_finished are declared
 * by lwIP itself (httpd.h, under LWIP_HTTPD_SUPPORT_POST) - this header just
 * groups the feature's own constants/doc. The definitions are in
 * httpd_post.c. */

#ifdef __cplusplus
}
#endif

#endif /* HTTPD_POST_H */
