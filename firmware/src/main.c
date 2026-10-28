/**
 * @file main.c
 * @brief Brain firmware: serve the brain protocol on two UARTs, the ST-Link VCP (USART3, the
 *        laptop) and the ESP32 link (USART1), over one shared neuron state.
 *
 * Main loop: each link's bytes go through its own protocol parser; each CRC-valid frame is
 * answered by the brain server on the link it arrived on. Only one master should be stepping at a
 * time (see brain_link.h). LEDs: green toggles on each STEP, yellow on any frame, red lights on
 * a failed reply or a non-zero dropped-byte count.
 *
 * Live dashboard: once the ESP32 has stepped the brain and no laptop frame has ever been answered
 * on the VCP, the VCP carries a text dashboard (telemetry.h) for a terminal. It is sent without
 * blocking, a few bytes per loop pass, so the ESP link is never delayed by it. A laptop master
 * answered on the VCP switches it off for good (until reset), so text and frames never mix.
 */
#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>

#include "board.h"
#include "brain_link.h"
#include "brain_protocol.h"
#include "brain_server.h"
#include "telemetry.h"
#include "uart_port.h"

#define UART_BAUD 115200U
#define DASHBOARD_PERIOD_MS 250U /* a screen is about 1 KB: 90 ms of the 250 ms at 115200 baud */
#define DASHBOARD_BYTES_PER_PASS 16U
#ifdef BRAIN_ESP_USART6
#define ESP_LINK_NAME "ESP32 USART6"
#else
#define ESP_LINK_NAME "ESP32 USART1"
#endif

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

static telemetry_t g_tel;
static char g_screen[TELEMETRY_MAX_BYTES];
static size_t g_screen_len;
static size_t g_screen_pos;
static uint32_t g_next_screen_ms;

/** Dashboard allowed: the ESP32 is the master and no laptop frame was ever answered. */
static bool dashboard_enabled(void)
{
    return (g_links[UART_ESP].steps_served != 0U) && (g_links[UART_VCP].frames_served == 0U);
}

/** Render a new screen when due, and push the pending one out without waiting. */
static void dashboard_pump(uint32_t now_ms)
{
    if (!dashboard_enabled()) {
        g_screen_pos = g_screen_len; /* drop a half-sent screen */
        return;
    }
    telemetry_observe(&g_tel, &g_server.last);
    if ((g_screen_pos >= g_screen_len) && ((int32_t)(now_ms - g_next_screen_ms) >= 0)) {
        g_screen_len = telemetry_render(&g_tel, &g_server.last, now_ms, board_core_hz() / 1000U,
                                        ESP_LINK_NAME, g_screen, sizeof(g_screen));
        g_screen_pos = 0U;
        g_next_screen_ms = now_ms + DASHBOARD_PERIOD_MS;
    }
    for (uint32_t n = 0U; (n < DASHBOARD_BYTES_PER_PASS) && (g_screen_pos < g_screen_len); n++) {
        if (!uart_try_write_byte(UART_VCP, (uint8_t)g_screen[g_screen_pos])) {
            break;
        }
        g_screen_pos++;
    }
}

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
    telemetry_init(&g_tel);
    for (;;) {
        dashboard_pump(board_millis());
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
