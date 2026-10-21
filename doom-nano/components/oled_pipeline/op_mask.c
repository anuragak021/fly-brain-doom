/**
 * @file op_mask.c
 * @brief Sprite and wall masks, area-averaged to the OLED grid.
 */
#include "op_mask.h"

#include <stddef.h>

_Static_assert(OP_MASK_SRC_BYTES <= OP_MASK_MAX_SRC_BYTES, "default window fits the max");
_Static_assert((OP_DOOM_SRC_W * (OP_DOOM_HUD_CROP_Y + OP_DOOM_HUD_CROP_H)) <= OP_MASK_MAX_SRC_BYTES,
               "HUD view window lies inside the 3D view");

op_status_t op_mask_window(const op_scale_cfg_t *cfg, size_t *offset, size_t *bytes)
{
    if ((cfg == NULL) || (offset == NULL) || (bytes == NULL)) {
        return OP_ERR_NULL;
    }
    /* Whole rows only, so the window is one contiguous block, and inside the 3D view. */
    if ((cfg->crop_x != 0U) || (cfg->crop_w != cfg->src_w) || (cfg->src_stride != cfg->src_w) ||
        (cfg->crop_h == 0U) || (((uint32_t)cfg->crop_y + cfg->crop_h) > cfg->src_h)) {
        return OP_ERR_SIZE;
    }
    const size_t n = (size_t)cfg->src_w * cfg->crop_h;
    if (n > (size_t)OP_MASK_MAX_SRC_BYTES) {
        return OP_ERR_SIZE;
    }
    *offset = (size_t)cfg->src_w * cfg->crop_y;
    *bytes = n;
    return OP_OK;
}

op_status_t op_mask_diff(uint8_t *before, const uint8_t *after, size_t n)
{
    if ((before == NULL) || (after == NULL)) {
        return OP_ERR_NULL;
    }
    if (n == 0U) {
        return OP_ERR_SIZE;
    }
    for (size_t i = 0U; i < n; i++) {
        before[i] = (before[i] != after[i]) ? 1U : 0U;
    }
    return OP_OK;
}

/** LUT that maps every index except @p zero_index to full coverage. */
static void fill_binary_lut(uint8_t *lut, uint8_t zero_index)
{
    for (size_t i = 0U; i < OP_LUT_ENTRIES; i++) {
        lut[i] = (uint8_t)OP_MASK_COVERAGE_FULL;
    }
    lut[zero_index] = 0U;
}

op_status_t op_mask_coverage_cfg(const op_scale_cfg_t *cfg, const uint8_t *diff, uint8_t *cov)
{
    if ((cfg == NULL) || (diff == NULL) || (cov == NULL)) {
        return OP_ERR_NULL;
    }
    size_t offset = 0U;
    size_t bytes = 0U;
    const op_status_t st = op_mask_window(cfg, &offset, &bytes);
    if (st != OP_OK) {
        return st;
    }
    /* The map is the crop window itself: same config with the crop moved to the origin, so
     * the weights and the output grid are those of the gray frame. */
    op_scale_cfg_t win = *cfg;
    win.src_h = cfg->crop_h;
    win.crop_y = 0U;
    uint8_t lut[OP_LUT_ENTRIES];
    fill_binary_lut(lut, 0U);
    return op_scale_area(&win, diff, lut, cov);
}

op_status_t op_mask_coverage(const uint8_t *diff, uint8_t *cov)
{
    op_scale_cfg_t cfg;
    const op_status_t st = op_scale_doom_default(&cfg);
    if (st != OP_OK) {
        return st;
    }
    return op_mask_coverage_cfg(&cfg, diff, cov);
}

op_status_t op_mask_wall_coverage(const op_scale_cfg_t *cfg, const uint8_t *fb, uint8_t marker,
                                  uint8_t *cov)
{
    if ((cfg == NULL) || (fb == NULL) || (cov == NULL)) {
        return OP_ERR_NULL;
    }
    uint8_t lut[OP_LUT_ENTRIES];
    fill_binary_lut(lut, marker);
    return op_scale_area(cfg, fb, lut, cov);
}

/** Source span [first, end) of output cell @p o when @p src cells map onto @p dst cells. */
static void span(uint32_t o, uint32_t src, uint32_t dst, uint32_t *first, uint32_t *end)
{
    *first = (o * src) / dst;
    *end = (((o + 1U) * src) + dst - 1U) / dst;
}

op_status_t op_mask_ids(const op_scale_cfg_t *cfg, const uint8_t *ids, uint8_t *out)
{
    if ((cfg == NULL) || (ids == NULL) || (out == NULL)) {
        return OP_ERR_NULL;
    }
    size_t offset = 0U;
    size_t bytes = 0U;
    const op_status_t st = op_mask_window(cfg, &offset, &bytes);
    if (st != OP_OK) {
        return st;
    }
    if ((cfg->dst_w == 0U) || (cfg->dst_h == 0U) || (cfg->dst_w > cfg->crop_w) ||
        (cfg->dst_h > cfg->crop_h)) {
        return OP_ERR_SIZE;
    }
    for (uint32_t oy = 0U; oy < cfg->dst_h; oy++) {
        uint32_t y0 = 0U;
        uint32_t y1 = 0U;
        span(oy, cfg->crop_h, cfg->dst_h, &y0, &y1);
        for (uint32_t ox = 0U; ox < cfg->dst_w; ox++) {
            uint32_t x0 = 0U;
            uint32_t x1 = 0U;
            span(ox, cfg->crop_w, cfg->dst_w, &x0, &x1);
            uint8_t front = 0U;
            for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = x0; x < x1; x++) {
                    const uint8_t k = ids[((size_t)y * cfg->src_w) + x];
                    front = (k > front) ? k : front;
                }
            }
            out[((size_t)oy * cfg->dst_w) + ox] = front;
        }
    }
    return OP_OK;
}
