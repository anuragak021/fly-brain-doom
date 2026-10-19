/**
 * @file uart_port.h
 * @brief Two UARTs, 8N1: the ST-Link virtual COM port (USART3, PD8 TX / PD9 RX, AF7) for the
 *        laptop, and the ESP32 link (USART6, PG14 TX / PG9 RX, AF7; Nucleo Zio D1 / D0).
 *
 * RX is interrupt driven into one ring buffer per port; TX is polled. The ports are
 * independent: a byte on one never reaches the other.
 */
#ifndef UART_PORT_H
#define UART_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Which UART. */
typedef enum {
    UART_VCP = 0, /**< USART3 on the ST-Link virtual COM port (APB1 kernel clock). */
    UART_ESP = 1, /**< USART6 on PG9 / PG14 (APB2 kernel clock). */
    UART_COUNT = 2
} uart_id_t;

/**
 * @brief Configure the pins, the USART and its RX interrupt.
 * @param[in] id       Which port.
 * @param[in] baud     Baud rate (e.g. 115200).
 * @param[in] pclk_hz  Kernel clock of that USART (board_pclk1_hz() for UART_VCP,
 *                     board_pclk2_hz() for UART_ESP), used to compute BRR.
 * @return true on success, false for a bad id or a baud rate the clock cannot produce.
 */
bool uart_init(uart_id_t id, uint32_t baud, uint32_t pclk_hz);

/**
 * @brief Pop one received byte, if any. Never blocks.
 * @param[in]  id    Which port.
 * @param[out] byte  Receives the byte when true is returned.
 * @return true if a byte was available.
 */
bool uart_read_byte(uart_id_t id, uint8_t *byte);

/**
 * @brief Send bytes, blocking until they are in the transmitter.
 * @param[in] id    Which port.
 * @param[in] data  Bytes to send.
 * @param[in] len   Number of bytes.
 * @return true on success, false on a bad id or a transmit timeout.
 */
bool uart_write(uart_id_t id, const uint8_t *data, size_t len);

/** @return Bytes dropped so far on that port (ring full or overrun/framing/noise error). */
uint32_t uart_drop_count(uart_id_t id);

#endif /* UART_PORT_H */
