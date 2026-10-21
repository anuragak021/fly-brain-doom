/**
 * @file bl_policy.h
 * @brief From the brain's reply to Doom controls, plus the heuristics around it. Pure logic.
 *
 * Who turns, in order of priority (each turn is tagged with its source for the logs):
 *  1. CONNECTOME: whenever a live monster is on screen, only the brain's action bits turn the
 *     player. No heuristic overrides it.
 *  1b. CONNECTOME (pain): no monster on screen but the player was just hurt (damage features sum
 *     to at least BL_PAIN_MIN) and the brain turns: its turn wins over the heuristics below. If
 *     the brain does not turn, the heuristics run as usual.
 *  2. HEURISTIC unstick: forward walking made no progress for BL_STUCK_TICS game tics; back up
 *     and turn about 150 degrees, pressing use (doors). A repeat within BL_UNSTICK_REPEAT_TICS
 *     turns the same way again (escalate); otherwise the direction alternates.
 *  3. HEURISTIC scan: no monster seen for BL_SCAN_AFTER_TICS; turn in place one full circle
 *     (monsters are often behind the player). Ends early as soon as a monster is on screen.
 *  4. HEURISTIC door probe: a wall ahead and no probe in the last BL_PROBE_COOLDOWN_TICS; walk
 *     straight at it for BL_PROBE_TICS pressing use. A door opens (use reaches 64 map units, and
 *     wall avoidance would turn away long before that); a plain wall leaves the player stuck and
 *     unstick backs it out.
 *  5. HEURISTIC wall avoidance: a wall close ahead (bl_walls_make); turn toward the side the map
 *     memory says is less visited (bl_map_explore, tagged BL_TURN_EXPLORE) or, when it has no
 *     clear answer, toward the open side on screen (BL_TURN_WALL).
 *  6. CONNECTOME: the brain's action bits when nothing above applies (rare: without a monster its
 *     outputs mostly stay inside the dead zone).
 * Turn rates come from PrBoom's keyboard turning (g_game.c, autorun off): angleturn 320 (1.76
 * degrees) per tic for the first SLOWTURNTICS = 6 tics, then 640 (3.52 degrees) per tic.
 * Also HEURISTIC, not the connectome: fire when the largest monster is within BL_FIRE_CONE of
 * the screen centre (same rule as FIRE_CONE in game/play.py); walk forward when not aiming; use to
 * respawn and to leave intermission screens (the caller's side).
 */
#ifndef BL_POLICY_H
#define BL_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "bl_features.h"

#define BL_FIRE_CONE (0.15F)          /**< |enemy offset| below this counts as aimed. */
#define BL_STUCK_TICS (35U)           /**< One second of game time (35 tics) without moving. */
#define BL_UNSTICK_TICS (46U)         /**< 6 x 1.76 + 40 x 3.52 = about 151 degrees. */
#define BL_UNSTICK_REPEAT_TICS (105U) /**< Stuck again within 3 s: keep the same direction. */
#define BL_SCAN_AFTER_TICS (280U)     /**< 8 s of game time without a monster on screen. */
#define BL_SCAN_TICS (106U)           /**< 6 x 1.76 + 100 x 3.52 = about 363 degrees. */
#define BL_PROBE_TICS (35U)           /**< 1 s walking at a wall pressing use. */
#define BL_PROBE_COOLDOWN_TICS (140U) /**< At most one probe every 4 s (from its start). */
#define BL_STUCK_MIN_MOVE_FP (65536)  /**< 16.16 map units per tic: below this is "not moving". */
#define BL_PAIN_MIN (0.1F) /**< Damage features summing to this count as "hurt" (rule 1b). */

/** Where a turn came from. */
typedef enum {
    BL_TURN_NONE = 0,
    BL_TURN_BRAIN = 1,   /**< The connectome's action bits. */
    BL_TURN_UNSTICK = 2, /**< Heuristic. */
    BL_TURN_WALL = 3,    /**< Heuristic. */
    BL_TURN_SCAN = 4,    /**< Heuristic. */
    BL_TURN_PAIN = 5,    /**< The connectome's action bits while hurt (rule 1b). */
    BL_TURN_EXPLORE = 6, /**< Heuristic: wall turn toward the less visited side (map memory). */
    BL_TURN_SOURCES = 7, /**< Count, for tables indexed by source. */
} bl_turn_source_t;

