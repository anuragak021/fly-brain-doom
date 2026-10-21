/**
 * @file i_sprite_ids.h
 * @brief Sprite id map for the nano OLED style (platform side of the r_things.c patches).
 *
 * While R_DrawMasked runs, the engine reports each sprite it starts (I_SpriteThing), every
 * column post it draws (I_SpritePost) and the end of each sprite (I_SpriteDone). This module
 * gives each sprite an id in drawing order (back to front, so a larger id is nearer), writes that
 * id over the post's rows in a map of the crop window, and records the id's class
 * (op_nano_classify). Posts drawn outside a sprite are masked mid textures (grates); each run
 * of them between two sprites gets its own id. The player's weapon is always OP_NANO_ID_WEAPON.
 *
 * Per mobj it keeps, across frames, the contrast mode op_nano chose (for hysteresis) and a hit
 * timer set by I_ThingDamaged (the hit flash). The table holds pointers only for comparison,
 * never dereferenced later; it is cleared when leveltime goes back (new level or loaded game).
 */
#ifndef I_SPRITE_IDS_H
#define I_SPRITE_IDS_H

#include <stdint.h>

#include "p_mobj.h"

/**
 * @brief Start recording a frame: clears @p map and the id tables.
 * @param map     Crop window id map, OP_DOOM_SRC_W * rows bytes (framebuffer rows
 *                win_y0 .. win_y0 + rows - 1).
 * @param win_y0  First framebuffer row of the window.
 * @param rows    Rows in the window.
 */
void sid_begin(uint8_t *map, uint32_t win_y0, uint32_t rows);

/** @brief Stop recording (end of R_DrawMasked); later engine calls are ignored. */
void sid_end(void);

/** @return OP_NANO_IDS entries: op_spr_class_t of each id of the last recorded frame. */
const uint8_t *sid_classes(void);

/** @return OP_NANO_IDS entries: each id's mobj mode from earlier frames (op_spr_mode_t). */
const uint8_t *sid_prev_modes(void);

/**
 * @brief Keep the modes op_nano chose for this frame's ids with their mobjs.
 * @param mode OP_NANO_IDS entries (op_nano_sprites_t.mode output).
 */
void sid_store_modes(const uint8_t *mode);

/* Engine hooks (doom-esp32 patches in r_things.c and p_inter.c). */

/**
 * @brief A sprite starts. @p thing is NULL for the weapon. Fuzz-drawn world sprites
 *        (spectres, invisible players) get no id, so they stay as hard to see as in Doom.
 */
void I_SpriteThing(const mobj_t *thing, int is_weapon, int is_fuzz);

/** @brief One column post of the current sprite or mid texture, in view coordinates. */
void I_SpritePost(int x, int yl, int yh);

/** @brief The current sprite ends; posts until the next sprite are mid textures. */
void I_SpriteDone(void);

/** @brief @p target takes damage: monsters get the hit flash for a few tics. */
void I_ThingDamaged(const mobj_t *target);

#endif /* I_SPRITE_IDS_H */
