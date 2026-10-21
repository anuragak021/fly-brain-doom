/**
 * @file op_outline.c
 * @brief Stage 3 alternative: dimmed Bayer fill plus bright Sobel edges of a blurred frame.
 */
#include "op_outline.h"

#include <stdbool.h>
#include <stddef.h>

#include "op_dither.h"

#define BLUR_TAPS (9U) /* 3x3 box */

/* Shared scratch: holds the dimmed frame first, then the blurred frame. */
static uint8_t s_scratch[OP_OUTLINE_MAX_PIXELS];

op_status_t op_outline_cfg_default(op_outline_cfg_t *cfg)
{
    if (cfg == NULL) {
        return OP_ERR_NULL;
    }
    cfg->fill_shift = (uint8_t)OP_OUTLINE_DEFAULT_FILL_SHIFT;
    cfg->edge_threshold = (uint16_t)OP_OUTLINE_DEFAULT_THRESHOLD;
    return OP_OK;
}

/** Clamp a coordinate into [0, n - 1] (edge replication at the borders). */
static uint32_t clamp_idx(int32_t v, uint32_t n)
{
    if (v < 0) {
        return 0U;
    }
    return ((uint32_t)v >= n) ? (n - 1U) : (uint32_t)v;
}

static uint32_t px(const uint8_t *img, uint32_t w, uint32_t h, int32_t x, int32_t y)
{
    return img[((size_t)clamp_idx(y, h) * w) + clamp_idx(x, w)];
}

static void box_blur(const uint8_t *gray, uint32_t w, uint32_t h, uint8_t *out)
{
    for (uint32_t y = 0U; y < h; y++) {
        for (uint32_t x = 0U; x < w; x++) {
            uint32_t sum = 0U;
            for (int32_t dy = -1; dy <= 1; dy++) {
                for (int32_t dx = -1; dx <= 1; dx++) {
                    sum += px(gray, w, h, (int32_t)x + dx, (int32_t)y + dy);
                }
            }
            out[((size_t)y * w) + x] = (uint8_t)(sum / BLUR_TAPS);
        }
    }
}

/** |gx| + |gy| of the 3x3 Sobel operator at (x, y). */
static uint32_t sobel_mag(const uint8_t *img, uint32_t w, uint32_t h, int32_t x, int32_t y)
{
    const int32_t gx =
        ((int32_t)px(img, w, h, x + 1, y - 1) + (2 * (int32_t)px(img, w, h, x + 1, y)) +
         (int32_t)px(img, w, h, x + 1, y + 1)) -
        ((int32_t)px(img, w, h, x - 1, y - 1) + (2 * (int32_t)px(img, w, h, x - 1, y)) +
         (int32_t)px(img, w, h, x - 1, y + 1));
    const int32_t gy =
        ((int32_t)px(img, w, h, x - 1, y + 1) + (2 * (int32_t)px(img, w, h, x, y + 1)) +
         (int32_t)px(img, w, h, x + 1, y + 1)) -
        ((int32_t)px(img, w, h, x - 1, y - 1) + (2 * (int32_t)px(img, w, h, x, y - 1)) +
         (int32_t)px(img, w, h, x + 1, y - 1));
    return (uint32_t)((gx < 0) ? -gx : gx) + (uint32_t)((gy < 0) ? -gy : gy);
}

op_status_t op_outline(const uint8_t *gray, uint16_t w, uint16_t h, const op_outline_cfg_t *cfg,
                       uint8_t *bits)
{
    if ((gray == NULL) || (cfg == NULL) || (bits == NULL)) {
        return OP_ERR_NULL;
    }
    if ((w == 0U) || (w > OP_MAX_DST_WIDTH_PX) || (h == 0U) ||
        (((uint32_t)w * h) > OP_OUTLINE_MAX_PIXELS)) {
        return OP_ERR_SIZE;
    }
    if (cfg->fill_shift > OP_OUTLINE_MAX_FILL_SHIFT) {
        return OP_ERR_RANGE;
    }
    const size_t n = (size_t)w * h;
    for (size_t i = 0U; i < n; i++) {
        s_scratch[i] = (uint8_t)(gray[i] >> cfg->fill_shift);
    }
    const op_status_t st = op_dither_bayer4(s_scratch, w, h, bits);
    if (st != OP_OK) {
        return st;
    }
    box_blur(gray, w, h, s_scratch);
    for (uint32_t y = 0U; y < h; y++) {
        for (uint32_t x = 0U; x < w; x++) {
            if (sobel_mag(s_scratch, w, h, (int32_t)x, (int32_t)y) > cfg->edge_threshold) {
                bits[((size_t)y * w) + x] = 1U;
            }
        }
    }
    return OP_OK;
}

