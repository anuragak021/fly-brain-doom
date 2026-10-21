/**
 * @file op_pack.c
 * @brief Stage 4: pack 0/1 pixels into the SSD1306 page format.
 */
#include "op_pack.h"

op_status_t op_pack_ssd1306(const uint8_t *bits, uint16_t w, uint16_t h, uint8_t *out,
                            size_t out_len)
{
    if ((bits == NULL) || (out == NULL)) {
        return OP_ERR_NULL;
    }
    if ((w == 0U) || (h == 0U) || ((h % OP_OLED_PAGE_ROWS_PX) != 0U)) {
        return OP_ERR_SIZE;
    }
    const uint32_t pages = h / OP_OLED_PAGE_ROWS_PX;
    if (out_len < ((size_t)pages * w)) {
        return OP_ERR_SIZE;
    }
    for (uint32_t page = 0U; page < pages; page++) {
        for (uint32_t x = 0U; x < w; x++) {
            uint32_t byte = 0U;
            for (uint32_t bit = 0U; bit < OP_OLED_PAGE_ROWS_PX; bit++) {
                const uint32_t y = (page * OP_OLED_PAGE_ROWS_PX) + bit;
                if (bits[((size_t)y * w) + x] != 0U) {
                    byte |= (1U << bit);
                }
            }
            out[((size_t)page * w) + x] = (uint8_t)byte;
        }
    }
    return OP_OK;
}
