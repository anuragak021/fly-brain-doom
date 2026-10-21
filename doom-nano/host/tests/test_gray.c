/** @file test_gray.c @brief Tests for stage 1, palette to gray LUT. */
#include <stdint.h>
#include <string.h>

#include "op_gray.h"
#include "test_runner.h"

static void test_luma_endpoints(void)
{
    CHECK_EQ_INT(op_gray_luma(0U, 0U, 0U), 0);
    CHECK_EQ_INT(op_gray_luma(255U, 255U, 255U), 255);
    /* Pure primaries follow the Rec.601 weights 77/150/29 out of 256. */
    CHECK_EQ_INT(op_gray_luma(255U, 0U, 0U), 77);
    CHECK_EQ_INT(op_gray_luma(0U, 255U, 0U), 149);
    CHECK_EQ_INT(op_gray_luma(0U, 0U, 255U), 29);
}

static void test_luma_gray_is_identity(void)
{
    for (uint32_t v = 0U; v < 256U; v++) {
        CHECK_EQ_INT(op_gray_luma((uint8_t)v, (uint8_t)v, (uint8_t)v), v);
    }
}

static void make_gray_ramp_palette(uint8_t pal[OP_PALETTE_BYTES])
{
    for (uint32_t i = 0U; i < OP_LUT_ENTRIES; i++) {
        pal[3U * i] = (uint8_t)i;
        pal[(3U * i) + 1U] = (uint8_t)i;
        pal[(3U * i) + 2U] = (uint8_t)i;
    }
}

static const op_gray_cfg_t IDENTITY = {0U, 255U, OP_GAMMA_X100_IDENTITY};

static void test_lut_identity(void)
{
    uint8_t pal[OP_PALETTE_BYTES];
    uint8_t lut[OP_LUT_ENTRIES];
    make_gray_ramp_palette(pal);
    CHECK_EQ_INT(op_gray_build_lut(pal, &IDENTITY, lut), OP_OK);
    for (uint32_t i = 0U; i < OP_LUT_ENTRIES; i++) {
        CHECK_EQ_INT(lut[i], i);
    }
}

static void test_lut_gamma_brightens_and_keeps_ends(void)
{
    uint8_t pal[OP_PALETTE_BYTES];
    uint8_t lut[OP_LUT_ENTRIES];
    const op_gray_cfg_t cfg = {0U, 255U, 200U};
    make_gray_ramp_palette(pal);
    CHECK_EQ_INT(op_gray_build_lut(pal, &cfg, lut), OP_OK);
    CHECK_EQ_INT(lut[0], 0);
    CHECK_EQ_INT(lut[255], 255);
    /* 255 * (64/255)^0.5 = 127.75 -> 128 */
    CHECK_EQ_INT(lut[64], 128);
    for (uint32_t i = 1U; i < OP_LUT_ENTRIES; i++) {
        CHECK(lut[i] >= lut[i - 1U]); /* monotonic */
        CHECK(lut[i] >= i);           /* gamma > 1 never darkens */
    }
}

static void test_lut_levels_stretch_and_clip(void)
{
    uint8_t pal[OP_PALETTE_BYTES];
    uint8_t lut[OP_LUT_ENTRIES];
    op_gray_cfg_t dflt;
    CHECK_EQ_INT(op_gray_cfg_default(&dflt), OP_OK);
    CHECK_EQ_INT(dflt.black, OP_GRAY_DEFAULT_BLACK);
    CHECK_EQ_INT(dflt.white, OP_GRAY_DEFAULT_WHITE);
    CHECK_EQ_INT(dflt.gamma_x100, OP_GAMMA_X100_IDENTITY);
    const op_gray_cfg_t cfg = {8U, 128U, OP_GAMMA_X100_IDENTITY};
    make_gray_ramp_palette(pal);
    CHECK_EQ_INT(op_gray_build_lut(pal, &cfg, lut), OP_OK);
    CHECK_EQ_INT(lut[0], 0);
    CHECK_EQ_INT(lut[8], 0);     /* at black point */
    CHECK_EQ_INT(lut[68], 128);  /* midpoint: (68 - 8) * 255 / 120 = 127.5 -> 128 */
    CHECK_EQ_INT(lut[128], 255); /* at white point */
    CHECK_EQ_INT(lut[200], 255); /* clipped */
    for (uint32_t i = 1U; i < OP_LUT_ENTRIES; i++) {
        CHECK(lut[i] >= lut[i - 1U]);
    }
}

static void test_lut_rejects_bad_args(void)
{
    uint8_t pal[OP_PALETTE_BYTES];
    uint8_t lut[OP_LUT_ENTRIES];
    memset(pal, 0, sizeof pal);
    const op_gray_cfg_t low_gamma = {0U, 255U, OP_GAMMA_X100_MIN - 1U};
    const op_gray_cfg_t high_gamma = {0U, 255U, OP_GAMMA_X100_MAX + 1U};
    const op_gray_cfg_t inverted = {100U, 100U, OP_GAMMA_X100_IDENTITY};
    CHECK_EQ_INT(op_gray_build_lut(NULL, &IDENTITY, lut), OP_ERR_NULL);
    CHECK_EQ_INT(op_gray_build_lut(pal, NULL, lut), OP_ERR_NULL);
    CHECK_EQ_INT(op_gray_build_lut(pal, &IDENTITY, NULL), OP_ERR_NULL);
    CHECK_EQ_INT(op_gray_build_lut(pal, &low_gamma, lut), OP_ERR_RANGE);
    CHECK_EQ_INT(op_gray_build_lut(pal, &high_gamma, lut), OP_ERR_RANGE);
    CHECK_EQ_INT(op_gray_build_lut(pal, &inverted, lut), OP_ERR_RANGE);
    CHECK_EQ_INT(op_gray_cfg_default(NULL), OP_ERR_NULL);
}

int main(void)
{
    RUN(test_luma_endpoints);
    RUN(test_luma_gray_is_identity);
    RUN(test_lut_identity);
    RUN(test_lut_levels_stretch_and_clip);
    RUN(test_lut_gamma_brightens_and_keeps_ends);
    RUN(test_lut_rejects_bad_args);
    return TEST_REPORT();
}
