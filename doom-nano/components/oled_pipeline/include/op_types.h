/**
 * @file op_types.h
 * @brief Shared types and constants for the OLED display pipeline (pure C, no IDF).
 *
 * Pipeline: palette index -> gray LUT -> area downscale -> 1 bit dither -> SSD1306 page pack.
 */
#ifndef OP_TYPES_H
#define OP_TYPES_H

#include <stdint.h>

/** Status returned by every fallible pipeline function. */
typedef enum {
    OP_OK = 0,        /**< Success. */
    OP_ERR_NULL = 1,  /**< A required pointer was NULL. */
    OP_ERR_SIZE = 2,  /**< A dimension or buffer length is out of range. */
    OP_ERR_RANGE = 3, /**< A parameter value is out of its allowed range. */
} op_status_t;

#define OP_PALETTE_BYTES (768U) /**< 256 RGB triplets, as in Doom's PLAYPAL lump. */
#define OP_LUT_ENTRIES (256U)   /**< One gray level per palette index. */

#define OP_OLED_WIDTH_PX (128U)   /**< SSD1306 panel width. */
#define OP_OLED_HEIGHT_PX (64U)   /**< SSD1306 panel height. */
#define OP_OLED_PAGE_ROWS_PX (8U) /**< Rows per SSD1306 page (one byte, LSB at the top). */
#define OP_OLED_PACKED_BYTES (OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX / OP_OLED_PAGE_ROWS_PX)

/** Largest output width the downscaler and dithers accept (sizes their stack buffers). */
#define OP_MAX_DST_WIDTH_PX (OP_OLED_WIDTH_PX)

#define OP_GRAY_MAX (255U)       /**< Full white. */
#define OP_GRAY_THRESHOLD (128U) /**< Midpoint used for 1 bit quantization. */

_Static_assert(OP_OLED_PACKED_BYTES == 1024U, "SSD1306 128x64 frame is 1024 bytes");
_Static_assert((OP_OLED_HEIGHT_PX % OP_OLED_PAGE_ROWS_PX) == 0U, "height must be whole pages");

#endif /* OP_TYPES_H */
