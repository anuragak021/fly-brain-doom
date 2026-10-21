/** @file test_scale.c @brief Tests for stage 2, crop and area-average downscale. */
#include <stdint.h>
#include <string.h>

#include "op_scale.h"
#include "test_runner.h"

#define DOOM_PIXELS (OP_DOOM_SRC_W * OP_DOOM_SRC_H)
#define OLED_PIXELS (OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX)

static uint8_t g_src[DOOM_PIXELS];
static uint8_t g_dst[OLED_PIXELS];

static void test_default_crop_geometry(void)
{
    op_scale_cfg_t cfg;
    CHECK_EQ_INT(op_scale_doom_default(&cfg), OP_OK);
    CHECK_EQ_INT(cfg.crop_y, 35);
    CHECK_EQ_INT(cfg.crop_h, 133);
    /* The window stays above the status bar. */
    CHECK_EQ_INT(cfg.crop_y + cfg.crop_h, OP_DOOM_SRC_H - OP_DOOM_STATUSBAR_H); /* bottom aligned */
}

static void test_constant_image_stays_constant(void)
{
    op_scale_cfg_t cfg;
    (void)op_scale_doom_default(&cfg);
    memset(g_src, 173, sizeof g_src);
    CHECK_EQ_INT(op_scale_area(&cfg, g_src, NULL, g_dst), OP_OK);
    for (uint32_t i = 0U; i < OLED_PIXELS; i++) {
        CHECK_EQ_INT(g_dst[i], 173);
    }
}

static void test_integer_ratio_is_block_mean(void)
{
    /* 4x2 -> 2x1: each output is the mean of a 2x2 block. */
    const uint8_t src[8] = {0U, 100U, 10U, 20U, 50U, 250U, 30U, 40U};
    uint8_t dst[2];
    const op_scale_cfg_t cfg = {4U, 2U, 4U, 0U, 0U, 4U, 2U, 2U, 1U};
    CHECK_EQ_INT(op_scale_area(&cfg, src, NULL, dst), OP_OK);
    CHECK_EQ_INT(dst[0], 100); /* (0 + 100 + 50 + 250) / 4 */
    CHECK_EQ_INT(dst[1], 25);  /* (10 + 20 + 30 + 40) / 4 */
}

static void test_fractional_ratio_weights_edges(void)
{
    /* 5 -> 2: output 0 covers src 0, 1 and half of 2; output 1 covers half of 2, 3, 4. */
    const uint8_t src[5] = {0U, 0U, 200U, 100U, 100U};
    uint8_t dst[2];
    const op_scale_cfg_t cfg = {5U, 1U, 5U, 0U, 0U, 5U, 1U, 2U, 1U};
    CHECK_EQ_INT(op_scale_area(&cfg, src, NULL, dst), OP_OK);
    CHECK_EQ_INT(dst[0], 40);  /* (0 + 0 + 100) / 2.5 */
    CHECK_EQ_INT(dst[1], 120); /* (100 + 100 + 100) / 2.5 */
}

static void test_lut_is_applied(void)
{
    uint8_t lut[OP_LUT_ENTRIES];
    for (uint32_t i = 0U; i < OP_LUT_ENTRIES; i++) {
        lut[i] = (uint8_t)(255U - i);
    }
    const uint8_t src[4] = {0U, 0U, 255U, 255U};
    uint8_t dst[2];
    const op_scale_cfg_t cfg = {4U, 1U, 4U, 0U, 0U, 4U, 1U, 2U, 1U};
    CHECK_EQ_INT(op_scale_area(&cfg, src, lut, dst), OP_OK);
    CHECK_EQ_INT(dst[0], 255);
    CHECK_EQ_INT(dst[1], 0);
}

static void test_crop_offset_and_stride(void)
{
    /* 3 wide rows padded to stride 4; crop the right 2x1 of row 1. */
    const uint8_t src[8] = {9U, 9U, 9U, 0U, 9U, 60U, 80U, 0U};
    uint8_t dst[1];
    const op_scale_cfg_t cfg = {3U, 2U, 4U, 1U, 1U, 2U, 1U, 1U, 1U};
    CHECK_EQ_INT(op_scale_area(&cfg, src, NULL, dst), OP_OK);
    CHECK_EQ_INT(dst[0], 70);
}

