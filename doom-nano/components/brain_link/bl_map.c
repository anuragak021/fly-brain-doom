/**
 * @file bl_map.c
 * @brief Map memory for the navigation heuristics. See bl_map.h.
 */
#include "bl_map.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define OCTANT_SHIFT (29U) /* top 3 bits of a binary angle: 8 octants of 45 degrees */
#define BAM_PER_TURN (4294967296.0F)
#define TWO_PI (6.28318531F)
#define DEG_PER_TURN (360.0F)
#define CELL_FP (8388608.0F) /* 1 << BL_MAP_CELL_SHIFT */

/** Cell index of (x, y), or false if outside the grid. */
static bool cell_of(const bl_map_t *map, int32_t x, int32_t y, uint32_t *index)
{
    const int64_t dx = (int64_t)x - (int64_t)map->org_x;
    const int64_t dy = (int64_t)y - (int64_t)map->org_y;
    if ((dx < 0) || (dy < 0)) {
        return false;
    }
    const int64_t cx = dx >> BL_MAP_CELL_SHIFT;
    const int64_t cy = dy >> BL_MAP_CELL_SHIFT;
    if ((cx >= (int64_t)map->w) || (cy >= (int64_t)map->h)) {
        return false;
    }
    *index = ((uint32_t)cy * (uint32_t)map->w) + (uint32_t)cx;
    return true;
}

static uint8_t octant_bit(uint32_t angle)
{
    return (uint8_t)(1U << (angle >> OCTANT_SHIFT));
}

bl_status_t bl_map_reset(bl_map_t *map, int32_t org_x, int32_t org_y, int32_t w_cells,
                         int32_t h_cells)
{
    if (map == NULL) {
        return BL_ERR_NULL;
    }
    (void)memset(map, 0, sizeof(*map));
    map->org_x = org_x;
    map->org_y = org_y;
    if ((w_cells <= 0) || (h_cells <= 0)) {
        return BL_ERR_SIZE;
    }
    map->w = (uint16_t)(((uint32_t)w_cells > BL_MAP_MAX_W) ? BL_MAP_MAX_W : (uint32_t)w_cells);
    map->h = (uint16_t)(((uint32_t)h_cells > BL_MAP_MAX_H) ? BL_MAP_MAX_H : (uint32_t)h_cells);
    return BL_OK;
}

void bl_map_visit(bl_map_t *map, int32_t x, int32_t y, uint32_t tics)
{
    uint32_t i;
    if ((map == NULL) || !cell_of(map, x, y, &i)) {
        return;
    }
    const uint32_t v = (uint32_t)map->visits[i] + tics;
    map->visits[i] = (uint16_t)((v > BL_MAP_VISIT_MAX) ? BL_MAP_VISIT_MAX : v);
}

bool bl_map_probe_failed(const bl_map_t *map, int32_t x, int32_t y, uint32_t angle)
{
    uint32_t i;
    if ((map == NULL) || !cell_of(map, x, y, &i)) {
        return false;
    }
    return (map->failed[i] & octant_bit(angle)) != 0U;
}

void bl_map_probe_begin(bl_map_t *map, int32_t x, int32_t y, uint32_t angle)
{
    if (map == NULL) {
        return;
    }
    map->probing = true;
    map->probe_x = x;
    map->probe_y = y;
    map->probe_angle = angle;
    map->probe_tics = 0U;
}

void bl_map_probe_cancel(bl_map_t *map)
{
    if (map != NULL) {
        map->probing = false;
    }
}

static float heading_rad(uint32_t angle)
{
    return ((float)angle / BAM_PER_TURN) * TWO_PI;
}

bl_probe_result_t bl_map_probe_update(bl_map_t *map, int32_t x, int32_t y, uint32_t tics)
{
    if ((map == NULL) || !map->probing) {
        return BL_PROBE_PENDING;
    }
    map->probe_tics += (tics == 0U) ? 1U : tics;
    const float rad = heading_rad(map->probe_angle);
    const float dx = (float)((int64_t)x - (int64_t)map->probe_x);
    const float dy = (float)((int64_t)y - (int64_t)map->probe_y);
    const float ahead = (dx * cosf(rad)) + (dy * sinf(rad));
    if (ahead >= (float)BL_MAP_PROBE_ADVANCE_FP) {
        map->probing = false;
        map->passed_probes++;
        return BL_PROBE_PASSED;
    }
    if (map->probe_tics < BL_MAP_PROBE_EVAL_TICS) {
        return BL_PROBE_PENDING;
    }
    map->probing = false;
    uint32_t i;
    if (cell_of(map, map->probe_x, map->probe_y, &i)) {
        map->failed[i] |= octant_bit(map->probe_angle);
    }
    map->failed_probes++;
    return BL_PROBE_FAILED;
}

/** Visit tics summed along one ray of BL_MAP_EXPLORE_STEPS cells. */
static uint32_t ray_visits(const bl_map_t *map, int32_t x, int32_t y, float rad)
{
    uint32_t total = 0U;
    const float c = cosf(rad);
    const float s = sinf(rad);
    for (uint32_t k = 1U; k <= BL_MAP_EXPLORE_STEPS; k++) {
        const float dist = CELL_FP * (float)k;
        const float px = (float)x + (c * dist);
        const float py = (float)y + (s * dist);
        if ((fabsf(px) >= 2147483520.0F) || (fabsf(py) >= 2147483520.0F)) {
            continue; /* would not fit a map coordinate */
        }
        uint32_t i;
        if (cell_of(map, (int32_t)px, (int32_t)py, &i)) {
            total += map->visits[i];
        }
    }
    return total;
}

bool bl_map_explore(const bl_map_t *map, int32_t x, int32_t y, uint32_t angle, bool *left)
{
    if ((map == NULL) || (left == NULL)) {
        return false;
    }
    const float heading = heading_rad(angle);
    const float side = (BL_MAP_EXPLORE_DEG / DEG_PER_TURN) * TWO_PI;
    const uint32_t vl = ray_visits(map, x, y, heading + side); /* counter-clockwise = left */
    const uint32_t vr = ray_visits(map, x, y, heading - side);
    const uint32_t lo = (vl < vr) ? vl : vr;
    const uint32_t hi = (vl < vr) ? vr : vl;
    if (((hi - lo) < BL_MAP_EXPLORE_MIN_DIFF) || ((float)hi < (BL_MAP_EXPLORE_RATIO * (float)lo))) {
        return false;
    }
    *left = (vl < vr);
    return true;
}
