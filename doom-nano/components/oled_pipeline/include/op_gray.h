/**
 * @file op_gray.h
 * @brief Stage 1: palette to grayscale lookup table.
 */
#ifndef OP_GRAY_H
#define OP_GRAY_H

#include <stdint.h>

#include "op_types.h"

#define OP_GAMMA_X100_IDENTITY (100U) /**< gamma_x100 value that leaves luma unchanged. */
#define OP_GAMMA_X100_MIN (50U)       /**< Darkest allowed setting (exponent 2.0). */
#define OP_GAMMA_X100_MAX (400U)      /**< Brightest allowed setting (exponent 0.25). */

/* Default levels. Luma percentiles of 120 captured Freedoom frames after the downscale are
 * 2nd 5..6, 98th 85..120. 16..170 was picked for the outline style from side by side host
 * previews of captured frames (see NOTES.md, "Levels" and "Outline style"). */
#define OP_GRAY_DEFAULT_BLACK (16U)  /**< Luma at or below this maps to 0. */
#define OP_GRAY_DEFAULT_WHITE (170U) /**< Luma at or above this maps to 255. */

/** Tone mapping applied when building the LUT. */
typedef struct {
    uint8_t black;       /**< Black point: luma <= black becomes 0. */
    uint8_t white;       /**< White point: luma >= white becomes 255. Must be > black. */
    uint16_t gamma_x100; /**< Gamma x100 after levels, [OP_GAMMA_X100_MIN, OP_GAMMA_X100_MAX]. */
} op_gray_cfg_t;

/**
 * @brief Fill @p cfg with the default levels and identity gamma.
 * @return OP_OK or OP_ERR_NULL.
 */
op_status_t op_gray_cfg_default(op_gray_cfg_t *cfg);

/**
 * @brief Build a 256 entry palette-index to gray table.
 *
 * Per entry: Rec.601 luma in integer form, (77 R + 150 G + 29 B) / 256; then a linear stretch of
 * [black, white] to [0, 255] with clipping; then out = 255 * (v / 255) ^ (100 / gamma_x100).
 * All cost is here, at palette change time (PrBoom calls I_SetPalette on damage and pickup
 * flashes), so the per-pixel cost is one table lookup. Fixed levels, not per-frame auto levels,
 * so brightness never pumps between frames.
 *
 * @param palette  768 bytes of RGB triplets (PLAYPAL format).
 * @param cfg      Tone mapping; validated.
 * @param lut      Output, 256 entries.
 * @return OP_OK, OP_ERR_NULL, or OP_ERR_RANGE for bad gamma or white <= black.
 * @note Uses single precision powf only when gamma is not identity.
 */
op_status_t op_gray_build_lut(const uint8_t palette[OP_PALETTE_BYTES], const op_gray_cfg_t *cfg,
                              uint8_t lut[OP_LUT_ENTRIES]);

/**
 * @brief Rec.601 luma of one RGB color, integer only.
 * @return Luma in 0..255.
 */
uint8_t op_gray_luma(uint8_t r, uint8_t g, uint8_t b);

#endif /* OP_GRAY_H */
