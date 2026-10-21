/**
 * @file bl_map.h
 * @brief Map memory for the navigation heuristics. Pure logic. HEURISTIC, not the connectome.
 *
 * A per-map grid over the level's blockmap (128 x 128 map unit cells) that remembers:
 *  - how long the player has spent in each cell (visit tics), so wall turns can prefer the side
 *    the player has seen less of (explore instead of circling one room);
 *  - door probes that failed (the player walked at a wall pressing use and did not get through),
 *    by cell and heading octant, so the same plain wall is not probed again.
 * The memory lives for one map: it survives the player's deaths and respawns on that map and is
 * cleared when a different map is loaded. Nothing here is learned by the brain; it is ordinary
 * bookkeeping on the ESP32, labelled as a heuristic in the README.
 *
 * Positions are Doom 16.16 fixed point map coordinates; angles are Doom binary angles (2^32 = one
 * turn, counter-clockwise, 0 = east).
 */
#ifndef BL_MAP_H
#define BL_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "bl_features.h"

#define BL_MAP_CELL_SHIFT (23)   /**< 16.16 fixed to 128-unit cells (PrBoom MAPBLOCKSHIFT). */
#define BL_MAP_MAX_W (128U)      /**< Largest grid kept; cells beyond it are not remembered. */
#define BL_MAP_MAX_H (128U)
#define BL_MAP_VISIT_MAX (65535U) /**< Visit tics saturate here. */
/** A probe gets through when the player gets this far (16.16, 48 map units) along the probe's
 *  heading within BL_MAP_PROBE_EVAL_TICS of its start (a door needs about 30 tics to open). */
#define BL_MAP_PROBE_ADVANCE_FP (48 * 65536)
#define BL_MAP_PROBE_EVAL_TICS (70U)
/** Explore rays: this far either side of the heading, sampled at 1, 2 and 3 cells. */
#define BL_MAP_EXPLORE_DEG (60.0F)
#define BL_MAP_EXPLORE_STEPS (3U)
/** A side counts as less visited when the other side has at least this many more visit tics and
 *  at least BL_MAP_EXPLORE_RATIO times as many. */
#define BL_MAP_EXPLORE_MIN_DIFF (70U) /* 2 s of game time */
#define BL_MAP_EXPLORE_RATIO (1.5F)

/** Map memory. Large (about 48 KB): keep one static instance (PSRAM on the ESP32). */
typedef struct {
    int32_t org_x; /**< Blockmap origin, 16.16. */
    int32_t org_y;
    uint16_t w; /**< Cells kept (at most BL_MAP_MAX_W). */
    uint16_t h;
    uint16_t visits[BL_MAP_MAX_W * BL_MAP_MAX_H]; /**< Tics spent per cell (saturating). */
    uint8_t failed[BL_MAP_MAX_W * BL_MAP_MAX_H];  /**< Bit k: a probe heading octant k failed. */
    bool probing;                                 /**< A probe is in progress. */
    int32_t probe_x;                              /**< Where it started, 16.16. */
    int32_t probe_y;
    uint32_t probe_angle; /**< Heading at its start. */
    uint32_t probe_tics;  /**< Tics since it started. */
    uint32_t failed_probes;
    uint32_t passed_probes;
} bl_map_t;

/** Outcome of a probe, from bl_map_probe_update(). */
typedef enum {
    BL_PROBE_PENDING = 0, /**< Still being judged, or no probe. */
    BL_PROBE_PASSED = 1,  /**< The player got through (a door opened or the way was open). */
    BL_PROBE_FAILED = 2,  /**< Did not get through in time; remembered. */
} bl_probe_result_t;

/**
 * @brief Forget everything and set the grid for a new map.
 * @param[out] map      Memory.
 * @param[in]  org_x    Blockmap origin x, 16.16 (PrBoom bmaporgx).
 * @param[in]  org_y    Blockmap origin y, 16.16 (bmaporgy).
 * @param[in]  w_cells  Blockmap width in cells (bmapwidth); clipped to BL_MAP_MAX_W.
 * @param[in]  h_cells  Blockmap height in cells (bmapheight); clipped to BL_MAP_MAX_H.
 * @return BL_OK, BL_ERR_NULL, or BL_ERR_SIZE if either size is 0 (the grid is then empty).
 */
bl_status_t bl_map_reset(bl_map_t *map, int32_t org_x, int32_t org_y, int32_t w_cells,
                         int32_t h_cells);

/** @brief Count @p tics game tics spent at (x, y). Positions outside the grid are ignored. */
void bl_map_visit(bl_map_t *map, int32_t x, int32_t y, uint32_t tics);

/** @brief True if a probe from this cell with this heading octant failed before. */
bool bl_map_probe_failed(const bl_map_t *map, int32_t x, int32_t y, uint32_t angle);

/** @brief A probe starts at (x, y) facing @p angle. */
void bl_map_probe_begin(bl_map_t *map, int32_t x, int32_t y, uint32_t angle);

/**
 * @brief Judge the running probe when game time advances.
 *
 * Progress is the displacement from the probe's start projected on its heading (backing up
 * does not count). Passed as soon as it reaches BL_MAP_PROBE_ADVANCE_FP; failed (start cell and
 * heading octant remembered) when BL_MAP_PROBE_EVAL_TICS pass first.
 *
 * @param[in,out] map   Memory.
 * @param[in]     x     Player x now, 16.16.
 * @param[in]     y     Player y now, 16.16.
 * @param[in]     tics  Game tics since the last call; 0 is treated as 1.
 * @return The outcome; BL_PROBE_PENDING if none is running or it is still undecided.
 */
bl_probe_result_t bl_map_probe_update(bl_map_t *map, int32_t x, int32_t y, uint32_t tics);

/** @brief Drop the running probe without judging it (a monster appeared, death, new map). */
void bl_map_probe_cancel(bl_map_t *map);

/**
 * @brief Which side, left or right of the heading, the player has spent less time on.
 *
 * Sums visit tics over cells 1..BL_MAP_EXPLORE_STEPS cells away along rays BL_MAP_EXPLORE_DEG to
 * the left and to the right of @p angle.
 *
 * @param[in]  map    Memory.
 * @param[in]  x      Player x, 16.16.
 * @param[in]  y      Player y, 16.16.
 * @param[in]  angle  Heading.
 * @param[out] left   True if the left side is less visited (valid when true is returned).
 * @return true if one side is clearly less visited (see BL_MAP_EXPLORE_MIN_DIFF, _RATIO).
 */
bool bl_map_explore(const bl_map_t *map, int32_t x, int32_t y, uint32_t angle, bool *left);

#endif /* BL_MAP_H */
