/**
 * @file app_main.c
 * @brief doom-esp32 entry point: board configuration, then hand over to the engine.
 */
#include "doom_platform.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled_ssd1306.h"
#include "sdkconfig.h"
#if defined(CONFIG_DOOM_BRAIN_PLAY)
#include "bl_client.h"
#endif

/* Home board pin map (README "Pin map"): GPIO8/9 are not strapping, flash, PSRAM or USB pins. */
#define PIN_OLED_SDA (GPIO_NUM_8)
#define PIN_OLED_SCL (GPIO_NUM_9)

static const char *TAG = "app";

void app_main(void)
{
    const dp_cfg_t cfg = {
        .oled_sda = PIN_OLED_SDA,
        .oled_scl = PIN_OLED_SCL,
        .oled_addr = (uint8_t)OLED_I2C_ADDR_DEFAULT,
        .oled_freq_hz = OLED_I2C_FREQ_STD_HZ,
    };
    const dp_status_t st = doom_platform_init(&cfg);
    if (st != DP_OK) {
        ESP_LOGE(TAG, "platform init failed (%d); see the log above. Halting.", (int)st);
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
#if defined(CONFIG_DOOM_BRAIN_PLAY)
    /* Link to the STM32 brain (Nucleo USART1): ESP TX -> PB7 (D0), ESP RX <- PB6 (D1), GND. */
    const bl_cfg_t brain = {
        .tx = (gpio_num_t)CONFIG_DOOM_BRAIN_TX_GPIO,
        .rx = (gpio_num_t)CONFIG_DOOM_BRAIN_RX_GPIO,
        .baud = (uint32_t)CONFIG_DOOM_BRAIN_BAUD,
    };
    if (bl_client_start(&brain) != BL_OK) {
        ESP_LOGE(TAG, "brain link not started; the player will stand still");
    }
#endif
    doom_platform_run(); /* does not return */
}
