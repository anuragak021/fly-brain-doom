/**
 * @file bl_features.c
 * @brief Feature extraction from the gray view and the sprite id map. See bl_features.h.
 */
#include "bl_features.h"

#include <stddef.h>

#include "op_nano.h"

#define ID_COUNT (256U)
#define NO_COLUMN (0xFFFFU)
#define COV_FULL (255.0F)
#define MIN_WALL_WIDTH (8U)
#define CENTRE_BAND_DIV (8U) /* centre band = columns 3/8 .. 5/8 of the width */

typedef struct {
    uint16_t min_x, max_x, min_y, max_y;
} id_box_t;

static bool is_live_monster(uint8_t cls)
{
    return (cls == (uint8_t)OP_SPR_MONSTER) || (cls == (uint8_t)OP_SPR_MONSTER_HIT);
}

static void luminance(const uint8_t *gray, uint16_t w, uint16_t h, float *left, float *right)
{
    const uint32_t half = (uint32_t)w / 2U;
    uint32_t sum_l = 0U;
    uint32_t sum_r = 0U;
    for (uint32_t y = 0U; y < h; y++) {
        const uint8_t *row = &gray[(size_t)y * w];
        for (uint32_t x = 0U; x < half; x++) {
            sum_l += row[x];
            sum_r += row[(uint32_t)w - half + x];
        }
    }
    const float denom = (float)(half * h) * BL_GRAY_MAX;
    *left = (float)sum_l / denom;
    *right = (float)sum_r / denom;
}

static void grow(id_box_t *box, uint16_t x, uint16_t y)
{
    if (box->min_x == NO_COLUMN) {
        box->min_x = x;
        box->max_x = x;
        box->min_y = y;
        box->max_y = y;
        return;
    }
    box->min_x = (x < box->min_x) ? x : box->min_x;
    box->max_x = (x > box->max_x) ? x : box->max_x;
    box->min_y = (y < box->min_y) ? y : box->min_y;
    box->max_y = (y > box->max_y) ? y : box->max_y;
}

static uint32_t box_area(const id_box_t *box)
{
    return ((uint32_t)box->max_x - box->min_x + 1U) * ((uint32_t)box->max_y - box->min_y + 1U);
}

/** Largest live monster box over the id map; false if there is none. */
static bool largest_monster(const uint8_t *ids, const uint8_t *cls, uint16_t w, uint16_t h,
                            id_box_t *best)
{
    id_box_t boxes[ID_COUNT];
    for (uint32_t i = 0U; i < ID_COUNT; i++) {
        boxes[i].min_x = NO_COLUMN;
    }
    for (uint16_t y = 0U; y < h; y++) {
        for (uint16_t x = 0U; x < w; x++) {
            const uint8_t id = ids[((size_t)y * w) + x];
            if ((id != 0U) && is_live_monster(cls[id])) {
                grow(&boxes[id], x, y);
            }
        }
    }
    bool found = false;
    uint32_t best_area = 0U;
    for (uint32_t i = 1U; i < ID_COUNT; i++) {
        if ((boxes[i].min_x != NO_COLUMN) && (box_area(&boxes[i]) > best_area)) {
            best_area = box_area(&boxes[i]);
            *best = boxes[i];
            found = true;
        }
    }
    return found;
}

bl_status_t bl_features_make(const uint8_t *gray, const uint8_t *ids, const uint8_t *cls,
                             uint16_t w, uint16_t h, bl_features_t *out)
{
    if ((gray == NULL) || (ids == NULL) || (cls == NULL) || (out == NULL)) {
        return BL_ERR_NULL;
    }
    if ((w < 2U) || (h < 1U)) {
        return BL_ERR_SIZE;
    }
    for (uint32_t i = 0U; i < BL_N_FEATURES; i++) {
        out->feature[i] = 0.0F;
    }
    out->enemy_visible = false;
    out->enemy_offset = 0.0F;
    out->wall_centre = 0.0F;
    out->wall_ahead = false;
    out->open_left = false;
    luminance(gray, w, h, &out->feature[BL_FEAT_LUM_LEFT], &out->feature[BL_FEAT_LUM_RIGHT]);

    id_box_t best;
    if (largest_monster(ids, cls, w, h, &best)) {
        const float centre = (float)best.min_x + ((float)(best.max_x - best.min_x + 1U) / 2.0F);
        float offset = ((2.0F * centre) / (float)w) - 1.0F;
        offset = (offset < -1.0F) ? -1.0F : offset;
        offset = (offset > 1.0F) ? 1.0F : offset;
        out->enemy_visible = true;
        out->enemy_offset = offset;
        out->feature[BL_FEAT_ENEMY_LEFT] = (offset < 0.0F) ? -offset : 0.0F;
        out->feature[BL_FEAT_ENEMY_RIGHT] = (offset > 0.0F) ? offset : 0.0F;
    }
    return BL_OK;
}

static float mean_coverage(const uint8_t *cov, uint16_t w, uint16_t h, uint32_t x0, uint32_t x1)
{
    uint32_t sum = 0U;
    for (uint32_t y = 0U; y < h; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            sum += cov[((size_t)y * w) + x];
        }
    }
    return (float)sum / ((float)((x1 - x0) * h) * COV_FULL);
}

bl_status_t bl_walls_make(const uint8_t *wall_cov, uint16_t w, uint16_t h, bl_features_t *out)
{
    if ((wall_cov == NULL) || (out == NULL)) {
        return BL_ERR_NULL;
    }
    if ((w < MIN_WALL_WIDTH) || (h < 1U)) {
        return BL_ERR_SIZE;
    }
    const uint32_t half = (uint32_t)w / 2U;
    const uint32_t c0 = (3U * (uint32_t)w) / CENTRE_BAND_DIV;
    const uint32_t c1 = (5U * (uint32_t)w) / CENTRE_BAND_DIV;
    out->wall_centre = mean_coverage(wall_cov, w, h, c0, c1);
    out->wall_ahead = (out->wall_centre >= BL_WALL_AHEAD_FRAC);
    out->open_left =
        mean_coverage(wall_cov, w, h, 0U, half) < mean_coverage(wall_cov, w, h, w - half, w);
    return BL_OK;
}

#define BAM_HALF_TURN (2147483648.0F) /* 2^31 = 180 degrees */
#define DEG_HALF_TURN (180.0F)
#define BAM_SIGN_BIT (0x80000000UL)

bl_status_t bl_damage_make(uint32_t rel_bam, int32_t damagecount, float *left, float *right)
{
    if ((left == NULL) || (right == NULL)) {
        return BL_ERR_NULL;
    }
    *left = 0.0F;
    *right = 0.0F;
    if (damagecount <= 0) {
        return BL_OK;
    }
    float pain = (float)damagecount / BL_DAMAGE_FULL_COUNT;
    pain = (pain > 1.0F) ? 1.0F : pain;
    /* Signed angle without an implementation-defined uint32 -> int32 conversion: 0..2^31-1 is
     * left (counter-clockwise), 2^31..2^32-1 is right; exactly 180 degrees counts as right. */
    const float bam = (rel_bam < BAM_SIGN_BIT) ? (float)rel_bam : -(float)(0U - rel_bam);
    const float deg = (bam / BAM_HALF_TURN) * DEG_HALF_TURN;
    float side = deg / BL_DAMAGE_SIDE_DEG;
    side = (side > 1.0F) ? 1.0F : side;
    side = (side < -1.0F) ? -1.0F : side;
    *left = (side > 0.0F) ? (pain * side) : 0.0F;
    *right = (side < 0.0F) ? (pain * -side) : 0.0F;
    return BL_OK;
}
