/**
 * @file op_hud.h
 * @brief A one line HUD (health, kills, ammo) for the bottom OP_HUD_ROWS_PX rows of the panel.
 *
 * Doom's 32 row status bar is unreadable at 128x64, so the nano layout drops it from the crop
 * and draws the three numbers that matter with a 3x5 digit font and 5x5 icons (cross, skull,
 * bullet). Font and icons are drawn for this project. Layout of the HUD rows: a separator
 * line, one blank row, five rows of glyphs, one blank row.
 */
#ifndef OP_HUD_H
#define OP_HUD_H

#include <stdbool.h>
#include <stdint.h>

#include "op_types.h"

#define OP_HUD_VALUE_MAX (999)      /**< Values are clamped to 0..999 (three digits). */
#define OP_HUD_GLYPH_ROWS_PX (5U)   /**< Digit and icon height. */
#define OP_HUD_DIGIT_COLS_PX (3U)   /**< Digit width; digits are spaced one column apart. */
#define OP_HUD_ICON_COLS_PX (5U)    /**< Icon width. */
#define OP_HUD_FIELD_PITCH_PX (42U) /**< Horizontal distance between the three fields. */

/** What the HUD shows. */
typedef struct {
    int32_t health; /**< Player health; negative (gibbed) shows 0. */
    int32_t kills;  /**< Monsters killed (the brain's score). */
    int32_t ammo;   /**< Ammo of the ready weapon. */
    bool show_ammo; /**< False for weapons without ammo (fist, chainsaw): field left empty. */
} op_hud_t;

/**
 * @brief Draw the HUD into the bottom OP_HUD_ROWS_PX rows of a 0/1 pixel frame.
 * @param hud   Values to show.
 * @param w     Frame width, must be OP_OLED_WIDTH_PX.
 * @param h     Frame height, >= OP_HUD_ROWS_PX.
 * @param bits  w * h bytes of 0/1; the HUD rows are overwritten, the rest is untouched.
 * @return OP_OK, OP_ERR_NULL or OP_ERR_SIZE.
 */
op_status_t op_hud_draw(const op_hud_t *hud, uint16_t w, uint16_t h, uint8_t *bits);

#endif /* OP_HUD_H */
