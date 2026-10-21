/**
 * @file oled_ssd1306.c
 * @brief Thin ESP-IDF I2C driver for a 128x64 SSD1306 OLED.
 */
#include "oled_ssd1306.h"

#include <stddef.h>

#include "driver/i2c_master.h"

#define I2C_TIMEOUT_MS (100)
#define I2C_GLITCH_FILTER_CYCLES (7U)
#define CTRL_CMD_STREAM (0x00U)
#define CTRL_DATA_STREAM (0x40U)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;

/* Init sequence (SSD1306 datasheet section 10 and application note):
 * display off, clock divide, multiplex 64, offset 0, start line 0, charge pump on,
 * horizontal addressing, segment remap + COM scan reversed (verified correct orientation on the
 * home module with tools/oled_probe), COM pins alternative, contrast, resume from RAM,
 * normal (not inverted), column range 0..127, page range 0..7, display on. */
static const uint8_t INIT_SEQ[] = {
    CTRL_CMD_STREAM,
    0xAEU,             /* display off */
    0xD5U, 0x80U,      /* clock divide ratio / oscillator frequency (reset value) */
    0xA8U, 0x3FU,      /* multiplex ratio 64 */
    0xD3U, 0x00U,      /* display offset 0 */
    0x40U,             /* start line 0 */
    0x8DU, 0x14U,      /* charge pump on (module has no external VCC) */
    0x20U, 0x00U,      /* horizontal addressing mode */
    0xA1U,             /* segment remap: column 127 -> SEG0 */
    0xC8U,             /* COM scan direction remapped */
    0xDAU, 0x12U,      /* COM pins: alternative, no left/right remap (128x64 panels) */
    0x81U, 0x7FU,      /* contrast */
    0xD9U, 0xF1U,      /* pre-charge period, internal charge pump */
    0xDBU, 0x40U,      /* VCOMH deselect level */
    0xA4U,             /* display follows RAM */
    0xA6U,             /* normal, not inverted */
    0x21U, 0x00U, 0x7FU, /* column address range 0..127 */
    0x22U, 0x00U, 0x07U, /* page address range 0..7 */
    0xAFU,             /* display on */
};

/* Reset the RAM write pointer to (0, 0) before every frame, so a frame that was cut short by
 * an I2C error cannot leave the next frame shifted. */
static const uint8_t WINDOW_SEQ[] = {CTRL_CMD_STREAM, 0x21U, 0x00U, 0x7FU, 0x22U, 0x00U, 0x07U};

static const uint8_t DATA_CTRL = CTRL_DATA_STREAM;

static oled_status_t add_device(uint32_t freq_hz)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_addr,
        .scl_speed_hz = freq_hz,
    };
    return (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) == ESP_OK) ? OLED_OK
                                                                          : OLED_ERR_BUS;
}

static void release(void)
{
    if (s_dev != NULL) {
        (void)i2c_master_bus_rm_device(s_dev); /* best effort during cleanup */
        s_dev = NULL;
    }
    if (s_bus != NULL) {
        (void)i2c_del_master_bus(s_bus); /* best effort during cleanup */
        s_bus = NULL;
    }
}

oled_status_t oled_init(const oled_cfg_t *cfg)
{
    if ((cfg == NULL) || (cfg->freq_hz == 0U)) {
        return OLED_ERR_ARG;
    }
    if (s_bus != NULL) {
        return OLED_ERR_STATE;
    }
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = cfg->sda,
        .scl_io_num = cfg->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = I2C_GLITCH_FILTER_CYCLES,
        .flags.enable_internal_pullup = cfg->internal_pullup,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        s_bus = NULL;
        return OLED_ERR_BUS;
    }
    s_addr = cfg->addr;
    if (i2c_master_probe(s_bus, s_addr, I2C_TIMEOUT_MS) != ESP_OK) {
        release();
        return OLED_ERR_NOT_FOUND;
    }
    oled_status_t st = add_device(cfg->freq_hz);
    if (st == OLED_OK) {
        st = (i2c_master_transmit(s_dev, INIT_SEQ, sizeof INIT_SEQ, I2C_TIMEOUT_MS) == ESP_OK)
                 ? OLED_OK
                 : OLED_ERR_IO;
    }
    if (st != OLED_OK) {
        release();
    }
    return st;
}

oled_status_t oled_set_freq(uint32_t freq_hz)
{
    if (freq_hz == 0U) {
        return OLED_ERR_ARG;
    }
    if ((s_bus == NULL) || (s_dev == NULL)) {
        return OLED_ERR_STATE;
    }
    if (i2c_master_bus_rm_device(s_dev) != ESP_OK) {
        return OLED_ERR_BUS;
    }
    s_dev = NULL;
    return add_device(freq_hz);
}

oled_status_t oled_write_frame(const uint8_t frame[OLED_FRAME_BYTES])
{
    if (frame == NULL) {
        return OLED_ERR_ARG;
    }
    if (s_dev == NULL) {
        return OLED_ERR_STATE;
    }
    if (i2c_master_transmit(s_dev, WINDOW_SEQ, sizeof WINDOW_SEQ, I2C_TIMEOUT_MS) != ESP_OK) {
        return OLED_ERR_IO;
    }
    /* Control byte and pixel data as one I2C transaction without copying the frame. The cast
     * drops const only because the IDF struct field is non-const; the driver only reads it. */
    i2c_master_transmit_multi_buffer_info_t parts[] = {
        {.write_buffer = (uint8_t *)&DATA_CTRL, .buffer_size = 1U},
        {.write_buffer = (uint8_t *)frame, .buffer_size = OLED_FRAME_BYTES},
    };
    const esp_err_t err =
        i2c_master_multi_buffer_transmit(s_dev, parts, sizeof parts / sizeof parts[0],
                                         I2C_TIMEOUT_MS);
    return (err == ESP_OK) ? OLED_OK : OLED_ERR_IO;
}
