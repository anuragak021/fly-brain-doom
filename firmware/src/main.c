/**
 * @file main.c
 * @brief Brain firmware: serve the brain protocol on the ST-Link VCP (USART3).
 *
 * Main loop: bytes from the UART ring go through the protocol parser; each CRC-valid frame is
 * answered by the brain server. LEDs: green toggles on each STEP, yellow on any frame, red
 * lights while a dropped-byte count is non-zero.
 */
#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "brain_protocol.h"
#include "brain_server.h"
#include "uart_port.h"

#define UART_BAUD 115200U

/* All state is static: no heap, no recursion. Lives in DTCM (see brain.ld). */
static brain_server_t g_server;
static brain_parser_t g_parser;
static uint32_t g_last_rx_ms;

static void send_response(const brain_frame_t *req)
{
    uint8_t out[BRAIN_MAX_FRAME_BYTES];
    size_t out_len = 0U;
    if (brain_server_handle(&g_server, req, out, sizeof(out), &out_len) != BRAIN_OK) {
        board_led_set(BOARD_LED_RED, true);
        return;
    }
    if (!uart_write(out, out_len)) {
        board_led_set(BOARD_LED_RED, true);
    }
    board_led_toggle(BOARD_LED_YELLOW);
    if (req->type == BRAIN_MSG_STEP) {
        board_led_toggle(BOARD_LED_GREEN);
    }
}

static void process_byte(uint8_t byte)
{
    brain_frame_t frame;
    bool got = false;
    if (brain_parser_push(&g_parser, byte, &frame, &got) != BRAIN_OK) {
        return;
    }
    while (got) {
        send_response(&frame);
        if (brain_parser_poll(&g_parser, &frame, &got) != BRAIN_OK) {
            return;
        }
    }
}

int main(void)
{
    board_init();
    if (!uart_init(UART_BAUD, board_pclk1_hz())) {
        board_led_set(BOARD_LED_RED, true);
        for (;;) {
        }
    }
    if (brain_server_init(&g_server, board_core_hz() / 1000U, board_cycles) != BRAIN_OK) {
        board_led_set(BOARD_LED_RED, true);
        for (;;) {
        }
    }
    brain_parser_reset(&g_parser);
    for (;;) {
        uint8_t byte;
        while (uart_read_byte(&byte)) {
            g_last_rx_ms = board_millis();
            process_byte(byte);
        }
        if ((g_parser.count != 0U) &&
            ((board_millis() - g_last_rx_ms) > BRAIN_INTERBYTE_TIMEOUT_MS)) {
            brain_parser_reset(&g_parser); /* partial frame went stale */
        }
        if (uart_drop_count() != 0U) {
            board_led_set(BOARD_LED_RED, true);
        }
    }
}
