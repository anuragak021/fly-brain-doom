/** @file test_mask.c @brief Tests for the sprite and wall masks (change map, window, OLED
 * coverage). */
#include <stdint.h>
#include <string.h>

#include "op_mask.h"
#include "op_scale.h"
#include "test_runner.h"

#define OW (OP_OLED_WIDTH_PX)
#define OH (OP_OLED_HEIGHT_PX)
#define ON (OW * OH)
#define FB_BYTES (OP_DOOM_SRC_W * OP_DOOM_SRC_H)

static uint8_t g_before[OP_MASK_SRC_BYTES];
static uint8_t g_after[OP_MASK_SRC_BYTES];
static uint8_t g_cov[ON];
static uint8_t g_ref[ON];
static uint8_t g_fb[FB_BYTES];
static uint8_t g_big_before[OP_MASK_MAX_SRC_BYTES];

static void test_diff_marks_changed_bytes(void)
{
    const uint8_t after[5] = {1U, 2U, 3U, 4U, 5U};
    uint8_t before[5] = {1U, 9U, 3U, 0U, 5U};
    CHECK_EQ_INT(op_mask_diff(before, after, sizeof before), OP_OK);
    const uint8_t expect[5] = {0U, 1U, 0U, 1U, 0U};
    CHECK(memcmp(before, expect, sizeof expect) == 0);
}

static void test_diff_rejects_bad_args(void)
{
    uint8_t a[1] = {0U};
    CHECK_EQ_INT(op_mask_diff(NULL, a, 1U), OP_ERR_NULL);
    CHECK_EQ_INT(op_mask_diff(a, NULL, 1U), OP_ERR_NULL);
    CHECK_EQ_INT(op_mask_diff(a, a, 0U), OP_ERR_SIZE);
}

static void test_coverage_empty_and_full(void)
{
    memset(g_before, 0, sizeof g_before);
    CHECK_EQ_INT(op_mask_coverage(g_before, g_cov), OP_OK);
    for (uint32_t i = 0U; i < ON; i++) {
        CHECK_EQ_INT(g_cov[i], 0);
    }
    memset(g_before, 1, sizeof g_before);
    CHECK_EQ_INT(op_mask_coverage(g_before, g_cov), OP_OK);
    for (uint32_t i = 0U; i < ON; i++) {
        CHECK_EQ_INT(g_cov[i], OP_MASK_COVERAGE_FULL);
    }
}

static void test_coverage_lines_up_with_gray_frame(void)
{
    /* A "sprite" rectangle drawn into a full framebuffer. The coverage of its change map must
     * equal what op_scale_area produces for the same framebuffer through the Doom crop, with a
     * LUT that maps the sprite colour to full white: same window, same weights. */
    const uint8_t wall = 3U;
    const uint8_t sprite = 7U;
    memset(g_fb, wall, sizeof g_fb);
    memcpy(g_before, &g_fb[OP_MASK_SRC_OFFSET], OP_MASK_SRC_BYTES);
    for (uint32_t y = 90U; y < 150U; y++) {
        for (uint32_t x = 101U; x < 177U; x++) { /* odd edges: partial coverage at borders */
            g_fb[(y * OP_DOOM_SRC_W) + x] = sprite;
        }
    }
    memcpy(g_after, &g_fb[OP_MASK_SRC_OFFSET], OP_MASK_SRC_BYTES);
    CHECK_EQ_INT(op_mask_diff(g_before, g_after, OP_MASK_SRC_BYTES), OP_OK);
    CHECK_EQ_INT(op_mask_coverage(g_before, g_cov), OP_OK);

    op_scale_cfg_t cfg;
    CHECK_EQ_INT(op_scale_doom_default(&cfg), OP_OK);
    uint8_t lut[OP_LUT_ENTRIES];
    memset(lut, 0, sizeof lut);
    lut[sprite] = (uint8_t)OP_MASK_COVERAGE_FULL;
    CHECK_EQ_INT(op_scale_area(&cfg, g_fb, lut, g_ref), OP_OK);
    CHECK(memcmp(g_cov, g_ref, ON) == 0);

    uint32_t partial = 0U;
    uint32_t full = 0U;
    for (uint32_t i = 0U; i < ON; i++) {
        partial += ((g_cov[i] > 0U) && (g_cov[i] < OP_MASK_COVERAGE_FULL)) ? 1U : 0U;
        full += (g_cov[i] == OP_MASK_COVERAGE_FULL) ? 1U : 0U;
    }
    CHECK(full > 0U);
    CHECK(partial > 0U);
}

