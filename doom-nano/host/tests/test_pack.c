/** @file test_pack.c @brief Tests for stage 4, SSD1306 page packing. */
#include <stdint.h>
#include <string.h>

#include "op_pack.h"
#include "test_runner.h"

#define W (OP_OLED_WIDTH_PX)
#define H (OP_OLED_HEIGHT_PX)

static uint8_t g_bits[W * H];
static uint8_t g_out[OP_OLED_PACKED_BYTES];

static void test_single_pixels_land_in_right_bit(void)
{
    const struct {
        uint32_t x, y, byte_index;
        uint8_t mask;
    } cases[] = {
        {0U, 0U, 0U, 0x01U},                 /* top left: page 0, bit 0 */
        {0U, 7U, 0U, 0x80U},                 /* bottom of page 0 */
        {5U, 8U, W + 5U, 0x01U},             /* first row of page 1 */
        {127U, 63U, (7U * W) + 127U, 0x80U}, /* bottom right */
        {64U, 35U, (4U * W) + 64U, 0x08U},
    };
    for (size_t i = 0U; i < sizeof cases / sizeof cases[0]; i++) {
        memset(g_bits, 0, sizeof g_bits);
        g_bits[(cases[i].y * W) + cases[i].x] = 1U;
        CHECK_EQ_INT(op_pack_ssd1306(g_bits, W, H, g_out, sizeof g_out), OP_OK);
        for (uint32_t b = 0U; b < OP_OLED_PACKED_BYTES; b++) {
            const uint8_t expect = (b == cases[i].byte_index) ? cases[i].mask : 0U;
            CHECK_EQ_INT(g_out[b], expect);
        }
    }
}

static void test_all_on(void)
{
    memset(g_bits, 1, sizeof g_bits);
    CHECK_EQ_INT(op_pack_ssd1306(g_bits, W, H, g_out, sizeof g_out), OP_OK);
    for (uint32_t b = 0U; b < OP_OLED_PACKED_BYTES; b++) {
        CHECK_EQ_INT(g_out[b], 0xFF);
    }
}

static void test_rejects_bad_args(void)
{
    CHECK_EQ_INT(op_pack_ssd1306(NULL, W, H, g_out, sizeof g_out), OP_ERR_NULL);
    CHECK_EQ_INT(op_pack_ssd1306(g_bits, W, H, NULL, sizeof g_out), OP_ERR_NULL);
    CHECK_EQ_INT(op_pack_ssd1306(g_bits, W, 60U, g_out, sizeof g_out), OP_ERR_SIZE);
    CHECK_EQ_INT(op_pack_ssd1306(g_bits, 0U, H, g_out, sizeof g_out), OP_ERR_SIZE);
    CHECK_EQ_INT(op_pack_ssd1306(g_bits, W, H, g_out, sizeof g_out - 1U), OP_ERR_SIZE);
}

int main(void)
{
    RUN(test_single_pixels_land_in_right_bit);
    RUN(test_all_on);
    RUN(test_rejects_bad_args);
    return TEST_REPORT();
}
