/** @file test_nano.c @brief Tests for the nano style (flat wall runs, black planes, sprites by
 * class). */
#include <stdint.h>
#include <string.h>

#include "op_dither.h"
#include "op_nano.h"
#include "test_runner.h"

#define W (16U)
#define H (12U)
#define N (W * H)
#define FULL (255U)

static uint8_t g_gray[N];
static uint8_t g_wall[N];
static uint8_t g_cov[N];
static uint8_t g_id[N];
static uint8_t g_cls[OP_NANO_IDS];
static uint8_t g_prev[OP_NANO_IDS];
static uint8_t g_mode[OP_NANO_IDS];
static op_nano_sprites_t g_spr = {g_cov, g_id, g_cls, g_prev, g_mode};
static uint8_t g_bits[N];
static uint8_t g_ref[N];

static op_nano_cfg_t default_cfg(void)
{
    op_nano_cfg_t cfg;
    (void)op_nano_cfg_default(&cfg);
    return cfg;
}

static void clear_inputs(void)
{
    memset(g_gray, 0, sizeof g_gray);
    memset(g_wall, 0, sizeof g_wall);
    memset(g_cov, 0, sizeof g_cov);
    memset(g_id, 0, sizeof g_id);
    memset(g_cls, 0, sizeof g_cls);
    memset(g_prev, 0, sizeof g_prev);
    memset(g_mode, 0, sizeof g_mode);
}

static uint32_t lit_in_rows(const uint8_t *bits, uint32_t y0, uint32_t y1)
{
    uint32_t n = 0U;
    for (uint32_t i = y0 * W; i < (y1 * W); i++) {
        n += bits[i];
    }
    return n;
}

static void test_defaults(void)
{
    const op_nano_cfg_t cfg = default_cfg();
    CHECK_EQ_INT(cfg.levels, OP_NANO_DEFAULT_LEVELS);
    CHECK_EQ_INT(cfg.min_level, OP_NANO_DEFAULT_MIN_LEVEL);
    CHECK_EQ_INT(cfg.split_threshold, OP_NANO_DEFAULT_SPLIT_THRESHOLD);
    CHECK(cfg.wall_edges);
    CHECK_EQ_INT(op_nano_cfg_default(NULL), OP_ERR_NULL);
}

static void test_planes_are_black(void)
{
    const op_nano_cfg_t cfg = default_cfg();
    clear_inputs();
    memset(g_gray, 200, sizeof g_gray); /* bright floor: still off */
    memset(g_bits, 1, sizeof g_bits);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(lit_in_rows(g_bits, 0U, H), 0);
}

static void test_wall_run_is_flat_with_edges(void)
{
    /* Gray 76 -> (76 - 12) * 2 = 128 -> level 2 of 0..4 -> shade 127. */
    const op_nano_cfg_t cfg = default_cfg();
    clear_inputs();
    memset(g_wall, FULL, sizeof g_wall);
    memset(g_gray, 76, sizeof g_gray);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    for (uint32_t y = 0U; y < H; y++) {
        for (uint32_t x = 0U; x < W; x++) {
            const uint8_t expect = ((y == 0U) || (y == (H - 1U)))
                                       ? 1U
                                       : ((127U > op_bayer4_threshold(x, y)) ? 1U : 0U);
            CHECK_EQ_INT(g_bits[(y * W) + x], expect);
        }
    }
    /* Small texture variation inside the run is averaged away: same picture. */
    memcpy(g_ref, g_bits, sizeof g_ref);
    for (uint32_t i = 0U; i < N; i++) {
        g_gray[i] = (uint8_t)((((i / W) % 2U) == 0U) ? 70U : 82U);
    }
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK(memcmp(g_bits, g_ref, N) == 0);
}

static void test_dark_wall_keeps_min_level(void)
{
    op_nano_cfg_t cfg = default_cfg();
    cfg.wall_edges = false;
    clear_inputs();
    memset(g_wall, FULL, sizeof g_wall);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    /* Level 1 of 0..4 = 63: 4 of the 16 Bayer thresholds (8, 24, 40, 56) are below it. */
    CHECK_EQ_INT(lit_in_rows(g_bits, 0U, H), N / 4U);
    cfg.min_level = 0U;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(lit_in_rows(g_bits, 0U, H), 0);
}

static void test_gray_jump_splits_run(void)
{
    /* Dark top half, white bottom half of one wall. Split on: the top keeps its dark shade and
     * a line is drawn at the jump. Split off: one run, one middle shade, no line. */
    op_nano_cfg_t cfg = default_cfg();
    const uint32_t mid = H / 2U;
    clear_inputs();
    memset(g_wall, FULL, sizeof g_wall);
    memset(&g_gray[mid * W], 220, (size_t)(H - mid) * W);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(lit_in_rows(g_bits, mid, mid + 1U), W);                /* split line */
    CHECK_EQ_INT(lit_in_rows(g_bits, mid + 1U, H), (H - mid - 1U) * W); /* white */
    uint32_t dark = 0U; /* row above the jump: min shade 63, Bayer only, no bottom edge */
    for (uint32_t x = 0U; x < W; x++) {
        dark += (63U > op_bayer4_threshold(x, mid - 1U)) ? 1U : 0U;
    }
    CHECK_EQ_INT(lit_in_rows(g_bits, mid - 1U, mid), dark);

    /* One run: the dark half takes the run's middle shade (191), so it gets brighter. */
    const uint32_t top_split = lit_in_rows(g_bits, 1U, mid);
    cfg.split_threshold = 0U;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK(lit_in_rows(g_bits, 1U, mid) > top_split);
    CHECK(lit_in_rows(g_bits, mid + 1U, H - 1U) < ((H - mid - 2U) * W));
}