/** Map memory's advice for this tic (bl_map_explore). */
typedef struct {
    bool explore_known; /**< One side is clearly less visited. */
    bool explore_left;  /**< That side is the left (valid when explore_known). */
} bl_hint_t;

/** The keys to hold this tic. */
typedef struct {
    bool turn_left;
    bool turn_right;
    bool fire;
    bool forward;
    bool back;
    bool use;
    bl_turn_source_t source;
} bl_controls_t;

/** Stuck detector state. */
typedef struct {
    int32_t last_x;
    int32_t last_y;
    bool have_last;
    uint32_t still_tics;
    uint32_t unstick_left; /**< Tics of the manoeuvre still to run. */
    uint32_t since_end;    /**< Tics since the last manoeuvre ended (saturates). */
    bool turn_left;        /**< Direction of the current manoeuvre. */
} bl_stuck_t;

/** Scan state: how long since a monster was seen, and the scan in progress. */
typedef struct {
    uint32_t unseen_tics;
    uint32_t scan_left; /**< Tics of the scan still to run. */
    bool turn_left;     /**< Direction of the current scan. */
} bl_scan_t;

/** Door probe state. */
typedef struct {
    uint32_t probe_left; /**< Tics of the probe still to run. */
    uint32_t cooldown;   /**< Tics before the next probe may start. */
} bl_probe_t;

/** @brief Clear the detector (new level, respawn). */
void bl_stuck_reset(bl_stuck_t *st);

/**
 * @brief Feed the player's position when game time advances.
 * @param[in,out] st              Detector.
 * @param[in]     x               Player x in 16.16 map units.
 * @param[in]     y               Player y in 16.16 map units.
 * @param[in]     wanted_forward  Forward walking was commanded since the last call.
 * @param[in]     tics            Game tics since the last call (the engine may run several per
 *                                call); 0 is treated as 1.
 * @return true while the unstick manoeuvre is running.
 */
bool bl_stuck_update(bl_stuck_t *st, int32_t x, int32_t y, bool wanted_forward, uint32_t tics);

/** @brief Clear the scan state (new level, respawn). */
void bl_scan_reset(bl_scan_t *sc);

/**
 * @brief Advance the scan timer when game time advances.
 * @param[in,out] sc             Scan state.
 * @param[in]     enemy_visible  A monster is on screen (stops a running scan).
 * @param[in]     tics           Game tics since the last call; 0 is treated as 1.
 * @return true while a scan is running.
 */
bool bl_scan_update(bl_scan_t *sc, bool enemy_visible, uint32_t tics);

/** @brief Clear the probe state (new level, respawn). */
void bl_probe_reset(bl_probe_t *pr);

/**
 * @brief Advance the door probe when game time advances.
 * @param[in,out] pr             Probe state.
 * @param[in]     wall_ahead     A wall close ahead (starts a probe when the cooldown is over).
 * @param[in]     enemy_visible  A monster is on screen (stops a probe; the brain turns then).
 * @param[in]     tics           Game tics since the last call; 0 is treated as 1.
 * @return true while a probe is running.
 */
bool bl_probe_update(bl_probe_t *pr, bool wall_ahead, bool enemy_visible, uint32_t tics);

/**
 * @brief Decide the controls for one tic.
 * @param[in]  actions  BRAIN_ACTION_* bits from the last STEP reply.
 * @param[in]  view     Features and wall information of the latest 3D frame, with the damage
 *                      features of this tic.
 * @param[in]  hint     Map memory's advice.
 * @param[in]  stuck    Detector state after bl_stuck_update().
 * @param[in]  scan     Scan state after bl_scan_update().
 * @param[in]  probe    Probe state after bl_probe_update().
 * @param[out] out      Controls.
 * @return BL_OK or BL_ERR_NULL.
 */
bl_status_t bl_policy_decide(uint8_t actions, const bl_features_t *view, const bl_hint_t *hint,
                             const bl_stuck_t *stuck, const bl_scan_t *scan,
                             const bl_probe_t *probe, bl_controls_t *out);

#endif /* BL_POLICY_H */
