/**
 * @file op_mask.h
 * @brief Engine-assisted masks on the OLED grid: which pixels show a sprite, which a wall.
 *
 * Sprites: the engine draws the level first and all sprites afterwards. The platform copies
 * the crop window of the framebuffer just before the sprites are drawn; afterwards
 * op_mask_diff turns that copy into a 0/1 map of the pixels the sprites changed, and
 * op_mask_coverage area-averages the map to the OLED grid with the same crop and weights as
 * op_scale_area, so the mask lines up exactly with the gray frame.
 *
 * Walls: before the engine draws walls, the platform fills the crop window with a marker
 * palette index that no lit wall texel can produce (OP_MASK_WALL_MARKER). Right after the
 * walls, every pixel that no longer holds the marker is a wall; floors and ceilings are drawn
 * later. op_mask_wall_coverage turns that into coverage on the OLED grid in one pass.
 *
 * Sprite ids (nano style): instead of a before/after copy, the engine reports every column
 * post it draws for a sprite and the platform writes that sprite's id (front sprites get higher
 * ids) into an id map of the crop window. op_mask_coverage_cfg works on that map as is (any
 * nonzero id counts), and op_mask_ids picks the front-most id under each OLED pixel.
 */
#ifndef OP_MASK_H
#define OP_MASK_H

#include <stddef.h>
#include <stdint.h>

#include "op_scale.h"
#include "op_types.h"

/** Bytes of the default crop window (133 rows), kept for the layers style. */
#define OP_MASK_SRC_BYTES (OP_DOOM_SRC_W * OP_DOOM_CROP_H)
/** Byte offset of the default crop window inside the 320x200 framebuffer. */
#define OP_MASK_SRC_OFFSET (OP_DOOM_SRC_W * OP_DOOM_CROP_Y)
/** Largest crop window any view config may use: the whole 3D view. Sizes snapshot buffers. */
#define OP_MASK_MAX_SRC_BYTES (OP_DOOM_SRC_W * OP_DOOM_VIEW_H)
/** Coverage value of an OLED pixel whose whole source area is masked. */
#define OP_MASK_COVERAGE_FULL (OP_GRAY_MAX)
/**
 * Palette index used to mark "not drawn yet" before the wall pass: 251 (magenta, 255/0/255).
 * Checked for DOOM1.WAD (v1.9) only: no texel of any TEXTURE1 wall patch maps to 251 through
 * any of the 34 COLORMAP rows (light levels, invulnerability, black). Recheck for other IWADs.
 */
#define OP_MASK_WALL_MARKER (251U)

/**
 * @brief Where a view config's crop window lies in the framebuffer.
 * @param cfg     View config; must crop whole rows (crop_x 0, crop_w == src_w == src_stride).
 * @param offset  Output: byte offset of the window's first row.
 * @param bytes   Output: window size in bytes, at most OP_MASK_MAX_SRC_BYTES.
 * @return OP_OK, OP_ERR_NULL, or OP_ERR_SIZE if the window is not whole rows of the 3D view.
 */
op_status_t op_mask_window(const op_scale_cfg_t *cfg, size_t *offset, size_t *bytes);

/**
 * @brief Replace each byte of @p before with 1 if it differs from @p after, else 0.
 * @param before  In: the crop window before the sprites. Out: the 0/1 change map.
 * @param after   The same window after the sprites.
 * @param n       Bytes in each buffer.
 * @return OP_OK, OP_ERR_NULL, or OP_ERR_SIZE if n is 0.
 * @note A sprite pixel that happens to have the same palette index as the wall behind it is not
 *       detected. That only thins the mask by isolated pixels; the area average absorbs it.
 */
op_status_t op_mask_diff(uint8_t *before, const uint8_t *after, size_t n);

/**
 * @brief Area-average a 0/1 map of a view config's crop window to coverage on its grid.
 * @param cfg   The view config the gray frame uses (see op_mask_window for the limits).
 * @param diff  The crop window's 0/1 map, op_mask_window() bytes.
 * @param cov   Output, cfg->dst_w * cfg->dst_h bytes, 0..OP_MASK_COVERAGE_FULL.
 * @return OP_OK, OP_ERR_NULL or OP_ERR_SIZE.
 */
op_status_t op_mask_coverage_cfg(const op_scale_cfg_t *cfg, const uint8_t *diff, uint8_t *cov);

/**
 * @brief op_mask_coverage_cfg() with the default 128x64 view config (op_scale_doom_default).
 * @param diff  OP_MASK_SRC_BYTES bytes from op_mask_diff.
 * @param cov   Output, OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX bytes.
 * @return OP_OK or OP_ERR_NULL.
 */
op_status_t op_mask_coverage(const uint8_t *diff, uint8_t *cov);

/**
 * @brief Wall coverage straight from the framebuffer right after the wall pass: every pixel
 *        of the crop window that is not @p marker counts as wall.
 * @param cfg     The view config the gray frame uses.
 * @param fb      The full source frame (cfg->src_stride * cfg->src_h bytes).
 * @param marker  The palette index the window was filled with before the walls.
 * @param cov     Output, cfg->dst_w * cfg->dst_h bytes, 0..OP_MASK_COVERAGE_FULL.
 * @return OP_OK, OP_ERR_NULL or OP_ERR_SIZE.
 */
op_status_t op_mask_wall_coverage(const op_scale_cfg_t *cfg, const uint8_t *fb, uint8_t marker,
                                  uint8_t *cov);

/**
 * @brief Front-most (largest) sprite id under each pixel of the view grid.
 * @param cfg  The view config (see op_mask_window for the limits).
 * @param ids  The crop window's id map, op_mask_window() bytes; 0 = no sprite.
 * @param out  Output, cfg->dst_w * cfg->dst_h ids: the maximum over the source pixels the
 *             output pixel touches (its area rounded outwards to whole source pixels).
 * @return OP_OK, OP_ERR_NULL or OP_ERR_SIZE.
 */
op_status_t op_mask_ids(const op_scale_cfg_t *cfg, const uint8_t *ids, uint8_t *out);

#endif /* OP_MASK_H */
