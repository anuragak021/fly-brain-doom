/**
 * @file op_dither.c
 * @brief Stage 3: 8 bit gray to 1 bit.
 */
#include "op_dither.h"

#include <stdbool.h>
#include <stddef.h>

#define BAYER_SIZE (4U)
#define BAYER_MASK (BAYER_SIZE - 1U)
#define BAYER_LEVELS (16U)
#define BAYER_STEP (OP_LUT_ENTRIES / BAYER_LEVELS) /* 16 gray levels per matrix step */

/* Classic 4x4 Bayer index matrix. Threshold = index * 16 + 8, so thresholds are 8..248 and a
 * gray value g lights (number of thresholds below g) of 16 pixels: 0 -> none, 255 -> all,
 * 128 -> exactly half. */
static const uint8_t BAYER4[BAYER_SIZE][BAYER_SIZE] = {
    {0U, 8U, 2U, 10U},
    {12U, 4U, 14U, 6U},
    {3U, 11U, 1U, 9U},
    {15U, 7U, 13U, 5U},
};

/* Floyd-Steinberg weights, in sixteenths. */
#define FS_DENOM (16)
#define FS_W_RIGHT (7)
#define FS_W_DOWN_BACK (3)
#define FS_W_DOWN (5)
#define FS_W_DOWN_FWD (1)
_Static_assert(FS_W_RIGHT + FS_W_DOWN_BACK + FS_W_DOWN + FS_W_DOWN_FWD == FS_DENOM,
               "FS weights must sum to 16");

/* Error rows carry one guard cell on each side so x - 1 and x + 1 never need a bounds test. */
#define FS_GUARD (1U)
#define FS_ROW_LEN (OP_MAX_DST_WIDTH_PX + (2U * FS_GUARD))

static op_status_t check_args(const uint8_t *gray, uint16_t w, uint16_t h, const uint8_t *bits)
{
    if ((gray == NULL) || (bits == NULL)) {
        return OP_ERR_NULL;
    }
    if ((w == 0U) || (w > OP_MAX_DST_WIDTH_PX) || (h == 0U)) {
        return OP_ERR_SIZE;
    }
    return OP_OK;
}

uint8_t op_bayer4_threshold(uint32_t x, uint32_t y)
{
    return (uint8_t)((BAYER4[y & BAYER_MASK][x & BAYER_MASK] * BAYER_STEP) + (BAYER_STEP / 2U));
}

op_status_t op_dither_bayer4(const uint8_t *gray, uint16_t w, uint16_t h, uint8_t *bits)
{
    const op_status_t st = check_args(gray, w, h, bits);
    if (st != OP_OK) {
        return st;
    }
    for (uint32_t y = 0U; y < h; y++) {
        const uint8_t *row_in = &gray[(size_t)y * w];
        uint8_t *row_out = &bits[(size_t)y * w];
        for (uint32_t x = 0U; x < w; x++) {
            const uint32_t threshold =
                (BAYER4[y & BAYER_MASK][x & BAYER_MASK] * BAYER_STEP) + (BAYER_STEP / 2U);
            row_out[x] = (row_in[x] > threshold) ? 1U : 0U;
        }
    }
    return OP_OK;
}

/** Quantize one pixel at index @p x (guard offset applied) and spread its error. */
static void fs_pixel(const uint8_t *row_in, uint8_t *row_out, int16_t *cur, int16_t *nxt,
                     uint32_t x, int32_t dir)
{
    const uint32_t i = x + FS_GUARD;
    const int32_t value = (int32_t)row_in[x] + cur[i];
    const bool on = value >= (int32_t)OP_GRAY_THRESHOLD;
    row_out[x] = on ? 1U : 0U;
    const int32_t err = value - (on ? (int32_t)OP_GRAY_MAX : 0);
    /* Integer split; the "right" share takes the remainder so no error is lost. */
    const int32_t e_fwd = (err * FS_W_DOWN_FWD) / FS_DENOM;
    const int32_t e_back = (err * FS_W_DOWN_BACK) / FS_DENOM;
    const int32_t e_down = (err * FS_W_DOWN) / FS_DENOM;
    const int32_t e_right = err - e_fwd - e_back - e_down;
    const uint32_t ahead = (uint32_t)((int32_t)i + dir);
    const uint32_t behind = (uint32_t)((int32_t)i - dir);
    cur[ahead] = (int16_t)(cur[ahead] + e_right);
    nxt[behind] = (int16_t)(nxt[behind] + e_back);
    nxt[i] = (int16_t)(nxt[i] + e_down);
    nxt[ahead] = (int16_t)(nxt[ahead] + e_fwd);
}

op_status_t op_dither_fs(const uint8_t *gray, uint16_t w, uint16_t h, uint8_t *bits)
{
    const op_status_t st = check_args(gray, w, h, bits);
    if (st != OP_OK) {
        return st;
    }
    /* Error magnitudes stay within about +-255 * 2, well inside int16. */
    int16_t err_a[FS_ROW_LEN] = {0};
    int16_t err_b[FS_ROW_LEN] = {0};
    int16_t *cur = err_a;
    int16_t *nxt = err_b;
    for (uint32_t y = 0U; y < h; y++) {
        const uint8_t *row_in = &gray[(size_t)y * w];
        uint8_t *row_out = &bits[(size_t)y * w];
        if ((y & 1U) == 0U) {
            for (uint32_t x = 0U; x < w; x++) {
                fs_pixel(row_in, row_out, cur, nxt, x, 1);
            }
        } else {
            for (uint32_t x = w; x > 0U; x--) {
                fs_pixel(row_in, row_out, cur, nxt, x - 1U, -1);
            }
        }
        int16_t *const done = cur;
        cur = nxt;
        nxt = done;
        for (uint32_t i = 0U; i < FS_ROW_LEN; i++) {
            nxt[i] = 0;
        }
    }
    return OP_OK;
}

op_status_t op_dither(op_dither_mode_t mode, const uint8_t *gray, uint16_t w, uint16_t h,
                      uint8_t *bits)
{
    switch (mode) {
    case OP_DITHER_BAYER4:
        return op_dither_bayer4(gray, w, h, bits);
    case OP_DITHER_FLOYD_STEINBERG:
        return op_dither_fs(gray, w, h, bits);
    default:
        return OP_ERR_RANGE;
    }
}
