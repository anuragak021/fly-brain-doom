/**
 * @file op_dither.h
 * @brief Stage 3: 8 bit gray to 1 bit, ordered (Bayer 4x4) or Floyd-Steinberg.
 *
 * Output is one byte per pixel holding 0 (off) or 1 (on), row major, so the pack stage stays
 * a separate, testable step.
 */
#ifndef OP_DITHER_H
#define OP_DITHER_H

#include <stdint.h>

#include "op_types.h"

/** Which dither op_dither() runs. */
typedef enum {
    OP_DITHER_BAYER4 = 0,         /**< Ordered 4x4: stable between frames, cheapest. Default. */
    OP_DITHER_FLOYD_STEINBERG = 1 /**< Error diffusion, serpentine: finer detail, shimmers. */
} op_dither_mode_t;

/**
 * @brief Ordered dither with the 4x4 Bayer matrix.
 * @param gray  w * h gray pixels.
 * @param w     Width, 1..OP_MAX_DST_WIDTH_PX.
 * @param h     Height, >= 1.
 * @param bits  Output, w * h bytes of 0/1.
 * @return OP_OK, OP_ERR_NULL or OP_ERR_SIZE.
 */
op_status_t op_dither_bayer4(const uint8_t *gray, uint16_t w, uint16_t h, uint8_t *bits);

/**
 * @brief Bayer 4x4 threshold at pixel (x, y), the same matrix op_dither_bayer4() uses.
 *
 * A gray value g lights the pixel when g > threshold. For styles that pick a different gray
 * per pixel (layers, nano) without building a separate gray buffer first.
 * @return Threshold in 8..248.
 */
uint8_t op_bayer4_threshold(uint32_t x, uint32_t y);

/**
 * @brief Floyd-Steinberg error diffusion, serpentine scan, exact error conservation.
 * @param gray  w * h gray pixels (not modified; error lives in two stack row buffers).
 * @param w     Width, 1..OP_MAX_DST_WIDTH_PX.
 * @param h     Height, >= 1.
 * @param bits  Output, w * h bytes of 0/1.
 * @return OP_OK, OP_ERR_NULL or OP_ERR_SIZE.
 */
op_status_t op_dither_fs(const uint8_t *gray, uint16_t w, uint16_t h, uint8_t *bits);

/**
 * @brief Dispatch to the dither selected by @p mode.
 * @return As the selected function, or OP_ERR_RANGE for an unknown mode.
 */
op_status_t op_dither(op_dither_mode_t mode, const uint8_t *gray, uint16_t w, uint16_t h,
                      uint8_t *bits);

#endif /* OP_DITHER_H */