static void test_coverage_rejects_null(void)
{
    CHECK_EQ_INT(op_mask_coverage(NULL, g_cov), OP_ERR_NULL);
    CHECK_EQ_INT(op_mask_coverage(g_before, NULL), OP_ERR_NULL);
}

static void test_window_of_default_and_hud_views(void)
{
    op_scale_cfg_t cfg;
    size_t offset = 0U;
    size_t bytes = 0U;
    CHECK_EQ_INT(op_scale_doom_default(&cfg), OP_OK);
    CHECK_EQ_INT(op_mask_window(&cfg, &offset, &bytes), OP_OK);
    CHECK_EQ_INT(offset, OP_MASK_SRC_OFFSET);
    CHECK_EQ_INT(bytes, OP_MASK_SRC_BYTES);
    CHECK_EQ_INT(op_scale_doom_hud_view(&cfg), OP_OK);
    CHECK_EQ_INT(op_mask_window(&cfg, &offset, &bytes), OP_OK);
    CHECK_EQ_INT(offset, OP_DOOM_SRC_W * OP_DOOM_HUD_CROP_Y);
    CHECK_EQ_INT(bytes, OP_DOOM_SRC_W * OP_DOOM_HUD_CROP_H);
    CHECK(bytes <= OP_MASK_MAX_SRC_BYTES);
}

static void test_window_rejects_partial_rows(void)
{
    op_scale_cfg_t cfg;
    size_t offset = 0U;
    size_t bytes = 0U;
    CHECK_EQ_INT(op_mask_window(NULL, &offset, &bytes), OP_ERR_NULL);
    CHECK_EQ_INT(op_scale_doom_default(&cfg), OP_OK);
    cfg.crop_x = 1U;
    CHECK_EQ_INT(op_mask_window(&cfg, &offset, &bytes), OP_ERR_SIZE);
    CHECK_EQ_INT(op_scale_doom_default(&cfg), OP_OK);
    cfg.crop_y = OP_DOOM_SRC_H; /* window runs past the frame */
    CHECK_EQ_INT(op_mask_window(&cfg, &offset, &bytes), OP_ERR_SIZE);
    CHECK_EQ_INT(op_scale_doom_default(&cfg), OP_OK);
    cfg.crop_y = 0U;
    cfg.crop_h = OP_DOOM_SRC_H; /* the status bar too: larger than the max window */
    CHECK_EQ_INT(op_mask_window(&cfg, &offset, &bytes), OP_ERR_SIZE);
}

/** Sprite rectangle drawn into g_fb over a plain wall colour (odd edges: partial coverage). */
static void draw_rect(uint8_t colour, uint32_t y0, uint32_t y1, uint32_t x0, uint32_t x1)
{
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            g_fb[(y * OP_DOOM_SRC_W) + x] = colour;
        }
    }
}

static void test_coverage_cfg_lines_up_on_hud_view(void)
{
    /* Same check as above for the nano crop: map built from the HUD view's window. */
    const uint8_t sprite = 7U;
    op_scale_cfg_t cfg;
    size_t offset = 0U;
    size_t bytes = 0U;
    CHECK_EQ_INT(op_scale_doom_hud_view(&cfg), OP_OK);
    CHECK_EQ_INT(op_mask_window(&cfg, &offset, &bytes), OP_OK);
    memset(g_fb, 3, sizeof g_fb);
    memcpy(g_big_before, &g_fb[offset], bytes);
    draw_rect(sprite, 60U, 165U, 33U, 97U);
    CHECK_EQ_INT(op_mask_diff(g_big_before, &g_fb[offset], bytes), OP_OK);
    CHECK_EQ_INT(op_mask_coverage_cfg(&cfg, g_big_before, g_cov), OP_OK);
    uint8_t lut[OP_LUT_ENTRIES];
    memset(lut, 0, sizeof lut);
    lut[sprite] = (uint8_t)OP_MASK_COVERAGE_FULL;
    CHECK_EQ_INT(op_scale_area(&cfg, g_fb, lut, g_ref), OP_OK);
    CHECK(memcmp(g_cov, g_ref, (size_t)cfg.dst_w * cfg.dst_h) == 0);
    CHECK_EQ_INT(op_mask_coverage_cfg(NULL, g_big_before, g_cov), OP_ERR_NULL);
}