op_status_t op_layers_cfg_default(op_layers_cfg_t *cfg)
{
    if (cfg == NULL) {
        return OP_ERR_NULL;
    }
    cfg->bg_shift = (uint8_t)OP_LAYERS_DEFAULT_BG_SHIFT;
    cfg->edge_threshold = (uint16_t)OP_LAYERS_DEFAULT_EDGE_THRESHOLD;
    cfg->fg_gain_x16 = (uint8_t)OP_LAYERS_DEFAULT_FG_GAIN_X16;
    cfg->fg_offset = (uint8_t)OP_LAYERS_DEFAULT_FG_OFFSET;
    cfg->cov_threshold = (uint8_t)OP_LAYERS_DEFAULT_COV_THRESHOLD;
    return OP_OK;
}

/** True if (x, y) or any of its 8 neighbours is a sprite pixel (out of range = not sprite). */
static bool near_sprite(const uint8_t *cov, uint32_t w, uint32_t h, uint32_t x, uint32_t y,
                        uint8_t threshold)
{
    for (int32_t dy = -1; dy <= 1; dy++) {
        for (int32_t dx = -1; dx <= 1; dx++) {
            const int32_t xx = (int32_t)x + dx;
            const int32_t yy = (int32_t)y + dy;
            if ((xx >= 0) && (yy >= 0) && ((uint32_t)xx < w) && ((uint32_t)yy < h) &&
                (cov[((size_t)yy * w) + (size_t)xx] >= threshold)) {
                return true;
            }
        }
    }
    return false;
}

op_status_t op_layers(const uint8_t *gray, const uint8_t *cov, uint16_t w, uint16_t h,
                      const op_layers_cfg_t *cfg, uint8_t *bits)
{
    if ((gray == NULL) || (cov == NULL) || (cfg == NULL) || (bits == NULL)) {
        return OP_ERR_NULL;
    }
    if ((w == 0U) || (w > OP_MAX_DST_WIDTH_PX) || (h == 0U) ||
        (((uint32_t)w * h) > OP_OUTLINE_MAX_PIXELS)) {
        return OP_ERR_SIZE;
    }
    if ((cfg->bg_shift > OP_OUTLINE_MAX_FILL_SHIFT) || (cfg->cov_threshold == 0U)) {
        return OP_ERR_RANGE;
    }
    /* Pass 1: fill. Bayer on a per-layer brightness; the matrix is position based, so each
     * pixel can pick its own input without a second buffer. */
    for (uint32_t y = 0U; y < h; y++) {
        for (uint32_t x = 0U; x < w; x++) {
            const size_t i = ((size_t)y * w) + x;
            uint32_t level;
            if (cov[i] >= cfg->cov_threshold) {
                level = ((gray[i] * (uint32_t)cfg->fg_gain_x16) / OP_LAYERS_FG_GAIN_DIV) +
                        cfg->fg_offset;
                level = (level > OP_GRAY_MAX) ? OP_GRAY_MAX : level;
            } else {
                level = (uint32_t)gray[i] >> cfg->bg_shift;
            }
            bits[i] = (level > op_bayer4_threshold(x, y)) ? 1U : 0U;
        }
    }
    /* Pass 2: halo around sprites (forced off), level edges elsewhere (forced on). Edges next
     * to a sprite are skipped: the halo already separates it, and its silhouette edge would
     * otherwise merge back into the wall. */
    box_blur(gray, w, h, s_scratch);
    for (uint32_t y = 0U; y < h; y++) {
        for (uint32_t x = 0U; x < w; x++) {
            const size_t i = ((size_t)y * w) + x;
            if (cov[i] >= cfg->cov_threshold) {
                continue;
            }
            if (near_sprite(cov, w, h, x, y, cfg->cov_threshold)) {
                bits[i] = 0U;
            } else if (sobel_mag(s_scratch, w, h, (int32_t)x, (int32_t)y) > cfg->edge_threshold) {
                bits[i] = 1U;
            }
        }
    }
    return OP_OK;
}
