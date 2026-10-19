/**
 * @file uart_port.h
 * @brief USART3 on the ST-Link virtual COM port (PD8 TX, PD9 RX, AF7), 8N1.
 *
 * RX is interrupt driven into a ring buffer; TX is polled. The ESP32 link will be a second
 * instance on USART6 (PG9/PG14, proposed) with its own ring; see README.
 */
#ifndef UART_PORT_H
#define UART_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Configure the pins, USART3 and its RX interrupt.
 * @param[in] baud     Baud rate (e.g. 115200).
 * @param[in] pclk_hz  Kernel clock of USART3 (APB1), used to compute BRR.
 * @return true on success, false for a baud rate the clock cannot produce.
 */
bool uart_init(uint32_t baud, uint32_t pclk_hz);

/**
 * @brief Pop one received byte, if any. Never blocks.
 * @param[out] byte  Receives the byte when true is returned.
 * @return true if a byte was available.
 */
bool uart_read_byte(uint8_t *byte);

/**
 * @brief Send bytes, blocking until they are in the transmitter.
 * @param[in] data  Bytes to send.
 * @param[in] len   Number of bytes.
 * @return true on success, false on a transmit timeout.
 */
bool uart_write(const uint8_t *data, size_t len);

/** @return Bytes dropped so far (ring full or hardware overrun/framing/noise error). */
uint32_t uart_drop_count(void);

#endif /* UART_PORT_H */
