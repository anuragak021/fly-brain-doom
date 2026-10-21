/**
 * @file op_gray.c
 * @brief Stage 1: palette to grayscale lookup table.
 */
#include "op_gray.h"

#include <math.h>
#include <stddef.h>

/* Rec.601 weights scaled to sum to 256, so the divide is a shift and white maps to 255. */
#define LUMA_R_WEIGHT (77U)
#define LUMA_G_WEIGHT (150U)
#define LUMA_B_WEIGHT (29U)
#define LUMA_SHIFT (8U)
#define LUMA_ROUND (1U << (LUMA_SHIFT - 1U))

_Static_assert(LUMA_R_WEIGHT + LUMA_G_WEIGHT + LUMA_B_WEIGHT == (1U << LUMA_SHIFT),
               "luma weights must sum to 256");

uint8_t op_gray_luma(uint8_t r, uint8_t g, uint8_t b)
{
    const uint32_t sum = (LUMA_R_WEIGHT * r) + (LUMA_G_WEIGHT * g) + (LUMA_B_WEIGHT * b);
    return (uint8_t)((sum + LUMA_ROUND) >> LUMA_SHIFT);
}

op_status_t op_gray_cfg_default(op_gray_cfg_t *cfg)
{
    if (cfg == NULL) {
        return OP_ERR_NULL;
    }
    cfg->black = (uint8_t)OP_GRAY_DEFAULT_BLACK;
    cfg->white = (uint8_t)OP_GRAY_DEFAULT_WHITE;
    cfg->gamma_x100 = (uint16_t)OP_GAMMA_X100_IDENTITY;
    return OP_OK;
}

/** Linear stretch of [black, white] to [0, 255], clipped, rounded. Requires white > black. */
static uint8_t apply_levels(uint8_t y, uint8_t black, uint8_t white)
{
    if (y <= black) {
        return 0U;
    }
    if (y >= white) {
        return (uint8_t)OP_GRAY_MAX;
    }
    const uint32_t range = (uint32_t)white - black;
    return (uint8_t)(((((uint32_t)y - black) * OP_GRAY_MAX) + (range / 2U)) / range);
}

op_status_t op_gray_build_lut(const uint8_t palette[OP_PALETTE_BYTES], const op_gray_cfg_t *cfg,
                              uint8_t lut[OP_LUT_ENTRIES])
{
    if ((palette == NULL) || (cfg == NULL) || (lut == NULL)) {
        return OP_ERR_NULL;
    }
    if ((cfg->gamma_x100 < OP_GAMMA_X100_MIN) || (cfg->gamma_x100 > OP_GAMMA_X100_MAX) ||
        (cfg->white <= cfg->black)) {
        return OP_ERR_RANGE;
    }
    const float exponent = (float)OP_GAMMA_X100_IDENTITY / (float)cfg->gamma_x100;
    const float full = (float)OP_GRAY_MAX;
    for (size_t i = 0U; i < OP_LUT_ENTRIES; i++) {
        const size_t rgb = 3U * i;
        const uint8_t y = op_gray_luma(palette[rgb], palette[rgb + 1U], palette[rgb + 2U]);
        const uint8_t stretched = apply_levels(y, cfg->black, cfg->white);
        if (cfg->gamma_x100 == OP_GAMMA_X100_IDENTITY) {
            lut[i] = stretched; /* exact, avoids float rounding on the default path */
        } else {
            const float v = full * powf((float)stretched / full, exponent);
            lut[i] = (uint8_t)(v + 0.5f); /* v is in [0, 255], so this cannot overflow */
        }
    }
    return OP_OK;
}
