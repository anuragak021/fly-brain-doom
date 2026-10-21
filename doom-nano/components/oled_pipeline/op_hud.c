/**
 * @file op_hud.c
 * @brief HUD line: 3x5 digits and 5x5 icons drawn into the bottom rows of the frame.
 */
#include "op_hud.h"

#include <stddef.h>

#include "op_scale.h"

#define DIGITS (10U)
#define FIELD_DIGITS (3U)
#define DIGIT_PITCH_PX (OP_HUD_DIGIT_COLS_PX + 1U)
#define ICON_GAP_PX (2U)
#define FIELD_X0_PX (2U)
#define GLYPH_TOP_OFFSET_PX (2U) /* separator row, blank row, then glyphs */
#define DECIMAL (10)

_Static_assert(OP_HUD_ROWS_PX == (GLYPH_TOP_OFFSET_PX + OP_HUD_GLYPH_ROWS_PX + 1U),
               "HUD rows: separator, blank, glyphs, blank");
_Static_assert((FIELD_X0_PX + (2U * OP_HUD_FIELD_PITCH_PX) + OP_HUD_ICON_COLS_PX + ICON_GAP_PX +
                (FIELD_DIGITS * DIGIT_PITCH_PX)) <= OP_OLED_WIDTH_PX,
               "three fields fit the panel width");

/* Each glyph row is a bit mask, most significant used bit = leftmost column. */
static const uint8_t DIGIT_FONT[DIGITS][OP_HUD_GLYPH_ROWS_PX] = {
    {0x7U, 0x5U, 0x5U, 0x5U, 0x7U}, /* 0 */
    {0x2U, 0x6U, 0x2U, 0x2U, 0x7U}, /* 1 */
    {0x7U, 0x1U, 0x7U, 0x4U, 0x7U}, /* 2 */
    {0x7U, 0x1U, 0x7U, 0x1U, 0x7U}, /* 3 */
    {0x5U, 0x5U, 0x7U, 0x1U, 0x1U}, /* 4 */
    {0x7U, 0x4U, 0x7U, 0x1U, 0x7U}, /* 5 */
    {0x7U, 0x4U, 0x7U, 0x5U, 0x7U}, /* 6 */
    {0x7U, 0x1U, 0x1U, 0x1U, 0x1U}, /* 7 */
    {0x7U, 0x5U, 0x7U, 0x5U, 0x7U}, /* 8 */
    {0x7U, 0x5U, 0x7U, 0x1U, 0x7U}, /* 9 */
};

static const uint8_t ICON_HEALTH[OP_HUD_GLYPH_ROWS_PX] = {0x04U, 0x04U, 0x1FU, 0x04U, 0x04U};
static const uint8_t ICON_KILLS[OP_HUD_GLYPH_ROWS_PX] = {0x0EU, 0x15U, 0x0EU, 0x0EU, 0x0AU};
static const uint8_t ICON_AMMO[OP_HUD_GLYPH_ROWS_PX] = {0x04U, 0x0EU, 0x0EU, 0x0EU, 0x0EU};

/** Draw a glyph @p cols wide at column x, glyph row 0 at frame row y. */
static void draw_glyph(uint8_t *bits, uint32_t w, uint32_t x, uint32_t y,
                       const uint8_t rows[OP_HUD_GLYPH_ROWS_PX], uint32_t cols)
{
    for (uint32_t r = 0U; r < OP_HUD_GLYPH_ROWS_PX; r++) {
        for (uint32_t col = 0U; col < cols; col++) {
            const uint32_t bit = (cols - 1U) - col;
            bits[((size_t)(y + r) * w) + x + col] = (uint8_t)((rows[r] >> bit) & 1U);
        }
    }
}

static uint32_t clamp_value(int32_t v)
{
    if (v < 0) {
        return 0U;
    }
    return (v > OP_HUD_VALUE_MAX) ? (uint32_t)OP_HUD_VALUE_MAX : (uint32_t)v;
}

/** Icon, then the value right aligned in three digit places (no leading zeros). */
static void draw_field(uint8_t *bits, uint32_t w, uint32_t x, uint32_t y,
                       const uint8_t icon[OP_HUD_GLYPH_ROWS_PX], int32_t value)
{
    draw_glyph(bits, w, x, y, icon, OP_HUD_ICON_COLS_PX);
    uint32_t v = clamp_value(value);
    const uint32_t digits_x = x + OP_HUD_ICON_COLS_PX + ICON_GAP_PX;
    for (uint32_t place = FIELD_DIGITS; place > 0U; place--) {
        const uint32_t dx = digits_x + ((place - 1U) * DIGIT_PITCH_PX);
        draw_glyph(bits, w, dx, y, DIGIT_FONT[v % (uint32_t)DECIMAL], OP_HUD_DIGIT_COLS_PX);
        v /= (uint32_t)DECIMAL;
        if (v == 0U) {
            break;
        }
    }
}

op_status_t op_hud_draw(const op_hud_t *hud, uint16_t w, uint16_t h, uint8_t *bits)
{
    if ((hud == NULL) || (bits == NULL)) {
        return OP_ERR_NULL;
    }
    if ((w != OP_OLED_WIDTH_PX) || (h < OP_HUD_ROWS_PX)) {
        return OP_ERR_SIZE;
    }
    const uint32_t top = (uint32_t)h - OP_HUD_ROWS_PX;
    for (uint32_t y = top; y < h; y++) {
        for (uint32_t x = 0U; x < w; x++) {
            bits[((size_t)y * w) + x] = (y == top) ? 1U : 0U; /* separator line, then blank */
        }
    }
    const uint32_t gy = top + GLYPH_TOP_OFFSET_PX;
    draw_field(bits, w, FIELD_X0_PX, gy, ICON_HEALTH, hud->health);
    draw_field(bits, w, FIELD_X0_PX + OP_HUD_FIELD_PITCH_PX, gy, ICON_KILLS, hud->kills);
    if (hud->show_ammo) {
        draw_field(bits, w, FIELD_X0_PX + (2U * OP_HUD_FIELD_PITCH_PX), gy, ICON_AMMO, hud->ammo);
    }
    return OP_OK;
}
