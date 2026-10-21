/** @file test_dither.c @brief Tests for stage 3, Bayer 4x4 and Floyd-Steinberg dithering. */
#include <stdint.h>
#include <string.h>

#include "op_dither.h"
#include "test_runner.h"

#define W (OP_OLED_WIDTH_PX)
#define H (OP_OLED_HEIGHT_PX)
#define N (W * H)

static uint8_t g_gray[N];
static uint8_t g_bits[N];

static uint32_t count_on(void)
{
    uint32_t on = 0U;
    for (uint32_t i = 0U; i < N; i++) {
        on += g_bits[i];
    }
    return on;
}

static void fill(uint8_t v)
{
    memset(g_gray, v, sizeof g_gray);
}

static void test_extremes_both_modes(void)
{
    for (int m = 0; m < 2; m++) {
        const op_dither_mode_t mode = (op_dither_mode_t)m;
        fill(0U);
        CHECK_EQ_INT(op_dither(mode, g_gray, W, H, g_bits), OP_OK);
        CHECK_EQ_INT(count_on(), 0);
        fill(255U);
        CHECK_EQ_INT(op_dither(mode, g_gray, W, H, g_bits), OP_OK);
        CHECK_EQ_INT(count_on(), N);
    }
}

static void test_bayer_levels_are_exact(void)
{
    /* Gray g lights exactly the thresholds (16k + 8) below g, per 4x4 tile. */
    const struct {
        uint8_t gray;
        uint32_t on_per_16;
    } cases[] = {{8U, 0U}, {9U, 1U}, {128U, 8U}, {200U, 12U}, {248U, 15U}, {249U, 16U}};
    for (size_t i = 0U; i < sizeof cases / sizeof cases[0]; i++) {
        fill(cases[i].gray);
        CHECK_EQ_INT(op_dither_bayer4(g_gray, W, H, g_bits), OP_OK);
        CHECK_EQ_INT(count_on(), (N / 16U) * cases[i].on_per_16);
    }
}

static void test_bayer_is_position_stable(void)
{
    /* Ordered dither depends only on (x, y, gray): same input, same pixel, no neighbour effect.
     * Changing one pixel must change at most that pixel. This is why it does not shimmer. */
    fill(100U);
    CHECK_EQ_INT(op_dither_bayer4(g_gray, W, H, g_bits), OP_OK);
    uint8_t before[N];
    memcpy(before, g_bits, N);
    g_gray[(10U * W) + 10U] = 255U;
    CHECK_EQ_INT(op_dither_bayer4(g_gray, W, H, g_bits), OP_OK);
    uint32_t changed = 0U;
    for (uint32_t i = 0U; i < N; i++) {
        changed += (before[i] != g_bits[i]) ? 1U : 0U;
    }
    CHECK(changed <= 1U);
}

static void test_fs_preserves_mean(void)
{
    const uint8_t levels[] = {1U, 32U, 77U, 128U, 190U, 254U};
    for (size_t i = 0U; i < sizeof levels; i++) {
        fill(levels[i]);
        CHECK_EQ_INT(op_dither_fs(g_gray, W, H, g_bits), OP_OK);
        const uint32_t expected = (N * levels[i] + 127U) / 255U;
        const uint32_t got = count_on();
        const uint32_t diff = (got > expected) ? got - expected : expected - got;
        CHECK(diff <= W); /* only the error left in the last row is lost */
    }
}

static void test_fs_is_deterministic(void)
{
    for (uint32_t i = 0U; i < N; i++) {
        g_gray[i] = (uint8_t)((i * 37U) & 0xFFU);
    }
    CHECK_EQ_INT(op_dither_fs(g_gray, W, H, g_bits), OP_OK);
    uint8_t first[N];
    memcpy(first, g_bits, N);
    CHECK_EQ_INT(op_dither_fs(g_gray, W, H, g_bits), OP_OK);
    CHECK(memcmp(first, g_bits, N) == 0);
}

static void test_odd_small_sizes(void)
{
    const uint8_t gray[3] = {200U, 50U, 128U};
    uint8_t bits[3];
    CHECK_EQ_INT(op_dither_fs(gray, 1U, 3U, bits), OP_OK);
    CHECK_EQ_INT(op_dither_bayer4(gray, 3U, 1U, bits), OP_OK);
}

static void test_rejects_bad_args(void)
{
    CHECK_EQ_INT(op_dither_bayer4(NULL, W, H, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_dither_fs(g_gray, W, H, NULL), OP_ERR_NULL);
    CHECK_EQ_INT(op_dither_fs(g_gray, 0U, H, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_dither_bayer4(g_gray, W + 1U, H, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_dither_fs(g_gray, W, 0U, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_dither((op_dither_mode_t)7, g_gray, W, H, g_bits), OP_ERR_RANGE);
}

int main(void)
{
    RUN(test_extremes_both_modes);
    RUN(test_bayer_levels_are_exact);
    RUN(test_bayer_is_position_stable);
    RUN(test_fs_preserves_mean);
    RUN(test_fs_is_deterministic);
    RUN(test_odd_small_sizes);
    RUN(test_rejects_bad_args);
    return TEST_REPORT();
}