/** Sprite @p id of class @p cls covering x [x0, x1), y [y0, y1) with gray @p gray. */
static void put_sprite(uint8_t id, uint8_t cls, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1,
                       uint8_t gray)
{
    g_cls[id] = cls;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            g_cov[(y * W) + x] = FULL;
            g_id[(y * W) + x] = id;
            g_gray[(y * W) + x] = gray;
        }
    }
}

static uint8_t px(uint32_t x, uint32_t y)
{
    return g_bits[(y * W) + x];
}

static void test_monster_light_on_dark_floor(void)
{
    /* No walls: dark background, so a light body with its rim, and a dark detail cut out. */
    const op_nano_cfg_t cfg = default_cfg();
    clear_inputs();
    put_sprite(1U, (uint8_t)OP_SPR_MONSTER, 5U, 11U, 3U, 9U, 100U);
    g_gray[(5U * W) + 7U] = 30U; /* eye: darker than 0.6 x the local mean */
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(g_mode[1], OP_SPR_MODE_LIGHT);
    CHECK_EQ_INT(px(6U, 4U), 1U);
    CHECK_EQ_INT(px(7U, 5U), 0U); /* the carved detail */
    CHECK_EQ_INT(px(5U, 3U), 1U); /* rim */
    CHECK_EQ_INT(px(4U, 3U), 0U); /* halo / floor */
}

static void test_monster_dark_on_bright_wall(void)
{
    /* White wall all around: dark body, lit rim, black halo between rim and wall. */
    const op_nano_cfg_t cfg = default_cfg();
    clear_inputs();
    memset(g_wall, FULL, sizeof g_wall);
    memset(g_gray, 255, sizeof g_gray);
    put_sprite(1U, (uint8_t)OP_SPR_MONSTER, 5U, 11U, 3U, 9U, 100U);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(g_mode[1], OP_SPR_MODE_DARK);
    CHECK_EQ_INT(px(7U, 5U), 0U);  /* body */
    CHECK_EQ_INT(px(5U, 5U), 1U);  /* rim */
    CHECK_EQ_INT(px(4U, 5U), 0U);  /* halo */
    CHECK_EQ_INT(px(2U, 5U), 1U);  /* wall */
    CHECK_EQ_INT(px(8U, 10U), 1U); /* wall below, past the halo */

    /* Same frame, monster just hit: the opposite look, a light body. */
    g_cls[1] = (uint8_t)OP_SPR_MONSTER_HIT;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(g_mode[1], OP_SPR_MODE_DARK); /* reported mode stays the background's */
    CHECK_EQ_INT(px(7U, 5U), 1U);
}

static void test_mode_hysteresis(void)
{
    /* Wall at level 2 (half the Bayer cells lit): a sprite keeps last frame's mode. */
    op_nano_cfg_t cfg = default_cfg();
    cfg.wall_edges = false;
    clear_inputs();
    memset(g_wall, FULL, sizeof g_wall);
    memset(g_gray, 76, sizeof g_gray);
    put_sprite(1U, (uint8_t)OP_SPR_THING, 6U, 10U, 4U, 8U, 100U);
    g_prev[1] = (uint8_t)OP_SPR_MODE_LIGHT;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(g_mode[1], OP_SPR_MODE_LIGHT);
    g_prev[1] = (uint8_t)OP_SPR_MODE_DARK;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(g_mode[1], OP_SPR_MODE_DARK);
    /* Wall at level 3 (12 of 16 lit): dark even coming from light. */
    for (uint32_t i = 0U; i < N; i++) {
        g_gray[i] = (g_id[i] == 0U) ? 107U : g_gray[i];
    }
    g_prev[1] = (uint8_t)OP_SPR_MODE_LIGHT;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(g_mode[1], OP_SPR_MODE_DARK);
    g_spr.prev_mode = NULL; /* optional */
    g_spr.mode = NULL;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    g_spr.prev_mode = g_prev;
    g_spr.mode = g_mode;
}

