/**
 * @file oled_stream_main.c
 * @brief Phase 1 hardware demo: receive 128x64 gray frames over UART0 or USB Serial/JTAG
 *        (Kconfig DEMO_TRANSPORT), run pipeline
 *        stages 3 and 4 (dither, pack) on the ESP32-S3, push them to the SSD1306 over I2C, and
 *        reply with per-stage timings.
 *
 * The laptop (host/stream/stream_oled.py) runs stages 1 and 2 with the same C code.
 * The link carries only binary packets: the console is disabled in sdkconfig.defaults. Boot ROM
 * text still appears at reset; both ends resync on the packet magic.
 */
#include <stdbool.h>
#include <stdint.h>

#include "demo_stream.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled_ssd1306.h"
#include "op_dither.h"
#include "op_outline.h"
#include "op_pack.h"
#include "sdkconfig.h"

/* Home board pin map (README "Pin map"): OLED on GPIO8/9, not strapping, flash, PSRAM or USB. */
#define PIN_OLED_SDA (GPIO_NUM_8)
#define PIN_OLED_SCL (GPIO_NUM_9)

#define LINK_RX_BUFFER_BYTES (16384U) /* holds two frames, so the host can run ahead by one */
#define LINK_TX_BUFFER_BYTES (1024U)
#define RX_CHUNK_BYTES (4096U)
#define RX_TIMEOUT_MS (100U)
#define TX_TIMEOUT_MS (20U)
#define HELLO_PERIOD_US (1000000LL) /* re-announce while idle so a late host can sync */

static const char *TAG = "oled_stream";

/* Static, not on the stack: the parser alone holds an 8 KB payload. */
static ds_parser_t s_parser;
static uint8_t s_rx[RX_CHUNK_BYTES];
static uint8_t s_bits[OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX];
static uint8_t s_packed[OP_OLED_PACKED_BYTES];
static bool s_fast_i2c;

_Static_assert(DS_FRAME_PAYLOAD_BYTES == OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX,
               "demo frame must match the OLED size");
_Static_assert(OP_OLED_PACKED_BYTES == OLED_FRAME_BYTES, "pack output must match driver frame");

static uint32_t elapsed_us(int64_t start, int64_t end)
{
    const int64_t d = end - start;
    return (d < 0) ? 0U : (uint32_t)d;
}

/* ---- Link transport: the only code that differs between the two connectors. ---- */
#if CONFIG_DEMO_TRANSPORT_UART
#define LINK_UART (UART_NUM_0)
#define LINK_UART_TX_PIN (GPIO_NUM_43) /* wired to the board's USB-UART bridge (COM connector) */
#define LINK_UART_RX_PIN (GPIO_NUM_44)

static bool link_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_DEMO_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* Pins set explicitly: with the console disabled, nothing else guarantees UART0 is routed
     * to GPIO43/44 once the app starts. */
    return (uart_driver_install(LINK_UART, (int)LINK_RX_BUFFER_BYTES, (int)LINK_TX_BUFFER_BYTES, 0,
                                NULL, 0) == ESP_OK) &&
           (uart_param_config(LINK_UART, &cfg) == ESP_OK) &&
           (uart_set_pin(LINK_UART, LINK_UART_TX_PIN, LINK_UART_RX_PIN, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE) == ESP_OK);
}

static int link_read(uint8_t *buf, uint32_t len, TickType_t ticks)
{
    return uart_read_bytes(LINK_UART, buf, len, ticks);
}

static int link_write(const uint8_t *buf, size_t len)
{
    return uart_write_bytes(LINK_UART, buf, len);
}
#elif CONFIG_DEMO_TRANSPORT_USB_JTAG
static bool link_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = LINK_TX_BUFFER_BYTES,
        .rx_buffer_size = LINK_RX_BUFFER_BYTES,
    };
    return usb_serial_jtag_driver_install(&cfg) == ESP_OK;
}

static int link_read(uint8_t *buf, uint32_t len, TickType_t ticks)
{
    return usb_serial_jtag_read_bytes(buf, len, ticks);
}

static int link_write(const uint8_t *buf, size_t len)
{
    return usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(TX_TIMEOUT_MS));
}
#else
#error "select a DEMO_TRANSPORT in menuconfig"
#endif

static void send_reply(const ds_reply_t *r)
{
    uint8_t out[DS_REPLY_BYTES];
    const size_t n = ds_encode_reply(r, out, sizeof out);
    if (n == 0U) {
        ESP_LOGE(TAG, "reply encode failed");
        return;
    }
    const int written = link_write(out, n);
    if (written != (int)n) {
        ESP_LOGW(TAG, "reply dropped (host not reading?)");
    }
}

static void send_hello(void)
{
    const ds_reply_t r = {.seq = 0U,
                          .status = DS_STATUS_HELLO,
                          .crc_errors = s_parser.crc_errors,
                          .len_errors = s_parser.len_errors};
    send_reply(&r);
}

