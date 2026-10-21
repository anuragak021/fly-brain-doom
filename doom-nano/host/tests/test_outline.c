/** @file test_outline.c @brief Tests for the outline and layers display styles. */
#include <stdint.h>
#include <string.h>

#include "op_dither.h"
#include "op_outline.h"
#include "test_runner.h"

#define W (OP_OLED_WIDTH_PX)
#define H (OP_OLED_HEIGHT_PX)
#define N (W * H)

static uint8_t g_gray[N];
static uint8_t g_bits[N];
static uint8_t g_ref[N];
static uint8_t g_cov[N];
static uint8_t g_level[N];

static void test_flat_image_is_dimmed_bayer_without_edges(void)
{
    op_outline_cfg_t cfg;
    CHECK_EQ_INT(op_outline_cfg_default(&cfg), OP_OK);
    memset(g_gray, 200, sizeof g_gray);
    CHECK_EQ_INT(op_outline(g_gray, W, H, &cfg, g_bits), OP_OK);
    /* No gradient anywhere, so the result is exactly Bayer of gray / 2. */
    uint8_t half[N];
    memset(half, 100, sizeof half);
    CHECK_EQ_INT(op_dither_bayer4(half, W, H, g_ref), OP_OK);
    CHECK(memcmp(g_bits, g_ref, N) == 0);
}

static void test_vertical_step_gets_outlined(void)
{
    op_outline_cfg_t cfg;
    (void)op_outline_cfg_default(&cfg);
    /* Black left half, white right half: a strong vertical edge at x = 64. */
    for (uint32_t y = 0U; y < H; y++) {
        for (uint32_t x = 0U; x < W; x++) {
            g_gray[(y * W) + x] = (x < 64U) ? 0U : 255U;
        }
    }
    CHECK_EQ_INT(op_outline(g_gray, W, H, &cfg, g_bits), OP_OK);
    for (uint32_t y = 0U; y < H; y++) {
        CHECK_EQ_INT(g_bits[(y * W) + 63U], 1); /* edge column forced on in every row */
        CHECK_EQ_INT(g_bits[(y * W) + 10U], 0); /* black, far from the edge: off */
    }
}

static void test_fine_texture_is_not_outlined(void)
{
    /* 1 px checkerboard of +-20 around mid gray: the blur removes it, so no edges fire. */
    op_outline_cfg_t cfg;
    (void)op_outline_cfg_default(&cfg);
    for (uint32_t i = 0U; i < N; i++) {
        const uint32_t x = i % W;
        const uint32_t y = i / W;
        g_gray[i] = (((x + y) & 1U) != 0U) ? 148U : 108U;
    }
    CHECK_EQ_INT(op_outline(g_gray, W, H, &cfg, g_bits), OP_OK);
    uint32_t on = 0U;
    for (uint32_t i = 0U; i < N; i++) {
        on += g_bits[i];
    }
    /* Only the dimmed fill (about 64/255 -> 4 of 16) should be lit, far below "all edges". */
    CHECK(on < (N / 2U));
}

static void test_rejects_bad_args(void)
{
    op_outline_cfg_t cfg;
    (void)op_outline_cfg_default(&cfg);
    CHECK_EQ_INT(op_outline(NULL, W, H, &cfg, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_outline(g_gray, W, H, NULL, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_outline(g_gray, W, H, &cfg, NULL), OP_ERR_NULL);
    CHECK_EQ_INT(op_outline(g_gray, W + 1U, H, &cfg, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_outline(g_gray, W, H + 1U, &cfg, g_bits), OP_ERR_SIZE);
    cfg.fill_shift = 8U;
    CHECK_EQ_INT(op_outline(g_gray, W, H, &cfg, g_bits), OP_ERR_RANGE);
    CHECK_EQ_INT(op_outline_cfg_default(NULL), OP_ERR_NULL);
}

static void test_tiny_image(void)
{
    op_outline_cfg_t cfg;
    (void)op_outline_cfg_default(&cfg);
    const uint8_t gray[2] = {0U, 255U};
    uint8_t bits[2];
    CHECK_EQ_INT(op_outline(gray, 2U, 1U, &cfg, bits), OP_OK);
}

/* ---- layers style ---- */

static void test_layers_no_sprites_flat_is_dim_bayer(void)
{
    op_layers_cfg_t cfg;
    CHECK_EQ_INT(op_layers_cfg_default(&cfg), OP_OK);
    memset(g_gray, 200, sizeof g_gray);
    memset(g_cov, 0, sizeof g_cov);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_OK);
    memset(g_level, 200 >> OP_LAYERS_DEFAULT_BG_SHIFT, sizeof g_level);
    CHECK_EQ_INT(op_dither_bayer4(g_level, W, H, g_ref), OP_OK);
    CHECK(memcmp(g_bits, g_ref, N) == 0);
}

static void test_layers_all_sprite_flat_is_boosted_bayer(void)
{
    op_layers_cfg_t cfg;
    (void)op_layers_cfg_default(&cfg);
    memset(g_gray, 100, sizeof g_gray);
    memset(g_cov, 255, sizeof g_cov);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_OK);
    /* 100 * 19 / 16 + 70 = 188 */
    memset(g_level, 188, sizeof g_level);
    CHECK_EQ_INT(op_dither_bayer4(g_level, W, H, g_ref), OP_OK);
    CHECK(memcmp(g_bits, g_ref, N) == 0);
}

static void test_layers_sprite_fill_saturates(void)
{
    op_layers_cfg_t cfg;
    (void)op_layers_cfg_default(&cfg);
    memset(g_gray, 250, sizeof g_gray); /* 250 * 19 / 16 + 70 > 255: clamped, all on */
    memset(g_cov, 255, sizeof g_cov);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_OK);
    uint32_t on = 0U;
    for (uint32_t i = 0U; i < N; i++) {
        on += g_bits[i];
    }
    CHECK_EQ_INT(on, N);
}

