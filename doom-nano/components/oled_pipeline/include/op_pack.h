/**
 * @file op_pack.h
 * @brief Stage 4: pack 0/1 pixels into the SSD1306 page format.
 */
#ifndef OP_PACK_H
#define OP_PACK_H

#include <stddef.h>
#include <stdint.h>

#include "op_types.h"

/**
 * @brief Pack a 1 byte per pixel image into SSD1306 pages.
 *
 * Byte (page * w + x) holds column x of rows page*8 .. page*8+7, bit 0 at the top. This matches
 * the controller's horizontal addressing mode, so a whole frame is one I2C data write.
 *
 * @param bits     w * h bytes, nonzero means pixel on.
 * @param w        Width in pixels, >= 1.
 * @param h        Height in pixels, a nonzero multiple of 8.
 * @param out      Output buffer.
 * @param out_len  Size of @p out; must be >= w * h / 8.
 * @return OP_OK, OP_ERR_NULL or OP_ERR_SIZE.
 */
op_status_t op_pack_ssd1306(const uint8_t *bits, uint16_t w, uint16_t h, uint8_t *out,
                            size_t out_len);

#endif /* OP_PACK_H */
