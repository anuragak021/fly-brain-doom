/**
 * @file main.c
 * @brief Brain firmware: serve the brain protocol on two UARTs, the ST-Link VCP (USART3, the
 *        laptop) and the ESP32 link (USART6), over one shared neuron state.
 *
 * Main loop: each link's bytes go through its own protocol parser; each CRC-valid frame is
 * answered by the brain server on the link it arrived on. Only one master should be stepping at a
 * time (see brain_link.h). LEDs: green toggles on each STEP, yellow on any frame, red lights on
 * a failed reply or a non-zero dropped-byte count.
 */
#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "brain_link.h"
#include "brain_protocol.h"
#include "brain_server.h"
#include "uart_port.h"

#define UART_BAUD 115200U

/* All state is static: no heap, no recursion. Lives in DTCM (see brain.ld). */
static brain_server_t g_server;
static brain_link_t g_links[UART_COUNT];

static bool port_read(void *ctx, uint8_t *byte)
{
    return uart_read_byte(*(const uart_id_t *)ctx, byte);
}

static bool port_write(void *ctx, const uint8_t *data, size_t len)
{
    return uart_write(*(const uart_id_t *)ctx, data, len);
}

static uart_id_t g_ids[UART_COUNT] = {UART_VCP, UART_ESP};

static void fail_forever(void)
{
    board_led_set(BOARD_LED_RED, true);
    for (;;) {
    }
}

static void show_result(const brain_link_result_t *res)
{
    if (res->frames != 0U) {
        board_led_toggle(BOARD_LED_YELLOW);
    }
    if (res->steps != 0U) {
        board_led_toggle(BOARD_LED_GREEN);
    }
    if (res->failed) {
        board_led_set(BOARD_LED_RED, true);
    }
}

int main(void)
{
    board_init();
    if (!uart_init(UART_VCP, UART_BAUD, board_pclk1_hz()) ||
        !uart_init(UART_ESP, UART_BAUD, board_pclk2_hz())) {
        fail_forever();
    }
    if (brain_server_init(&g_server, board_core_hz() / 1000U, board_cycles) != BRAIN_OK) {
        fail_forever();
    }
    for (uint32_t i = 0U; i < (uint32_t)UART_COUNT; i++) {
        if (brain_link_init(&g_links[i], port_read, port_write, &g_ids[i]) != BRAIN_OK) {
            fail_forever();
        }
    }
    for (;;) {
        for (uint32_t i = 0U; i < (uint32_t)UART_COUNT; i++) {
            brain_link_result_t res;
            if (brain_link_service(&g_links[i], &g_server, board_millis(), &res) == BRAIN_OK) {
                show_result(&res);
            }
            if (uart_drop_count(g_ids[i]) != 0U) {
                board_led_set(BOARD_LED_RED, true);
            }
        }
    }
}