/* Sprite square [40, 60) x [20, 40) on a bright flat level. */
#define SQ_X0 (40U)
#define SQ_X1 (60U)
#define SQ_Y0 (20U)
#define SQ_Y1 (40U)

static void make_square_sprite(uint8_t level_gray, uint8_t sprite_gray)
{
    for (uint32_t y = 0U; y < H; y++) {
        for (uint32_t x = 0U; x < W; x++) {
            const int in = (x >= SQ_X0) && (x < SQ_X1) && (y >= SQ_Y0) && (y < SQ_Y1);
            g_cov[(y * W) + x] = in ? 255U : 0U;
            g_gray[(y * W) + x] = in ? sprite_gray : level_gray;
        }
    }
}

static void test_layers_sprite_gets_black_halo(void)
{
    op_layers_cfg_t cfg;
    (void)op_layers_cfg_default(&cfg);
    /* Same gray inside and out: no gradient, so only the halo can separate them. Full white
     * level so its fill (255 >> 2 = 63) would light some halo pixels if they were not forced. */
    make_square_sprite(255U, 255U);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_OK);
    for (uint32_t x = SQ_X0 - 1U; x <= SQ_X1; x++) {
        CHECK_EQ_INT(g_bits[((SQ_Y0 - 1U) * W) + x], 0); /* row above */
        CHECK_EQ_INT(g_bits[(SQ_Y1 * W) + x], 0);        /* row below */
    }
    for (uint32_t y = SQ_Y0; y < SQ_Y1; y++) {
        CHECK_EQ_INT(g_bits[(y * W) + SQ_X0 - 1U], 0); /* column left */
        CHECK_EQ_INT(g_bits[(y * W) + SQ_X1], 0);      /* column right */
        for (uint32_t x = SQ_X0; x < SQ_X1; x++) {
            CHECK_EQ_INT(g_bits[(y * W) + x], 1); /* white sprite: all on */
        }
    }
}

static void test_layers_level_edges_kept_away_from_sprites(void)
{
    op_layers_cfg_t cfg;
    (void)op_layers_cfg_default(&cfg);
    /* Black sprite on a black level, plus a black/white level step at x = 100. */
    make_square_sprite(0U, 0U);
    for (uint32_t y = 0U; y < H; y++) {
        for (uint32_t x = 100U; x < W; x++) {
            g_gray[(y * W) + x] = 255U;
        }
    }
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_OK);
    for (uint32_t y = 0U; y < H; y++) {
        CHECK_EQ_INT(g_bits[(y * W) + 99U], 1); /* level edge drawn */
        CHECK_EQ_INT(g_bits[(y * W) + 10U], 0); /* dark level, far from everything: off */
    }
}

static void test_layers_coverage_threshold(void)
{
    op_layers_cfg_t cfg;
    (void)op_layers_cfg_default(&cfg);
    memset(g_gray, 255, sizeof g_gray);
    memset(g_cov, (int)cfg.cov_threshold - 1, sizeof g_cov); /* just below: all level */
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_OK);
    memset(g_level, 255 >> OP_LAYERS_DEFAULT_BG_SHIFT, sizeof g_level);
    CHECK_EQ_INT(op_dither_bayer4(g_level, W, H, g_ref), OP_OK);
    CHECK(memcmp(g_bits, g_ref, N) == 0);
}

static void test_layers_rejects_bad_args(void)
{
    op_layers_cfg_t cfg;
    (void)op_layers_cfg_default(&cfg);
    CHECK_EQ_INT(op_layers(NULL, g_cov, W, H, &cfg, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_layers(g_gray, NULL, W, H, &cfg, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, NULL, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, NULL), OP_ERR_NULL);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, 0U, H, &cfg, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W + 1U, H, &cfg, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H + 1U, &cfg, g_bits), OP_ERR_SIZE);
    cfg.bg_shift = 8U;
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_ERR_RANGE);
    (void)op_layers_cfg_default(&cfg);
    cfg.cov_threshold = 0U;
    CHECK_EQ_INT(op_layers(g_gray, g_cov, W, H, &cfg, g_bits), OP_ERR_RANGE);
    CHECK_EQ_INT(op_layers_cfg_default(NULL), OP_ERR_NULL);
}

int main(void)
{
    RUN(test_flat_image_is_dimmed_bayer_without_edges);
    RUN(test_vertical_step_gets_outlined);
    RUN(test_fine_texture_is_not_outlined);
    RUN(test_rejects_bad_args);
    RUN(test_tiny_image);
    RUN(test_layers_no_sprites_flat_is_dim_bayer);
    RUN(test_layers_all_sprite_flat_is_boosted_bayer);
    RUN(test_layers_sprite_fill_saturates);
    RUN(test_layers_sprite_gets_black_halo);
    RUN(test_layers_level_edges_kept_away_from_sprites);
    RUN(test_layers_coverage_threshold);
    RUN(test_layers_rejects_bad_args);
    return TEST_REPORT();
}
