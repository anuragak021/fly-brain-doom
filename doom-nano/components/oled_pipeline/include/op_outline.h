/**
 * @file op_outline.h
 * @brief Stage 3 alternative: "outline" style, a dimmed Bayer fill plus bright edges.
 *
 * Plain dithering turns Doom's wall textures into uniform dot noise at 128x64, and objects
 * (weapon, enemies) do not stand out. This style (chosen from side by side previews, see
 * NOTES.md "Outline style") dithers the scene at reduced brightness, then forces on every pixel
 * where a 3x3 box-blurred copy of the frame has a strong Sobel gradient. The blur suppresses
 * fine texture (brick mortar) so mostly geometry and sprite silhouettes get outlined.
 */
#ifndef OP_OUTLINE_H
#define OP_OUTLINE_H

#include <stdint.h>

#include "op_types.h"

#define OP_OUTLINE_MAX_PIXELS (OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX) /**< Sizes the scratch. */
#define OP_OUTLINE_DEFAULT_FILL_SHIFT (1U)                           /**< Fill at gray / 2. */
#define OP_OUTLINE_DEFAULT_THRESHOLD (90U) /**< On |gx| + |gy| of the blurred frame (max 2040). */
#define OP_OUTLINE_MAX_FILL_SHIFT (7U)

/** Outline style parameters. */
typedef struct {
    uint8_t fill_shift; /**< Fill brightness = gray >> fill_shift (0 = full, 8 would be off). */
    uint16_t edge_threshold; /**< Gradient magnitude above which a pixel is forced on. */
} op_outline_cfg_t;

/**
 * @brief Fill @p cfg with the defaults above.
 * @return OP_OK or OP_ERR_NULL.
 */
op_status_t op_outline_cfg_default(op_outline_cfg_t *cfg);

/**
 * @brief Render gray to 0/1 pixels in the outline style.
 * @param gray  w * h gray pixels.
 * @param w     Width, 1..OP_MAX_DST_WIDTH_PX.
 * @param h     Height, >= 1, with w * h <= OP_OUTLINE_MAX_PIXELS.
 * @param cfg   Parameters; fill_shift <= OP_OUTLINE_MAX_FILL_SHIFT.
 * @param bits  Output, w * h bytes of 0/1.
 * @return OP_OK, OP_ERR_NULL, OP_ERR_SIZE or OP_ERR_RANGE.
 * @note Uses an internal static scratch buffer (OP_OUTLINE_MAX_PIXELS bytes), so it is not
 *       reentrant. The pipeline runs in one task, so this is acceptable and avoids a heap.
 */
op_status_t op_outline(const uint8_t *gray, uint16_t w, uint16_t h, const op_outline_cfg_t *cfg,
                       uint8_t *bits);

/*
 * "Layers" style: the outline idea split by what each pixel shows, using the sprite coverage
 * from op_mask. The level (walls, floor, ceiling) is drawn dim with only strong geometry edges;
 * sprites (monsters, items, the player's weapon) are drawn bright and get a one pixel black
 * halo, so they stand out from the walls even when both have the same brightness. Chosen from
 * side by side previews on the shareware demo maps, see NOTES.md "Layers style".
 */
#define OP_LAYERS_DEFAULT_BG_SHIFT (2U)         /**< Level fill at gray / 4. */
#define OP_LAYERS_DEFAULT_EDGE_THRESHOLD (160U) /**< Level edges, same scale as the outline's. */
#define OP_LAYERS_DEFAULT_FG_GAIN_X16 (19U)     /**< Sprite fill = gray * 19 / 16 + offset. */
#define OP_LAYERS_DEFAULT_FG_OFFSET (70U)
#define OP_LAYERS_DEFAULT_COV_THRESHOLD (128U) /**< Sprite pixel if coverage >= about half. */
#define OP_LAYERS_FG_GAIN_DIV (16U)

/** Layers style parameters. */
typedef struct {
    uint8_t bg_shift;        /**< Level fill brightness = gray >> bg_shift. */
    uint16_t edge_threshold; /**< Level edge threshold on |gx| + |gy| of the blurred frame. */
    uint8_t fg_gain_x16;     /**< Sprite fill gain in sixteenths. */
    uint8_t fg_offset;       /**< Added to the sprite fill, result clamped to 255. */
    uint8_t cov_threshold;   /**< Coverage (op_mask, 0..255) at or above which a pixel is sprite. */
} op_layers_cfg_t;

/**
 * @brief Fill @p cfg with the OP_LAYERS_DEFAULT_* values.
 * @return OP_OK or OP_ERR_NULL.
 */
op_status_t op_layers_cfg_default(op_layers_cfg_t *cfg);

/**
 * @brief Render gray to 0/1 pixels in the layers style.
 * @param gray  w * h gray pixels (op_scale_area output).
 * @param cov   w * h sprite coverage (op_mask_coverage output) on the same grid.
 * @param w     Width, 1..OP_MAX_DST_WIDTH_PX.
 * @param h     Height, >= 1, with w * h <= OP_OUTLINE_MAX_PIXELS.
 * @param cfg   Parameters; bg_shift <= OP_OUTLINE_MAX_FILL_SHIFT, cov_threshold >= 1.
 * @param bits  Output, w * h bytes of 0/1.
 * @return OP_OK, OP_ERR_NULL, OP_ERR_SIZE or OP_ERR_RANGE.
 * @note Shares op_outline's static scratch buffer: not reentrant, same reasoning.
 */
op_status_t op_layers(const uint8_t *gray, const uint8_t *cov, uint16_t w, uint16_t h,
                      const op_layers_cfg_t *cfg, uint8_t *bits);

#endif /* OP_OUTLINE_H */
