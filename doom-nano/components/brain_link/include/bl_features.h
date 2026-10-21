/**
 * @file bl_features.h
 * @brief Brain feature vector from the OLED pipeline's view of a Doom frame. Pure logic.
 *
 * Same six features, in the same order and with the same arithmetic as game/features.py in the
 * drosophila project (the brain's inputs must mean the same on the laptop and on the ESP32):
 *   [luminance_left, luminance_right, enemy_left, enemy_right, damage_left, damage_right],
 * each in 0..1. The first four come from the rendered frame (bl_features_make); the damage pair
 * comes from the engine's player state (bl_damage_make), see brain_protocol.h for its definition.
 *
 * Differences from the ViZDoom version, all deliberate and listed in the project NOTES.md:
 *  - luminance is taken from the 128x56 gray view the OLED shows (after the pipeline's black and
 *    white point levels), not from ViZDoom's raw 160x120 gray buffer;
 *  - the enemy is the largest live monster in the engine's sprite id map (by bounding box area),
 *    not a ViZDoom label. Both are engine ground truth, not a vision pipeline.
 */
#ifndef BL_FEATURES_H
#define BL_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

/** Status codes; BL_OK is zero. */
typedef enum { BL_OK = 0, BL_ERR_NULL = 1, BL_ERR_SIZE = 2 } bl_status_t;

#define BL_N_FEATURES (6U) /**< Equals BRAIN_N_FEATURES (checked in bl_client.c). */
#define BL_FEAT_LUM_LEFT (0U)
#define BL_FEAT_LUM_RIGHT (1U)
#define BL_FEAT_ENEMY_LEFT (2U)
#define BL_FEAT_ENEMY_RIGHT (3U)
#define BL_FEAT_DAMAGE_LEFT (4U)
#define BL_FEAT_DAMAGE_RIGHT (5U)
#define BL_GRAY_MAX (255.0F)

/** Wall in the centre columns covering at least this fraction of the view height = wall ahead. */
#define BL_WALL_AHEAD_FRAC (0.55F)

/** Features plus what the heuristics need (the enemy's position, walls). Only `feature` is sent
 *  to the brain. */
typedef struct {
    float feature[BL_N_FEATURES]; /**< The vector sent to the brain. */
    bool enemy_visible;           /**< A live monster is on screen. */
    float enemy_offset;           /**< -1 (left edge) .. +1 (right edge); 0 if not visible. */
    float wall_centre;            /**< Mean wall coverage of the centre quarter of columns, 0..1. */
    bool wall_ahead;              /**< wall_centre >= BL_WALL_AHEAD_FRAC. */
    bool open_left;               /**< The left half has less wall than the right half. */
} bl_features_t;

/**
 * @brief Build the feature vector for one rendered frame.
 *
 * Luminance: mean of the left w/2 columns and of the right w/2 columns, divided by 255.
 * Enemy: among ids whose class is OP_SPR_MONSTER or OP_SPR_MONSTER_HIT, the one with the largest
 * bounding box (first id wins a tie); offset = clip(2 * centre_x / w - 1, -1, 1) where centre_x
 * is the box's left edge plus half its width; enemy_left = max(-offset, 0), enemy_right =
 * max(offset, 0). No such id: both enemy features are 0 and enemy_visible is false.
 *
 * @param[in]  gray  w * h gray bytes (row stride w).
 * @param[in]  ids   w * h sprite id bytes (0 = no sprite), same layout as gray.
 * @param[in]  cls   256 entries: op_spr_class_t of each id.
 * @param[in]  w     Width in pixels, at least 2.
 * @param[in]  h     Height in pixels, at least 1.
 * @param[out] out   Result.
 * @return BL_OK, BL_ERR_NULL or BL_ERR_SIZE.
 */
bl_status_t bl_features_make(const uint8_t *gray, const uint8_t *ids, const uint8_t *cls,
                             uint16_t w, uint16_t h, bl_features_t *out);

/**
 * @brief Fill the wall fields of @p out from the wall coverage map (heuristics only).
 *
 * Near walls are tall on screen, so a centre band mostly covered by wall means a wall close ahead.
 * Floors and ceilings are not walls, so the half with less wall is the more open direction.
 *
 * @param[in]     wall_cov  w * h coverage bytes, 0..255 (255 = all wall), row stride w.
 * @param[in]     w         Width, at least 8.
 * @param[in]     h         Height, at least 1.
 * @param[in,out] out       Wall fields are written; the rest is left alone.
 * @return BL_OK, BL_ERR_NULL or BL_ERR_SIZE.
 */
bl_status_t bl_walls_make(const uint8_t *wall_cov, uint16_t w, uint16_t h, bl_features_t *out);

/** damagecount giving full pain (p = 1); see brain_protocol.h, DAMAGE_LEFT/RIGHT. */
#define BL_DAMAGE_FULL_COUNT (25.0F)
/** An attacker this many degrees off the view axis (or more) counts fully to its side. */
#define BL_DAMAGE_SIDE_DEG (90.0F)

/**
 * @brief The damage direction features (brain_protocol.h: DAMAGE_LEFT, DAMAGE_RIGHT).
 *
 * p = min(damagecount / BL_DAMAGE_FULL_COUNT, 1); a = the attacker's direction relative to the
 * view in degrees, positive to the left; s = clamp(a / 90, -1, 1); left = p * max(s, 0),
 * right = p * max(-s, 0).
 *
 * @param[in]  rel_bam      Attacker direction minus view direction as a Doom angle (binary
 *                          angle: 2^32 = 360 degrees, counter-clockwise = left), unsigned and
 *                          wrapped, exactly as R_PointToAngle2(...) - mo->angle gives it.
 * @param[in]  damagecount  The player's damagecount; <= 0 means not in pain (both 0).
 * @param[out] left         DAMAGE_LEFT, 0..1.
 * @param[out] right        DAMAGE_RIGHT, 0..1.
 * @return BL_OK or BL_ERR_NULL.
 */
bl_status_t bl_damage_make(uint32_t rel_bam, int32_t damagecount, float *left, float *right);

#endif /* BL_FEATURES_H */