static void test_front_sprite_cuts_the_one_behind(void)
{
    /* Monster (id 1) with the weapon (id 255) overlapping its right part. */
    const op_nano_cfg_t cfg = default_cfg();
    clear_inputs();
    put_sprite(1U, (uint8_t)OP_SPR_MONSTER, 2U, 10U, 2U, 10U, 100U);
    put_sprite((uint8_t)OP_NANO_ID_WEAPON, (uint8_t)OP_SPR_WEAPON, 8U, 14U, 6U, 12U, 200U);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(px(7U, 7U), 0U);  /* monster pixel next to the weapon: halo */
    CHECK_EQ_INT(px(7U, 4U), 1U);  /* monster pixel away from it */
    CHECK_EQ_INT(px(8U, 7U), 1U);  /* weapon rim */
    CHECK_EQ_INT(px(11U, 9U), 1U); /* weapon body: 200 * 18 / 16 > every Bayer threshold */
}

static void test_mid_texture_casts_no_halo(void)
{
    const op_nano_cfg_t cfg = default_cfg();
    clear_inputs();
    put_sprite(1U, (uint8_t)OP_SPR_MONSTER, 2U, 8U, 2U, 8U, 100U);
    put_sprite(2U, (uint8_t)OP_SPR_MID, 8U, 12U, 2U, 8U, 100U);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(px(7U, 4U), 1U); /* monster next to the grate keeps its rim */
    CHECK_EQ_INT(px(9U, 4U), 1U); /* grate: gray above the threshold */
}

static void test_fx_corpse_item(void)
{
    const op_nano_cfg_t cfg = default_cfg();
    clear_inputs();
    put_sprite(1U, (uint8_t)OP_SPR_FX, 1U, 3U, 1U, 3U, 10U);       /* dark blood: still on */
    put_sprite(2U, (uint8_t)OP_SPR_CORPSE, 5U, 13U, 1U, 5U, 100U); /* dots and rim */
    put_sprite(3U, (uint8_t)OP_SPR_ITEM, 4U, 10U, 7U, 12U, 200U);  /* rim and bright pixels */
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(px(1U, 1U) + px(2U, 1U) + px(1U, 2U) + px(2U, 2U), 4U);
    uint32_t inner = 0U;
    for (uint32_t x = 6U; x < 12U; x++) {
        inner += px(x, 2U) + px(x, 3U);
    }
    CHECK_EQ_INT(inner, 3U); /* 64 > 4 of 16 Bayer cells: 3 of these 12 pixels */
    CHECK_EQ_INT(px(5U, 2U), 1U);
    CHECK_EQ_INT(px(7U, 9U), 1U);
    CHECK_EQ_INT(px(4U, 9U), 1U);
    g_gray[(9U * W) + 7U] = 40U; /* dark item pixel inside: off */
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_OK);
    CHECK_EQ_INT(px(7U, 9U), 0U);
}

static void test_classify(void)
{
    op_spr_facts_t f = {0};
    CHECK_EQ_INT(op_nano_classify(NULL), OP_SPR_NONE);
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_THING);
    f.item = true;
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_ITEM);
    f = (op_spr_facts_t){.monster = true};
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_MONSTER);
    f.hit = true;
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_MONSTER_HIT);
    f.corpse = true; /* dying: flash until the last frame */
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_MONSTER_HIT);
    f.still = true;
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_CORPSE);
    f.fx = true;
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_FX);
    f.weapon = true;
    CHECK_EQ_INT(op_nano_classify(&f), OP_SPR_WEAPON);
}

static void test_rejects_bad_args(void)
{
    op_nano_cfg_t cfg = default_cfg();
    CHECK_EQ_INT(op_nano(NULL, g_wall, &g_spr, W, H, &cfg, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_nano(g_gray, NULL, &g_spr, W, H, &cfg, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, NULL, W, H, &cfg, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, NULL, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, NULL), OP_ERR_NULL);
    op_nano_sprites_t bad = g_spr;
    bad.id = NULL;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &bad, W, H, &cfg, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, 128U, 65U, &cfg, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, 0U, H, &cfg, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, OP_MAX_DST_WIDTH_PX + 1U, 1U, &cfg, g_bits),
                 OP_ERR_SIZE);
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, 0U, &cfg, g_bits), OP_ERR_SIZE);
    cfg.levels = 1U;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_ERR_RANGE);
    cfg = default_cfg();
    cfg.min_level = cfg.levels;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_ERR_RANGE);
    cfg = default_cfg();
    cfg.cov_threshold = 0U;
    CHECK_EQ_INT(op_nano(g_gray, g_wall, &g_spr, W, H, &cfg, g_bits), OP_ERR_RANGE);
}

int main(void)
{
    RUN(test_defaults);
    RUN(test_planes_are_black);
    RUN(test_wall_run_is_flat_with_edges);
    RUN(test_dark_wall_keeps_min_level);
    RUN(test_gray_jump_splits_run);
    RUN(test_monster_light_on_dark_floor);
    RUN(test_monster_dark_on_bright_wall);
    RUN(test_mode_hysteresis);
    RUN(test_front_sprite_cuts_the_one_behind);
    RUN(test_mid_texture_casts_no_halo);
    RUN(test_fx_corpse_item);
    RUN(test_classify);
    RUN(test_rejects_bad_args);
    return TEST_REPORT();
}
