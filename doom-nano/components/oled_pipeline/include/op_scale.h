/**
 * @file op_scale.h
 * @brief Stage 2: crop and area-average downscale to the OLED resolution.
 */
#ifndef OP_SCALE_H
#define OP_SCALE_H

#include <stdint.h>

#include "op_types.h"

/** Source geometry, crop window and destination size for op_scale_area(). */
typedef struct {
    uint16_t src_w;      /**< Source width in pixels. */
    uint16_t src_h;      /**< Source height in pixels. */
    uint16_t src_stride; /**< Bytes between source rows (>= src_w). */
    uint16_t crop_x;     /**< Left edge of the crop window. */
    uint16_t crop_y;     /**< Top edge of the crop window. */
    uint16_t crop_w;     /**< Crop width (>= dst_w: downscale only). */
    uint16_t crop_h;     /**< Crop height (>= dst_h: downscale only). */
    uint16_t dst_w;      /**< Output width, 1..OP_MAX_DST_WIDTH_PX. */
    uint16_t dst_h;      /**< Output height, >= 1. */
} op_scale_cfg_t;

/**
 * @brief Default crop for a 320x200 Doom frame onto the 128x64 OLED.
 *
 * Doom pixels are displayed 1.2 times taller than wide (320x200 shown at 4:3). For a 2:1 panel
 * with correct proportions the window must be 320 x (320 / 2 / 1.2) = 320 x 133 source rows.
 * The window is bottom aligned on the 3D view above the 32 row status bar (rows 0..167): it
 * keeps the weapon sprite, which sits on the bottom of the view, and drops 35 rows of ceiling.
 * (A centred window cut the weapon in half on the real OLED.) See NOTES.md, "Crop vs letterbox".
 */
#define OP_DOOM_SRC_W (320U)
#define OP_DOOM_SRC_H (200U)
#define OP_DOOM_STATUSBAR_H (32U)
#define OP_DOOM_CROP_H (133U)
#define OP_DOOM_CROP_Y (OP_DOOM_SRC_H - OP_DOOM_STATUSBAR_H - OP_DOOM_CROP_H)

/**
 * HUD layout (nano style): the 3D view takes the top OP_HUD_VIEW_H_PX rows of the panel and a
 * text HUD the bottom OP_HUD_ROWS_PX. Same proportions rule as above:
 * 320 / (128 / 56) / 1.2 = 116.7, so 117 source rows, bottom aligned on the 3D view.
 */
#define OP_DOOM_VIEW_H (OP_DOOM_SRC_H - OP_DOOM_STATUSBAR_H) /**< 3D view rows (168). */
#define OP_HUD_ROWS_PX (8U)
#define OP_HUD_VIEW_H_PX (OP_OLED_HEIGHT_PX - OP_HUD_ROWS_PX)
#define OP_DOOM_HUD_CROP_H (117U)
#define OP_DOOM_HUD_CROP_Y (OP_DOOM_VIEW_H - OP_DOOM_HUD_CROP_H)

/**
 * @brief Fill @p cfg with the default Doom 320x200 -> 128x64 crop described above.
 * @param cfg Output configuration.
 * @return OP_OK or OP_ERR_NULL.
 */
op_status_t op_scale_doom_default(op_scale_cfg_t *cfg);

/**
 * @brief Fill @p cfg with the HUD layout crop: 320x200 -> 128 x OP_HUD_VIEW_H_PX.
 * @param cfg Output configuration.
 * @return OP_OK or OP_ERR_NULL.
 */
op_status_t op_scale_doom_hud_view(op_scale_cfg_t *cfg);

/**
 * @brief Area-average (box filter) downscale of the crop window to dst_w x dst_h.
 *
 * Each output pixel is the exact area-weighted mean of the source pixels it covers, including
 * fractional coverage at its edges, in integer arithmetic (no float, no rounding drift).
 * Works for any ratio, so the 2.5 x 2.08 Doom scale and an aspect-ignoring 2.5 x 2.5 both work.
 *
 * @param cfg  Geometry; validated before use.
 * @param src  Source pixels, src_stride * src_h bytes.
 * @param lut  Optional palette-index to gray table (256 entries). NULL means @p src is already
 *             gray. Folding the lookup in here avoids a 64000 byte gray copy of the frame.
 * @param dst  Output gray pixels, dst_w * dst_h bytes, row major.
 * @return OP_OK, OP_ERR_NULL, or OP_ERR_SIZE when the geometry is invalid.
 */
op_status_t op_scale_area(const op_scale_cfg_t *cfg, const uint8_t *src, const uint8_t *lut,
                          uint8_t *dst);

#endif /* OP_SCALE_H */