static void test_wall_coverage_counts_non_marker(void)
{
    const uint8_t marker = (uint8_t)OP_MASK_WALL_MARKER;
    op_scale_cfg_t cfg;
    CHECK_EQ_INT(op_scale_doom_hud_view(&cfg), OP_OK);
    const size_t n = (size_t)cfg.dst_w * cfg.dst_h;

    memset(g_fb, marker, sizeof g_fb);
    CHECK_EQ_INT(op_mask_wall_coverage(&cfg, g_fb, marker, g_cov), OP_OK);
    for (size_t i = 0U; i < n; i++) {
        CHECK_EQ_INT(g_cov[i], 0);
    }
    /* Walls in two colours (one of them index 0) both count; the marker never does. */
    draw_rect(0U, 51U, 168U, 0U, 160U);
    draw_rect(96U, 51U, 168U, 160U, 320U);
    CHECK_EQ_INT(op_mask_wall_coverage(&cfg, g_fb, marker, g_cov), OP_OK);
    for (size_t i = 0U; i < n; i++) {
        CHECK_EQ_INT(g_cov[i], OP_MASK_COVERAGE_FULL);
    }
    CHECK_EQ_INT(op_mask_wall_coverage(&cfg, NULL, marker, g_cov), OP_ERR_NULL);
}

static void test_ids_take_the_front_most_id(void)
{
    op_scale_cfg_t cfg;
    size_t offset = 0U;
    size_t bytes = 0U;
    CHECK_EQ_INT(op_scale_doom_hud_view(&cfg), OP_OK);
    CHECK_EQ_INT(op_mask_window(&cfg, &offset, &bytes), OP_OK);
    const size_t n = (size_t)cfg.dst_w * cfg.dst_h;
    memset(g_big_before, 0, bytes);
    CHECK_EQ_INT(op_mask_ids(&cfg, g_big_before, g_cov), OP_OK);
    for (size_t i = 0U; i < n; i++) {
        CHECK_EQ_INT(g_cov[i], 0);
    }
    /* 320 -> 128 columns: output 0 reads source 0..2 and output 1 reads 2..4, so source column
     * 2 shows in both; the larger id wins over a smaller one in the same cell. */
    g_big_before[1] = 3U;
    g_big_before[2] = 9U;
    g_big_before[bytes - 1U] = 200U;
    CHECK_EQ_INT(op_mask_ids(&cfg, g_big_before, g_cov), OP_OK);
    CHECK_EQ_INT(g_cov[0], 9);
    CHECK_EQ_INT(g_cov[1], 9);
    CHECK_EQ_INT(g_cov[2], 0);
    CHECK_EQ_INT(g_cov[cfg.dst_w], 0); /* second output row starts below source row 0 */
    CHECK_EQ_INT(g_cov[n - 1U], 200);

    CHECK_EQ_INT(op_mask_ids(NULL, g_big_before, g_cov), OP_ERR_NULL);
    CHECK_EQ_INT(op_mask_ids(&cfg, NULL, g_cov), OP_ERR_NULL);
    CHECK_EQ_INT(op_mask_ids(&cfg, g_big_before, NULL), OP_ERR_NULL);
    cfg.dst_h = (uint16_t)(cfg.crop_h + 1U); /* upscaling is not supported */
    CHECK_EQ_INT(op_mask_ids(&cfg, g_big_before, g_cov), OP_ERR_SIZE);
    cfg.dst_h = 0U;
    CHECK_EQ_INT(op_mask_ids(&cfg, g_big_before, g_cov), OP_ERR_SIZE);
}

int main(void)
{
    RUN(test_diff_marks_changed_bytes);
    RUN(test_diff_rejects_bad_args);
    RUN(test_coverage_empty_and_full);
    RUN(test_coverage_lines_up_with_gray_frame);
    RUN(test_coverage_rejects_null);
    RUN(test_window_of_default_and_hud_views);
    RUN(test_window_rejects_partial_rows);
    RUN(test_coverage_cfg_lines_up_on_hud_view);
    RUN(test_wall_coverage_counts_non_marker);
    RUN(test_ids_take_the_front_most_id);
    return TEST_REPORT();
}
