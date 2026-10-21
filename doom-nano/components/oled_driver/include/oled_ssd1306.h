/**
 * @file oled_ssd1306.h
 * @brief Thin ESP-IDF I2C driver for a 128x64 SSD1306 OLED (hardware access only, no pixels).
 *
 * The panel is put in horizontal addressing mode once, so a full frame is a single I2C write of
 * a control byte plus the 1024 page-format bytes produced by op_pack_ssd1306().
 */
#ifndef OLED_SSD1306_H
#define OLED_SSD1306_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"

#define OLED_I2C_ADDR_DEFAULT (0x3CU)
#define OLED_I2C_FREQ_STD_HZ (400000U)   /**< SSD1306 datasheet fast mode maximum. */
#define OLED_I2C_FREQ_FAST_HZ (1000000U) /**< Out of spec overclock, for measurement only. */
#define OLED_FRAME_BYTES (1024U)         /**< 128 x 64 / 8 */

/** Driver status. */
typedef enum {
    OLED_OK = 0,
    OLED_ERR_ARG = 1,       /**< NULL pointer or bad parameter. */
    OLED_ERR_STATE = 2,     /**< Not initialised, or already initialised. */
    OLED_ERR_BUS = 3,       /**< I2C bus or device creation failed. */
    OLED_ERR_NOT_FOUND = 4, /**< No ACK at the configured address. */
    OLED_ERR_IO = 5,        /**< An I2C transfer failed. */
} oled_status_t;

/** Wiring and bus settings. */
typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
    uint8_t addr;          /**< 7 bit address, usually 0x3C. */
    uint32_t freq_hz;      /**< SCL frequency. */
    bool internal_pullup;  /**< Enable the ESP32 internal pull-ups (weak, ~45 kOhm). */
} oled_cfg_t;

/**
 * @brief Create the I2C bus, check the panel answers, and send the init sequence.
 * @param cfg Wiring and bus settings.
 * @return OLED_OK, or an error; on error no bus is left allocated.
 * @note Side effects: claims I2C port 0 and the two GPIOs; turns the display on (blank).
 */
oled_status_t oled_init(const oled_cfg_t *cfg);

/**
 * @brief Change the SCL frequency of the already initialised panel.
 * @param freq_hz New frequency in Hz (> 0).
 * @return OLED_OK, OLED_ERR_STATE if not initialised, OLED_ERR_ARG or OLED_ERR_BUS.
 */
oled_status_t oled_set_freq(uint32_t freq_hz);

/**
 * @brief Write one full frame (1024 bytes, SSD1306 page format) to the panel.
 * @param frame Page-format bytes; must stay valid during the call (blocking transfer).
 * @return OLED_OK, OLED_ERR_ARG, OLED_ERR_STATE or OLED_ERR_IO.
 */
oled_status_t oled_write_frame(const uint8_t frame[OLED_FRAME_BYTES]);

#endif /* OLED_SSD1306_H */
