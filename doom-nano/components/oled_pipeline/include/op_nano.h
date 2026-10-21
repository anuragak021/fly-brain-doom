/**
 * @file op_nano.h
 * @brief Stage 3 alternative: "nano" style, the look of the doom-nano raycaster on real Doom.
 *
 * doom-nano (an Arduino raycaster for the same 128x64 SSD1306) is readable because it draws
 * very little: every wall column is one flat shade of a few Bayer levels with its top and
 * bottom pixel lit, floors and ceilings are black, and sprites are crisp shapes. This module
 * reproduces that look from a real Doom frame with engine masks (op_mask): wall coverage tells
 * which pixels are walls, and a sprite id map tells which sprite each pixel shows, front to
 * back, and what kind of sprite it is. Only the look is reproduced; no doom-nano code is used.
 * See NOTES.md "Nano style" and "Nano sprites".
 *
 * Walls: each vertical run of wall pixels in a column, cut where the gray jumps by more than
 * split_threshold (stacked walls, steps), is shaded with its mean gray mapped to
 * clamp((mean - black) * gain_x16 / 16), quantized to @c levels levels (at least
 * @c min_level, so far walls never vanish) and Bayer dithered; with wall_edges the first and
 * last pixel of each run is on. Floors, ceilings and sky are off.
 *
 * Sprites (pixel with coverage >= cov_threshold and a nonzero id), drawn by class:
 * - monster / thing: contrast chosen against the background around the sprite. On a dark
 *   background a light body (gray > sprite_threshold, dark details carved out); on a mostly
 *   lit background a dark body with a lit rim and highlights. A monster that was just hit is
 *   drawn in the opposite mode (the hit flash).
 * - corpse: sparse dots and a rim, so the dead recede; item: rim and its brighter pixels;
 *   missile, puff, blood: solid on; weapon: Bayer of its gray with a rim, muzzle flash solid;
 *   masked mid texture (grates): on if gray > sprite_threshold.
 * - Every sprite except mid textures casts a one pixel black halo onto whatever is behind it:
 *   walls, floors and sprites with a lower id. So the gun is cut out of a monster behind it,
 *   and two overlapping monsters stay apart.
 */
#ifndef OP_NANO_H
#define OP_NANO_H

#include <stdbool.h>
#include <stdint.h>

#include "op_types.h"

#define OP_NANO_DEFAULT_LEVELS (5U)            /**< Wall shades, black and white included. */
#define OP_NANO_DEFAULT_GAIN_X16 (32U)         /**< Wall contrast x2. */
#define OP_NANO_DEFAULT_BLACK (12U)            /**< Gray mapped to level 0 before the gain. */
#define OP_NANO_DEFAULT_MIN_LEVEL (1U)         /**< Darkest wall shade still drawn. */
#define OP_NANO_DEFAULT_SPLIT_THRESHOLD (40U)  /**< Gray jump that starts a new wall run. */
#define OP_NANO_DEFAULT_SPRITE_THRESHOLD (24U) /**< Light sprite body: gray above this is on. */
#define OP_NANO_DEFAULT_COV_THRESHOLD (128U)   /**< Mask coverage that counts as set. */
#define OP_NANO_GAIN_DIV (16U)
#define OP_NANO_MIN_LEVELS (2U)
#define OP_NANO_MAX_PIXELS (OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX)

#define OP_NANO_IDS (256U)           /**< Sprite ids are bytes; 0 = no sprite. */
#define OP_NANO_ID_WEAPON (255U)     /**< Id of the player's weapon (always in front). */
#define OP_NANO_ID_LAST_THING (254U) /**< Highest id for world sprites. */

/** What a sprite id shows; decides how it is drawn. */
typedef enum {
    OP_SPR_NONE = 0,        /**< Unused id. */
    OP_SPR_MID = 1,         /**< Masked mid texture (grates, bars). No halo. */
    OP_SPR_THING = 2,       /**< Decoration, barrel, anything else: adaptive contrast. */
    OP_SPR_MONSTER = 3,     /**< Live monster: adaptive contrast. */
    OP_SPR_MONSTER_HIT = 4, /**< Monster damaged in the last few tics: opposite contrast. */
    OP_SPR_CORPSE = 5,      /**< Dead or dying monster. */
    OP_SPR_ITEM = 6,        /**< Pickup. */
    OP_SPR_FX = 7,          /**< Missile, bullet puff, blood: solid. */
    OP_SPR_WEAPON = 8,      /**< Player weapon and its muzzle flash. */
    OP_SPR_CLASSES = 9
} op_spr_class_t;

