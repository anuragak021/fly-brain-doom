/**
 * @file op_scale.c
 * @brief Stage 2: crop and area-average downscale.
 *
 * Integer area averaging: put source and destination on a common grid by scaling coordinates.
 * Horizontally, source column c covers [c * dst_w, (c + 1) * dst_w) and output column x covers
 * [x * crop_w, (x + 1) * crop_w). The overlap length is the integer weight of c in x, and the
 * weights of one output pixel sum to crop_w. Vertically the same with dst_h / crop_h. The total
 * weight per output pixel is crop_w * crop_h, so the result is an exact mean.
 */
#include "op_scale.h"

#include <stddef.h>

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? a : b;
}

static op_status_t validate(const op_scale_cfg_t *cfg)
{
    if ((cfg->dst_w == 0U) || (cfg->dst_w > OP_MAX_DST_WIDTH_PX) || (cfg->dst_h == 0U)) {
        return OP_ERR_SIZE;
    }
    if ((cfg->crop_w < cfg->dst_w) || (cfg->crop_h < cfg->dst_h)) {
        return OP_ERR_SIZE; /* downscale only */
    }
    if ((cfg->src_stride < cfg->src_w) || (((uint32_t)cfg->crop_x + cfg->crop_w) > cfg->src_w) ||
        (((uint32_t)cfg->crop_y + cfg->crop_h) > cfg->src_h)) {
        return OP_ERR_SIZE;
    }
    return OP_OK;
}

/** Weighted horizontal sums of one source row into acc[0..dst_w), each times @p wy. */
static void accumulate_row(const op_scale_cfg_t *cfg, const uint8_t *row, const uint8_t *lut,
                           uint32_t wy, uint32_t *acc)
{
    for (uint32_t x = 0U; x < cfg->dst_w; x++) {
        const uint32_t x0 = x * cfg->crop_w;
        const uint32_t x1 = x0 + cfg->crop_w;
        uint32_t sum = 0U;
        for (uint32_t c = x0 / cfg->dst_w; (c * cfg->dst_w) < x1; c++) {
            const uint32_t wx = min_u32(x1, (c + 1U) * cfg->dst_w) - max_u32(x0, c * cfg->dst_w);
            const uint8_t raw = row[cfg->crop_x + c];
            const uint32_t gray = (lut != NULL) ? lut[raw] : raw;
            sum += gray * wx;
        }
        acc[x] += sum * wy;
    }
}

op_status_t op_scale_doom_default(op_scale_cfg_t *cfg)
{
    if (cfg == NULL) {
        return OP_ERR_NULL;
    }
    cfg->src_w = (uint16_t)OP_DOOM_SRC_W;
    cfg->src_h = (uint16_t)OP_DOOM_SRC_H;
    cfg->src_stride = (uint16_t)OP_DOOM_SRC_W;
    cfg->crop_x = 0U;
    cfg->crop_y = (uint16_t)OP_DOOM_CROP_Y;
    cfg->crop_w = (uint16_t)OP_DOOM_SRC_W;
    cfg->crop_h = (uint16_t)OP_DOOM_CROP_H;
    cfg->dst_w = (uint16_t)OP_OLED_WIDTH_PX;
    cfg->dst_h = (uint16_t)OP_OLED_HEIGHT_PX;
    return OP_OK;
}

op_status_t op_scale_doom_hud_view(op_scale_cfg_t *cfg)
{
    const op_status_t st = op_scale_doom_default(cfg);
    if (st != OP_OK) {
        return st;
    }
    cfg->crop_y = (uint16_t)OP_DOOM_HUD_CROP_Y;
    cfg->crop_h = (uint16_t)OP_DOOM_HUD_CROP_H;
    cfg->dst_h = (uint16_t)OP_HUD_VIEW_H_PX;
    return OP_OK;
}

op_status_t op_scale_area(const op_scale_cfg_t *cfg, const uint8_t *src, const uint8_t *lut,
                          uint8_t *dst)
{
    if ((cfg == NULL) || (src == NULL) || (dst == NULL)) {
        return OP_ERR_NULL;
    }
    const op_status_t st = validate(cfg);
    if (st != OP_OK) {
        return st;
    }
    /* Max accumulator: 255 * crop_w * crop_h <= 255 * 65535 * 65535 would overflow, but crop
     * sizes are bounded by the source; check the product explicitly. */
    const uint32_t total = (uint32_t)cfg->crop_w * cfg->crop_h;
    if (total > (UINT32_MAX / OP_GRAY_MAX)) {
        return OP_ERR_SIZE;
    }
    uint32_t acc[OP_MAX_DST_WIDTH_PX];
    for (uint32_t y = 0U; y < cfg->dst_h; y++) {
        for (uint32_t x = 0U; x < cfg->dst_w; x++) {
            acc[x] = 0U;
        }
        const uint32_t y0 = y * cfg->crop_h;
        const uint32_t y1 = y0 + cfg->crop_h;
        for (uint32_t r = y0 / cfg->dst_h; (r * cfg->dst_h) < y1; r++) {
            const uint32_t wy = min_u32(y1, (r + 1U) * cfg->dst_h) - max_u32(y0, r * cfg->dst_h);
            const uint8_t *row = &src[(size_t)(cfg->crop_y + r) * cfg->src_stride];
            accumulate_row(cfg, row, lut, wy, acc);
        }
        uint8_t *out = &dst[(size_t)y * cfg->dst_w];
        for (uint32_t x = 0U; x < cfg->dst_w; x++) {
            out[x] = (uint8_t)((acc[x] + (total / 2U)) / total);
        }
    }
    return OP_OK;
}
