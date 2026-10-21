/**
 * @file oled_probe.c
 * @brief Phase 0 bring-up probe: report chip/PSRAM, scan I2C, identify the OLED controller.
 *
 * Throwaway diagnostic, not part of the game image. It draws a pattern in page addressing
 * mode (supported by both SSD1306 and SH1106) so the controller can be told apart by eye:
 *   SSD1306: vertical lines exactly on the left and right edges, clean frame.
 *   SH1106:  left line missing (RAM column 0 is off screen), right line 2 px in from the edge,
 *            noise in the 2 rightmost columns (RAM 128..131 never written).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled_probe";

enum {
    I2C_FREQ_HZ = 400000,
    I2C_TIMEOUT_MS = 50,
    OLED_WIDTH_PX = 128,
    OLED_PAGES = 8,
    ADDR_FIRST = 0x08,
    ADDR_LAST = 0x77,
    CTRL_CMD = 0x00,  /* control byte: command stream */
    CTRL_DATA = 0x40, /* control byte: data stream */
};

typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
} pin_pair_t;

/* Lyrics player wiring first, then common S3 choices that avoid strapping and flash/PSRAM pins. */
static const pin_pair_t PIN_PAIRS[] = {
    {GPIO_NUM_8, GPIO_NUM_9},
    {GPIO_NUM_4, GPIO_NUM_5},
    {GPIO_NUM_17, GPIO_NUM_18},
    {GPIO_NUM_1, GPIO_NUM_2},
    {GPIO_NUM_41, GPIO_NUM_42},
};

static void report_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_bytes = 0U;
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
        flash_bytes = 0U;
    }
    ESP_LOGI(TAG, "chip model %d rev %d cores %d, flash %lu MB", (int)info.model, (int)info.revision,
             (int)info.cores, (unsigned long)(flash_bytes / (1024UL * 1024UL)));
    ESP_LOGI(TAG, "PSRAM initialised: %s, size %u bytes", esp_psram_is_initialized() ? "yes" : "no",
             (unsigned)esp_psram_get_size());
}

static esp_err_t send(i2c_master_dev_handle_t dev, uint8_t ctrl, const uint8_t *buf, size_t len)
{
    uint8_t tx[1 + OLED_WIDTH_PX];
    if (len > OLED_WIDTH_PX) {
        return ESP_ERR_INVALID_SIZE;
    }
    tx[0] = ctrl;
    for (size_t i = 0; i < len; i++) {
        tx[1 + i] = buf[i];
    }
    return i2c_master_transmit(dev, tx, len + 1U, I2C_TIMEOUT_MS);
}

static esp_err_t draw_pattern(i2c_master_dev_handle_t dev)
{
    /* Init sequence valid on both controllers; 0x8D/0x14 is the SSD1306 charge pump (SH1106
     * treats it as a no-op pair), 0xAD/0x8B is the SH1106 DC-DC (ignored by SSD1306). */
    static const uint8_t init[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
                                   0xAD, 0x8B, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0x7F, 0xA4, 0xA6};
    esp_err_t err = send(dev, CTRL_CMD, init, sizeof init);
    if (err != ESP_OK) {
        return err;
    }
    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        uint8_t row[OLED_WIDTH_PX];
        for (uint32_t x = 0; x < OLED_WIDTH_PX; x++) {
            uint8_t v = 0x00;
            if ((x == 0U) || (x == OLED_WIDTH_PX - 1U)) {
                v = 0xFF; /* left and right edge lines */
            } else if (page == 0U) {
                v = 0x01; /* top line */
            } else if (page == OLED_PAGES - 1U) {
                v = 0x80; /* bottom line */
            } else if ((x >= 8U) && (x < 24U) && (page == 3U)) {
                v = 0xFF; /* solid block marks the top-left quadrant (orientation check) */
            }
            row[x] = v;
        }
        const uint8_t addr_cmd[] = {(uint8_t)(0xB0U + page), 0x00, 0x10};
        err = send(dev, CTRL_CMD, addr_cmd, sizeof addr_cmd);
        if (err == ESP_OK) {
            err = send(dev, CTRL_DATA, row, sizeof row);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    static const uint8_t on[] = {0xAF};
    return send(dev, CTRL_CMD, on, sizeof on);
}

static bool probe_pair(const pin_pair_t *pins)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = pins->sda,
        .scl_io_num = pins->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "SDA=%d SCL=%d: bus init failed", pins->sda, pins->scl);
        return false;
    }
    uint16_t oled_addr = 0U;
    uint32_t found = 0U;
    for (uint16_t a = ADDR_FIRST; a <= ADDR_LAST; a++) {
        if (i2c_master_probe(bus, a, I2C_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "SDA=%d SCL=%d: device at 0x%02X", pins->sda, pins->scl, a);
            found++;
            if ((a == 0x3CU) || (a == 0x3DU)) {
                oled_addr = a;
            }
        }
    }
    if (found == 0U) {
        ESP_LOGI(TAG, "SDA=%d SCL=%d: nothing", pins->sda, pins->scl);
    }
    if (oled_addr != 0U) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = oled_addr,
            .scl_speed_hz = I2C_FREQ_HZ,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            uint8_t status = 0U;
            esp_err_t err = i2c_master_receive(dev, &status, 1U, I2C_TIMEOUT_MS);
            ESP_LOGI(TAG, "status byte read: %s, value 0x%02X", esp_err_to_name(err), status);
            err = draw_pattern(dev);
            ESP_LOGI(TAG, "test pattern: %s", esp_err_to_name(err));
            (void)i2c_master_bus_rm_device(dev);
        }
    }
    (void)i2c_del_master_bus(bus);
    return oled_addr != 0U;
}

void app_main(void)
{
    report_chip();
    for (size_t i = 0; i < sizeof PIN_PAIRS / sizeof PIN_PAIRS[0]; i++) {
        if (probe_pair(&PIN_PAIRS[i])) {
            ESP_LOGI(TAG, "OLED found; pattern left on screen");
            break;
        }
    }
    ESP_LOGI(TAG, "probe done");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