/** Apply the requested I2C speed; returns false if the bus could not be reconfigured. */
static bool apply_i2c_speed(bool fast)
{
    if (fast == s_fast_i2c) {
        return true;
    }
    const uint32_t hz = fast ? OLED_I2C_FREQ_FAST_HZ : OLED_I2C_FREQ_STD_HZ;
    if (oled_set_freq(hz) != OLED_OK) {
        ESP_LOGE(TAG, "I2C speed change to %lu Hz failed", (unsigned long)hz);
        return false;
    }
    s_fast_i2c = fast;
    ESP_LOGI(TAG, "I2C now %lu Hz", (unsigned long)hz);
    return true;
}

/** Stage 3 for the style requested by the host (DS_STYLE_*). */
static op_status_t render(uint32_t style, const uint8_t *gray)
{
    switch (style) {
    case DS_STYLE_BAYER4:
        return op_dither_bayer4(gray, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, s_bits);
    case DS_STYLE_FLOYD_STEINBERG:
        return op_dither_fs(gray, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, s_bits);
    case DS_STYLE_OUTLINE: {
        op_outline_cfg_t cfg;
        const op_status_t st = op_outline_cfg_default(&cfg);
        return (st == OP_OK) ? op_outline(gray, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, &cfg, s_bits)
                             : st;
    }
    default:
        return OP_ERR_RANGE;
    }
}

static void handle_frame(void)
{
    ds_reply_t r = {.seq = s_parser.seq, .flags = s_parser.flags, .status = DS_STATUS_OK};
    if (!apply_i2c_speed((s_parser.flags & DS_FLAG_FAST_I2C) != 0U)) {
        r.status = DS_STATUS_ERR_I2C;
    } else {
        const int64_t t0 = esp_timer_get_time();
        op_status_t st = render(s_parser.flags & DS_FLAG_STYLE_MASK, s_parser.payload);
        const int64_t t1 = esp_timer_get_time();
        if (st == OP_OK) {
            st = op_pack_ssd1306(s_bits, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, s_packed,
                                 sizeof s_packed);
        }
        const int64_t t2 = esp_timer_get_time();
        if (st != OP_OK) {
            r.status = DS_STATUS_ERR_PIPE;
        } else if (oled_write_frame(s_packed) != OLED_OK) {
            r.status = DS_STATUS_ERR_I2C;
        }
        const int64_t t3 = esp_timer_get_time();
        r.dither_us = elapsed_us(t0, t1);
        r.pack_us = elapsed_us(t1, t2);
        r.i2c_us = elapsed_us(t2, t3);
    }
    r.crc_errors = s_parser.crc_errors;
    r.len_errors = s_parser.len_errors;
    send_reply(&r);
}

/**
 * Boot screen: blank panel with a short bar in the middle. The SSD1306 keeps its RAM across an
 * ESP32 reset and holds random data after power up, so without this the old picture or noise
 * stays on screen and a running demo looks identical to a dead one.
 */
static const uint8_t *waiting_splash(void)
{
    enum { BAR_PAGE = 3U, BAR_X0 = 40U, BAR_X1 = 88U, BAR_BITS = 0x80U };
    for (uint32_t i = 0U; i < OP_OLED_PACKED_BYTES; i++) {
        s_packed[i] = 0U;
    }
    for (uint32_t x = BAR_X0; x < BAR_X1; x++) {
        s_packed[(BAR_PAGE * OP_OLED_WIDTH_PX) + x] = BAR_BITS; /* row 31, centre line */
    }
    return s_packed;
}

void app_main(void)
{
    if (!link_init()) {
        ESP_LOGE(TAG, "link driver install failed");
        return;
    }
    const oled_cfg_t oled_cfg = {
        .sda = PIN_OLED_SDA,
        .scl = PIN_OLED_SCL,
        .addr = (uint8_t)OLED_I2C_ADDR_DEFAULT,
        .freq_hz = OLED_I2C_FREQ_STD_HZ,
        /* Parallel to the module's own pull-ups: lowers the effective resistance, only helps
         * rise time. */
        .internal_pullup = true,
    };
    const oled_status_t ost = oled_init(&oled_cfg);
    if (ost != OLED_OK) {
        ESP_LOGE(TAG, "OLED init failed: %d", (int)ost);
        /* Keep running: replies will report DS_STATUS_ERR_I2C, which the host shows. */
    } else if (oled_write_frame(waiting_splash()) != OLED_OK) {
        ESP_LOGE(TAG, "OLED splash write failed");
    }
    ds_parser_init(&s_parser);
    send_hello();
    int64_t last_activity = esp_timer_get_time();

    for (;;) {
        const int n = link_read(s_rx, sizeof s_rx, pdMS_TO_TICKS(RX_TIMEOUT_MS));
        const int64_t now = esp_timer_get_time();
        if (n <= 0) {
            if ((now - last_activity) > HELLO_PERIOD_US) {
                send_hello();
                last_activity = now;
            }
            continue;
        }
        last_activity = now;
        /* n <= sizeof s_rx is guaranteed by the driver; bound the loop by it anyway. */
        const uint32_t count = ((uint32_t)n <= sizeof s_rx) ? (uint32_t)n : sizeof s_rx;
        for (uint32_t i = 0U; i < count; i++) {
            if (ds_parser_push(&s_parser, s_rx[i]) == DS_EV_FRAME) {
                handle_frame(); /* payload is valid until the next push */
            }
        }
    }
}