static void test_mean_is_preserved_on_gradient(void)
{
    op_scale_cfg_t cfg;
    (void)op_scale_doom_default(&cfg);
    uint64_t src_sum = 0U;
    for (uint32_t y = 0U; y < OP_DOOM_SRC_H; y++) {
        for (uint32_t x = 0U; x < OP_DOOM_SRC_W; x++) {
            const uint8_t v = (uint8_t)((x * 255U) / (OP_DOOM_SRC_W - 1U));
            g_src[(y * OP_DOOM_SRC_W) + x] = v;
            if ((y >= cfg.crop_y) && (y < (uint32_t)cfg.crop_y + cfg.crop_h)) {
                src_sum += v;
            }
        }
    }
    CHECK_EQ_INT(op_scale_area(&cfg, g_src, NULL, g_dst), OP_OK);
    uint64_t dst_sum = 0U;
    for (uint32_t i = 0U; i < OLED_PIXELS; i++) {
        dst_sum += g_dst[i];
    }
    const uint64_t src_mean_x1000 = (src_sum * 1000U) / ((uint64_t)cfg.crop_w * cfg.crop_h);
    const uint64_t dst_mean_x1000 = (dst_sum * 1000U) / OLED_PIXELS;
    const uint64_t diff = (src_mean_x1000 > dst_mean_x1000) ? src_mean_x1000 - dst_mean_x1000
                                                            : dst_mean_x1000 - src_mean_x1000;
    CHECK(diff <= 500U); /* within half a gray level (rounding only) */
    for (uint32_t x = 1U; x < OP_OLED_WIDTH_PX; x++) {
        CHECK(g_dst[x] >= g_dst[x - 1U]); /* monotonic left to right */
    }
}

static void test_rejects_bad_geometry(void)
{
    op_scale_cfg_t cfg;
    (void)op_scale_doom_default(&cfg);
    CHECK_EQ_INT(op_scale_area(NULL, g_src, NULL, g_dst), OP_ERR_NULL);
    CHECK_EQ_INT(op_scale_area(&cfg, NULL, NULL, g_dst), OP_ERR_NULL);
    CHECK_EQ_INT(op_scale_area(&cfg, g_src, NULL, NULL), OP_ERR_NULL);

    op_scale_cfg_t bad = cfg;
    bad.dst_w = (uint16_t)(OP_MAX_DST_WIDTH_PX + 1U);
    CHECK_EQ_INT(op_scale_area(&bad, g_src, NULL, g_dst), OP_ERR_SIZE);
    bad = cfg;
    bad.dst_h = 0U;
    CHECK_EQ_INT(op_scale_area(&bad, g_src, NULL, g_dst), OP_ERR_SIZE);
    bad = cfg;
    bad.crop_y = 100U; /* 100 + 133 > 200 */
    CHECK_EQ_INT(op_scale_area(&bad, g_src, NULL, g_dst), OP_ERR_SIZE);
    bad = cfg;
    bad.crop_w = 64U; /* upscale */
    CHECK_EQ_INT(op_scale_area(&bad, g_src, NULL, g_dst), OP_ERR_SIZE);
    bad = cfg;
    bad.src_stride = 100U;
    CHECK_EQ_INT(op_scale_area(&bad, g_src, NULL, g_dst), OP_ERR_SIZE);
}

int main(void)
{
    RUN(test_default_crop_geometry);
    RUN(test_constant_image_stays_constant);
    RUN(test_integer_ratio_is_block_mean);
    RUN(test_fractional_ratio_weights_edges);
    RUN(test_lut_is_applied);
    RUN(test_crop_offset_and_stride);
    RUN(test_mean_is_preserved_on_gradient);
    RUN(test_rejects_bad_geometry);
    return TEST_REPORT();
}
