/**
 * @file op_nano.c
 * @brief Nano style: flat shaded wall runs, black planes, sprites drawn by class.
 */
#include "op_nano.h"

#include <stddef.h>

#include "op_dither.h"

/* Sprite drawing (see op_nano.h). Gray values on the 0..255 scale. */
#define RING_PX (3U)         /* background sampled this far around a sprite's box */
#define DARK_ENTER_PCT (62U) /* lit background share that turns a light sprite dark */
#define DARK_EXIT_PCT (38U)  /* ...and back: hysteresis between the 5 wall levels */
#define DARK_FIRST_PCT (50U) /* first frame of a sprite */
#define PCT (100U)
#define DETAIL_X10 (6U) /* light body: pixel darker than 0.6 x local mean is cut */
#define DETAIL_DIV (10U)
#define DETAIL_R (2U)         /* local mean window radius (5x5) */
#define HIGHLIGHT_GRAY (150U) /* dark body: pixels brighter than this stay lit */
#define ITEM_MIN_GRAY (60U)
#define CORPSE_FILL_GRAY (64U) /* Bayer level: 4 of 16 pixels */
#define WEAPON_GAIN_X16 (18U)
#define WEAPON_FLASH_GRAY (170U) /* muzzle flash and other very bright weapon pixels: solid */

op_status_t op_nano_cfg_default(op_nano_cfg_t *cfg)
{
    if (cfg == NULL) {
        return OP_ERR_NULL;
    }
    cfg->levels = (uint8_t)OP_NANO_DEFAULT_LEVELS;
    cfg->gain_x16 = (uint8_t)OP_NANO_DEFAULT_GAIN_X16;
    cfg->black = (uint8_t)OP_NANO_DEFAULT_BLACK;
    cfg->min_level = (uint8_t)OP_NANO_DEFAULT_MIN_LEVEL;
    cfg->split_threshold = (uint8_t)OP_NANO_DEFAULT_SPLIT_THRESHOLD;
    cfg->sprite_threshold = (uint8_t)OP_NANO_DEFAULT_SPRITE_THRESHOLD;
    cfg->cov_threshold = (uint8_t)OP_NANO_DEFAULT_COV_THRESHOLD;
    cfg->wall_edges = true;
    return OP_OK;
}

/** Everything one render needs, so the helpers keep short parameter lists. */
typedef struct {
    const uint8_t *gray;
    const uint8_t *wall_cov;
    const op_nano_sprites_t *spr;
    uint32_t w;
    uint32_t h;
    const op_nano_cfg_t *cfg;
    uint8_t *bits;
} nano_ctx_t;

/* Per id scratch: bounding box on the grid and contrast mode. Static: not reentrant. */
static uint8_t s_x0[OP_NANO_IDS];
static uint8_t s_y0[OP_NANO_IDS];
static uint8_t s_x1[OP_NANO_IDS];
static uint8_t s_y1[OP_NANO_IDS];
static uint8_t s_mode[OP_NANO_IDS];

_Static_assert(OP_MAX_DST_WIDTH_PX <= 255U, "box coordinates fit a byte");
_Static_assert(OP_OLED_HEIGHT_PX <= 255U, "box coordinates fit a byte");

/** Sprite id shown at (x, y), 0 where the sprite coverage is below the threshold. */
static uint32_t eid(const nano_ctx_t *c, uint32_t x, uint32_t y)
{
    const size_t i = ((size_t)y * c->w) + x;
    return (c->spr->cov[i] >= c->cfg->cov_threshold) ? c->spr->id[i] : 0U;
}

static bool is_wall(const nano_ctx_t *c, uint32_t x, uint32_t y)
{
    const size_t i = ((size_t)y * c->w) + x;
    return (c->wall_cov[i] >= c->cfg->cov_threshold) && (eid(c, x, y) == 0U);
}