/** Contrast mode of an adaptive sprite, kept per sprite between frames for hysteresis. */
typedef enum {
    OP_SPR_MODE_UNKNOWN = 0, /**< First frame: decided at 50% lit background. */
    OP_SPR_MODE_LIGHT = 1,   /**< Light body on a dark background. */
    OP_SPR_MODE_DARK = 2,    /**< Dark body with a lit rim on a bright background. */
} op_spr_mode_t;

/** Sprite inputs of one view, on the view grid (w * h) or indexed by id. */
typedef struct {
    const uint8_t *cov;       /**< w * h coverage of any sprite (op_mask_coverage_cfg). */
    const uint8_t *id;        /**< w * h front-most sprite id per pixel (op_mask_ids). */
    const uint8_t *cls;       /**< OP_NANO_IDS entries: op_spr_class_t of each id. */
    const uint8_t *prev_mode; /**< OP_NANO_IDS op_spr_mode_t from the last frame, or NULL. */
    uint8_t *mode;            /**< Out, OP_NANO_IDS entries: mode chosen this frame, or NULL. */
} op_nano_sprites_t;

/** What the engine knows about a drawn sprite; op_nano_classify turns it into a class. */
typedef struct {
    bool weapon;  /**< Player sprite (weapon, muzzle flash). */
    bool fx;      /**< Missile (MF_MISSILE), bullet puff or blood. */
    bool corpse;  /**< MF_CORPSE: set when the death animation starts. */
    bool still;   /**< Current state never ends (tics == -1): the final corpse frame. */
    bool monster; /**< Counts as a kill (MF_COUNTKILL) or is a lost soul. */
    bool hit;     /**< Damaged in the last few tics (the platform keeps the timer). */
    bool item;    /**< Pickup (MF_SPECIAL). */
} op_spr_facts_t;

/** Nano style parameters. */
typedef struct {
    uint8_t levels;           /**< Wall shades, >= OP_NANO_MIN_LEVELS. */
    uint8_t gain_x16;         /**< Wall contrast in sixteenths. */
    uint8_t black;            /**< Gray subtracted before the gain. */
    uint8_t min_level;        /**< Lowest wall shade index, < levels. */
    uint8_t split_threshold;  /**< Gray step between rows that cuts a run; 0 = never cut. */
    uint8_t sprite_threshold; /**< Light sprite body / mid texture pixel on if gray > this. */
    uint8_t cov_threshold;    /**< Coverage at or above which a pixel is wall / sprite, >= 1. */
    bool wall_edges;          /**< Light the first and last pixel of every wall run. */
} op_nano_cfg_t;

/**
 * @brief Fill @p cfg with the OP_NANO_DEFAULT_* values (wall_edges on).
 * @return OP_OK or OP_ERR_NULL.
 */
op_status_t op_nano_cfg_default(op_nano_cfg_t *cfg);

/**
 * @brief Class of a sprite from engine facts, in this order: weapon, fx, corpse, monster, item,
 *        anything else is a thing. A corpse still falling (MF_CORPSE but not yet still) is drawn
 *        as MONSTER_HIT, so a kill reads as a flash and the dots appear once the body is down.
 * @param facts What the engine knows about the sprite.
 * @return The op_spr_class_t; OP_SPR_NONE if @p facts is NULL.
 */
op_spr_class_t op_nano_classify(const op_spr_facts_t *facts);

/**
 * @brief Render one view in the nano style.
 * @param gray      w * h gray pixels (op_scale_area output).
 * @param wall_cov  w * h wall coverage (op_mask_wall_coverage) on the same grid.
 * @param spr       Sprite inputs on the same grid; cov, id and cls are required.
 * @param w         Width, 1..OP_MAX_DST_WIDTH_PX.
 * @param h         Height, >= 1, with w * h <= OP_NANO_MAX_PIXELS.
 * @param cfg       Parameters (see op_nano_cfg_t for the limits).
 * @param bits      Output, w * h bytes of 0/1.
 * @return OP_OK, OP_ERR_NULL, OP_ERR_SIZE or OP_ERR_RANGE.
 * @note Uses static scratch tables: not reentrant (the pipeline runs in one task).
 */
op_status_t op_nano(const uint8_t *gray, const uint8_t *wall_cov, const op_nano_sprites_t *spr,
                    uint16_t w, uint16_t h, const op_nano_cfg_t *cfg, uint8_t *bits);

#endif /* OP_NANO_H */