static uint32_t abs_diff(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

/** Shaded gray (0..255, one of cfg->levels values) for a run with mean gray @p mean. */
static uint32_t run_shade(const op_nano_cfg_t *cfg, uint32_t mean)
{
    uint32_t v = 0U;
    if (mean > cfg->black) {
        v = ((mean - cfg->black) * cfg->gain_x16) / OP_NANO_GAIN_DIV;
    }
    if (v > OP_GRAY_MAX) {
        v = OP_GRAY_MAX;
    }
    const uint32_t steps = (uint32_t)cfg->levels - 1U;
    uint32_t q = ((v * steps) + (OP_GRAY_MAX / 2U)) / OP_GRAY_MAX;
    if (q < cfg->min_level) {
        q = cfg->min_level;
    }
    return (q * OP_GRAY_MAX) / steps;
}

/** Shade the wall run [y0, y1) of column x and draw its edge pixels. */
static void draw_run(const nano_ctx_t *c, uint32_t x, uint32_t y0, uint32_t y1)
{
    uint32_t sum = 0U;
    for (uint32_t y = y0; y < y1; y++) {
        sum += c->gray[((size_t)y * c->w) + x];
    }
    const uint32_t n = y1 - y0;
    const uint32_t shade = run_shade(c->cfg, (sum + (n / 2U)) / n);
    for (uint32_t y = y0; y < y1; y++) {
        c->bits[((size_t)y * c->w) + x] = (shade > op_bayer4_threshold(x, y)) ? 1U : 0U;
    }
    if (c->cfg->wall_edges) {
        /* First pixel: the wall's top or the split line. Last pixel only where the wall itself
         * ends; at a split the next run's first pixel already draws the line. */
        c->bits[((size_t)y0 * c->w) + x] = 1U;
        if ((y1 == c->h) || !is_wall(c, x, y1)) {
            c->bits[((size_t)(y1 - 1U) * c->w) + x] = 1U;
        }
    }
}

/** Walls of one column: find each run (cut at gray jumps) and shade it; the rest is off. */
static void draw_column(const nano_ctx_t *c, uint32_t x)
{
    uint32_t y = 0U;
    while (y < c->h) {
        if (!is_wall(c, x, y)) {
            c->bits[((size_t)y * c->w) + x] = 0U;
            y++;
            continue;
        }
        const uint32_t y0 = y;
        y++;
        while ((y < c->h) && is_wall(c, x, y)) {
            const uint32_t g = c->gray[((size_t)y * c->w) + x];
            const uint32_t g_above = c->gray[((size_t)(y - 1U) * c->w) + x];
            if ((c->cfg->split_threshold != 0U) &&
                (abs_diff(g, g_above) > c->cfg->split_threshold)) {
                break;
            }
            y++;
        }
        draw_run(c, x, y0, y);
    }
}

/** Bounding box of every id on the grid; ids not present get x0 > x1. */
static void sprite_boxes(const nano_ctx_t *c)
{
    for (uint32_t k = 0U; k < OP_NANO_IDS; k++) {
        s_x0[k] = UINT8_MAX;
        s_y0[k] = UINT8_MAX;
        s_x1[k] = 0U;
        s_y1[k] = 0U;
    }
    for (uint32_t y = 0U; y < c->h; y++) {
        for (uint32_t x = 0U; x < c->w; x++) {
            const uint32_t k = eid(c, x, y);
            if (k == 0U) {
                continue;
            }
            s_x0[k] = (x < s_x0[k]) ? (uint8_t)x : s_x0[k];
            s_y0[k] = (y < s_y0[k]) ? (uint8_t)y : s_y0[k];
            s_x1[k] = (x > s_x1[k]) ? (uint8_t)x : s_x1[k];
            s_y1[k] = (y > s_y1[k]) ? (uint8_t)y : s_y1[k];
        }
    }
}

static bool is_adaptive(uint32_t cls)
{
    return (cls == (uint32_t)OP_SPR_THING) || (cls == (uint32_t)OP_SPR_MONSTER) ||
           (cls == (uint32_t)OP_SPR_MONSTER_HIT) || (cls == (uint32_t)OP_SPR_CORPSE) ||
           (cls == (uint32_t)OP_SPR_ITEM);
}

/**
 * Contrast mode of id k: share of lit pixels among the non-sprite pixels in its box grown by
 * RING_PX (walls are already drawn in c->bits), with hysteresis against the last frame.
 */
static uint8_t choose_mode(const nano_ctx_t *c, uint32_t k)
{
    const uint32_t x0 = (s_x0[k] > RING_PX) ? (s_x0[k] - RING_PX) : 0U;
    const uint32_t y0 = (s_y0[k] > RING_PX) ? (s_y0[k] - RING_PX) : 0U;
    const uint32_t x1 = (((uint32_t)s_x1[k] + RING_PX) < c->w) ? (s_x1[k] + RING_PX) : (c->w - 1U);
    const uint32_t y1 = (((uint32_t)s_y1[k] + RING_PX) < c->h) ? (s_y1[k] + RING_PX) : (c->h - 1U);
    uint32_t lit = 0U;
    uint32_t total = 0U;
    for (uint32_t y = y0; y <= y1; y++) {
        for (uint32_t x = x0; x <= x1; x++) {
            if (eid(c, x, y) == 0U) {
                lit += c->bits[((size_t)y * c->w) + x];
                total++;
            }
        }
    }
    const uint8_t prev =
        (c->spr->prev_mode != NULL) ? c->spr->prev_mode[k] : (uint8_t)OP_SPR_MODE_UNKNOWN;
    if (total == 0U) {
        return (prev == (uint8_t)OP_SPR_MODE_DARK) ? prev : (uint8_t)OP_SPR_MODE_LIGHT;
    }
    uint32_t pct = DARK_FIRST_PCT;
    if (prev == (uint8_t)OP_SPR_MODE_LIGHT) {
        pct = DARK_ENTER_PCT;
    } else if (prev == (uint8_t)OP_SPR_MODE_DARK) {
        pct = DARK_EXIT_PCT;
    } else {
        /* unknown: midpoint */
    }
    return ((lit * PCT) > (pct * total)) ? (uint8_t)OP_SPR_MODE_DARK : (uint8_t)OP_SPR_MODE_LIGHT;
}

/** True if a 4-neighbour inside the view shows another id (the view border is not a rim). */
static bool is_rim(const nano_ctx_t *c, uint32_t x, uint32_t y, uint32_t k)
{
    return ((x > 0U) && (eid(c, x - 1U, y) != k)) ||
           (((x + 1U) < c->w) && (eid(c, x + 1U, y) != k)) ||
           ((y > 0U) && (eid(c, x, y - 1U) != k)) ||
           (((y + 1U) < c->h) && (eid(c, x, y + 1U) != k));
}

/** Light body pixel darker than DETAIL_X10/10 of the mean of its 5x5 same-id neighbours. */
static bool is_dark_detail(const nano_ctx_t *c, uint32_t x, uint32_t y, uint32_t k)
{
    const uint32_t x0 = (x > DETAIL_R) ? (x - DETAIL_R) : 0U;
    const uint32_t y0 = (y > DETAIL_R) ? (y - DETAIL_R) : 0U;
    const uint32_t x1 = ((x + DETAIL_R) < c->w) ? (x + DETAIL_R) : (c->w - 1U);
    const uint32_t y1 = ((y + DETAIL_R) < c->h) ? (y + DETAIL_R) : (c->h - 1U);
    uint32_t sum = 0U;
    uint32_t n = 0U;
    for (uint32_t ny = y0; ny <= y1; ny++) {
        for (uint32_t nx = x0; nx <= x1; nx++) {
            if (eid(c, nx, ny) == k) {
                sum += c->gray[((size_t)ny * c->w) + nx];
                n++;
            }
        }
    }
    const uint32_t g = c->gray[((size_t)y * c->w) + x];
    return (g * DETAIL_DIV * n) < (DETAIL_X10 * sum);
}

/** Pixel value of sprite k (class cls, contrast mode) at (x, y). */
static uint8_t sprite_pixel(const nano_ctx_t *c, uint32_t x, uint32_t y, uint32_t k)
{
    const uint32_t cls = c->spr->cls[k];
    const uint32_t g = c->gray[((size_t)y * c->w) + x];
    const bool rim = is_rim(c, x, y, k);
    bool dark = (s_mode[k] == (uint8_t)OP_SPR_MODE_DARK);
    bool on = false;
    switch (cls) {
    case OP_SPR_MID:
        on = g > c->cfg->sprite_threshold;
        break;
    case OP_SPR_FX:
        on = true;
        break;
    case OP_SPR_WEAPON:
        on = rim || (g > WEAPON_FLASH_GRAY) ||
             (((g * WEAPON_GAIN_X16) / OP_NANO_GAIN_DIV) > op_bayer4_threshold(x, y));
        break;
    case OP_SPR_CORPSE: {
        const bool dot = CORPSE_FILL_GRAY > op_bayer4_threshold(x, y);
        on = dark ? (!dot && !rim) : (dot || rim);
        break;
    }
    case OP_SPR_ITEM:
        on = dark ? (!rim && (g > ITEM_MIN_GRAY)) : (rim || (g > ITEM_MIN_GRAY));
        break;
    case OP_SPR_MONSTER_HIT:
        dark = !dark; /* the hit flash: opposite of the normal look */
        /* fall through */
    default: /* OP_SPR_THING, OP_SPR_MONSTER, unknown ids */
        if (dark) {
            on = rim || (g > HIGHLIGHT_GRAY);
        } else {
            on = rim || ((g > c->cfg->sprite_threshold) && !is_dark_detail(c, x, y, k));
        }
        break;
    }
    return on ? 1U : 0U;
}

/** Front-most id among the 8 neighbours of (x, y) that casts a halo (not a mid texture). */
static uint32_t front_neighbour(const nano_ctx_t *c, uint32_t x, uint32_t y)
{
    const uint32_t x0 = (x > 0U) ? (x - 1U) : 0U;
    const uint32_t y0 = (y > 0U) ? (y - 1U) : 0U;
    const uint32_t x1 = ((x + 1U) < c->w) ? (x + 1U) : x;
    const uint32_t y1 = ((y + 1U) < c->h) ? (y + 1U) : y;
    uint32_t front = 0U;
    for (uint32_t ny = y0; ny <= y1; ny++) {
        for (uint32_t nx = x0; nx <= x1; nx++) {
            const uint32_t k = eid(c, nx, ny);
            if ((k > front) && (c->spr->cls[k] != (uint8_t)OP_SPR_MID)) {
                front = k;
            }
        }
    }
    return front;
}

/** Sprites over the drawn walls: modes, pixels by class, then the halos. */
static void draw_sprites(const nano_ctx_t *c)
{
    sprite_boxes(c);
    for (uint32_t k = 1U; k < OP_NANO_IDS; k++) {
        s_mode[k] = (uint8_t)OP_SPR_MODE_UNKNOWN;
        if ((s_x0[k] <= s_x1[k]) && is_adaptive(c->spr->cls[k])) {
            s_mode[k] = choose_mode(c, k);
        }
    }
    s_mode[0] = (uint8_t)OP_SPR_MODE_UNKNOWN;
    if (c->spr->mode != NULL) {
        for (uint32_t k = 0U; k < OP_NANO_IDS; k++) {
            c->spr->mode[k] = s_mode[k];
        }
    }
    for (uint32_t y = 0U; y < c->h; y++) {
        for (uint32_t x = 0U; x < c->w; x++) {
            const uint32_t k = eid(c, x, y);
            if (k != 0U) {
                c->bits[((size_t)y * c->w) + x] = sprite_pixel(c, x, y, k);
            }
        }
    }
    for (uint32_t y = 0U; y < c->h; y++) {
        for (uint32_t x = 0U; x < c->w; x++) {
            if (front_neighbour(c, x, y) > eid(c, x, y)) {
                c->bits[((size_t)y * c->w) + x] = 0U;
            }
        }
    }
}

op_spr_class_t op_nano_classify(const op_spr_facts_t *facts)
{
    if (facts == NULL) {
        return OP_SPR_NONE;
    }
    if (facts->weapon) {
        return OP_SPR_WEAPON;
    }
    if (facts->fx) {
        return OP_SPR_FX;
    }
    if (facts->corpse) {
        return facts->still ? OP_SPR_CORPSE : OP_SPR_MONSTER_HIT;
    }
    if (facts->monster) {
        return facts->hit ? OP_SPR_MONSTER_HIT : OP_SPR_MONSTER;
    }
    return facts->item ? OP_SPR_ITEM : OP_SPR_THING;
}

op_status_t op_nano(const uint8_t *gray, const uint8_t *wall_cov, const op_nano_sprites_t *spr,
                    uint16_t w, uint16_t h, const op_nano_cfg_t *cfg,
                    uint8_t *bits) /* NOLINT(readability-non-const-parameter): via nano_ctx_t */
{
    if ((gray == NULL) || (wall_cov == NULL) || (spr == NULL) || (cfg == NULL) || (bits == NULL)) {
        return OP_ERR_NULL;
    }
    if ((spr->cov == NULL) || (spr->id == NULL) || (spr->cls == NULL)) {
        return OP_ERR_NULL;
    }
    if ((w == 0U) || (w > OP_MAX_DST_WIDTH_PX) || (h == 0U) ||
        (((uint32_t)w * h) > OP_NANO_MAX_PIXELS)) {
        return OP_ERR_SIZE;
    }
    if ((cfg->levels < OP_NANO_MIN_LEVELS) || (cfg->min_level >= cfg->levels) ||
        (cfg->cov_threshold == 0U)) {
        return OP_ERR_RANGE;
    }
    const nano_ctx_t c = {gray, wall_cov, spr, w, h, cfg, bits};
    for (uint32_t x = 0U; x < c.w; x++) {
        draw_column(&c, x);
    }
    draw_sprites(&c);
    return OP_OK;
}
