/** @file test_bl_map.c @brief Map memory: grid, visits, probe judging, explore side. */
#include <stdbool.h>
#include <stdint.h>

#include "bl_map.h"
#include "test_runner.h"

#define FP (65536)
#define CELL (128 * FP)
#define ORG (-1024 * FP)    /* blockmap origin, like a real map's negative corner */
#define EAST (0U)           /* Doom angles: counter-clockwise from east */
#define NORTH (0x40000000U) /* 90 degrees */
#define WEST (0x80000000U)

static bl_map_t g_map; /* large: static, as on the ESP32 */

/** Centre of cell (cx, cy) in 16.16. */
static int32_t at(int32_t c)
{
    return ORG + (c * CELL) + (CELL / 2);
}

static void test_reset_and_grid(void)
{
    CHECK_EQ_INT(bl_map_reset(NULL, 0, 0, 4, 4), BL_ERR_NULL);
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 0, 4), BL_ERR_SIZE);
    CHECK_EQ_INT(g_map.w, 0);
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 500, 3), BL_OK);
    CHECK_EQ_INT(g_map.w, BL_MAP_MAX_W); /* clipped */
    CHECK_EQ_INT(g_map.h, 3);
    /* outside the grid: ignored, never written */
    bl_map_visit(&g_map, ORG - 1, at(0), 10U);
    bl_map_visit(&g_map, at(0), at(3), 10U);
    bl_map_visit(NULL, at(0), at(0), 10U);
    uint32_t sum = 0U;
    for (uint32_t i = 0U; i < (BL_MAP_MAX_W * BL_MAP_MAX_H); i++) {
        sum += g_map.visits[i];
    }
    CHECK_EQ_INT(sum, 0);
    /* inside: counted and saturating */
    bl_map_visit(&g_map, at(2), at(1), 30U);
    bl_map_visit(&g_map, at(2) + (CELL / 3), at(1) - (CELL / 3), 5U); /* same cell */
    CHECK_EQ_INT(g_map.visits[(1U * g_map.w) + 2U], 35);
    bl_map_visit(&g_map, at(2), at(1), 70000U);
    CHECK_EQ_INT(g_map.visits[(1U * g_map.w) + 2U], BL_MAP_VISIT_MAX);
}

static void test_probe_passes_when_the_player_gets_through(void)
{
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 16, 16), BL_OK);
    CHECK_EQ_INT(bl_map_probe_update(&g_map, 0, 0, 1U), BL_PROBE_PENDING); /* none running */
    bl_map_probe_begin(&g_map, at(4), at(4), EAST);
    CHECK_EQ_INT(bl_map_probe_update(&g_map, at(4) + (10 * FP), at(4), 30U), BL_PROBE_PENDING);
    CHECK_EQ_INT(bl_map_probe_update(&g_map, at(4) + (50 * FP), at(4), 5U), BL_PROBE_PASSED);
    CHECK_EQ_INT(g_map.passed_probes, 1);
    CHECK(!bl_map_probe_failed(&g_map, at(4), at(4), EAST));
    CHECK_EQ_INT(bl_map_probe_update(&g_map, at(9), at(9), 100U), BL_PROBE_PENDING); /* over */
}

static void test_probe_fails_and_is_remembered_by_cell_and_heading(void)
{
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 16, 16), BL_OK);
    bl_map_probe_begin(&g_map, at(4), at(4), NORTH);
    /* Sideways and backwards movement is not progress (unstick backs the player out). */
    CHECK_EQ_INT(bl_map_probe_update(&g_map, at(4) + (90 * FP), at(4), 30U), BL_PROBE_PENDING);
    CHECK_EQ_INT(bl_map_probe_update(&g_map, at(4), at(4) - (90 * FP), 39U), BL_PROBE_PENDING);
    CHECK_EQ_INT(bl_map_probe_update(&g_map, at(4), at(4) + (40 * FP), 1U), BL_PROBE_FAILED);
    CHECK_EQ_INT(g_map.failed_probes, 1);
    CHECK(bl_map_probe_failed(&g_map, at(4) + (CELL / 3), at(4), NORTH + 0x10000000U));
    CHECK(!bl_map_probe_failed(&g_map, at(4), at(4), WEST));  /* other heading */
    CHECK(!bl_map_probe_failed(&g_map, at(5), at(4), NORTH)); /* other cell */
    /* cancel drops a probe without judging it */
    bl_map_probe_begin(&g_map, at(6), at(6), EAST);
    bl_map_probe_cancel(&g_map);
    CHECK_EQ_INT(bl_map_probe_update(&g_map, at(6), at(6), 100U), BL_PROBE_PENDING);
    CHECK(!bl_map_probe_failed(&g_map, at(6), at(6), EAST));
    /* a new map forgets */
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 16, 16), BL_OK);
    CHECK(!bl_map_probe_failed(&g_map, at(4), at(4), NORTH));
}

static void test_explore_prefers_the_less_visited_side(void)
{
    bool left = false;
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 16, 16), BL_OK);
    CHECK(!bl_map_explore(&g_map, at(8), at(8), EAST, &left)); /* nothing visited */
    CHECK(!bl_map_explore(&g_map, at(8), at(8), EAST, NULL));
    /* Facing east, left is north (+y). Visit the cells up and to the right of the player. */
    for (int32_t k = 1; k <= 3; k++) {
        bl_map_visit(&g_map, at(8) + (k * CELL / 2), at(8) + (k * CELL * 87 / 100), 100U);
    }
    CHECK(bl_map_explore(&g_map, at(8), at(8), EAST, &left));
    CHECK(!left); /* the north side was visited: go right */
    /* Facing west the rays (120 and 240 degrees) miss that area: no clear answer. */
    CHECK(!bl_map_explore(&g_map, at(8), at(8), WEST, &left));
    /* Mirror image: visited down and to the right (south-east) while facing east: go left. */
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 16, 16), BL_OK);
    for (int32_t k = 1; k <= 3; k++) {
        bl_map_visit(&g_map, at(8) + (k * CELL / 2), at(8) - (k * CELL * 87 / 100), 100U);
    }
    CHECK(bl_map_explore(&g_map, at(8), at(8), EAST, &left));
    CHECK(left);
    /* A small difference is no clear answer. */
    CHECK_EQ_INT(bl_map_reset(&g_map, ORG, ORG, 16, 16), BL_OK);
    bl_map_visit(&g_map, at(8), at(9), BL_MAP_EXPLORE_MIN_DIFF - 1U);
    CHECK(!bl_map_explore(&g_map, at(8), at(8), EAST, &left));
    /* Both sides visited a lot, nearly equally: no clear answer either. */
    bl_map_visit(&g_map, at(8), at(9), 1000U);
    bl_map_visit(&g_map, at(8), at(7), 1000U);
    CHECK(!bl_map_explore(&g_map, at(8), at(8), EAST, &left));
    /* Near the grid edge the rays leave the grid without harm. */
    CHECK(!bl_map_explore(&g_map, at(0), at(0), WEST, &left));
}

int main(void)
{
    RUN(test_reset_and_grid);
    RUN(test_probe_passes_when_the_player_gets_through);
    RUN(test_probe_fails_and_is_remembered_by_cell_and_heading);
    RUN(test_explore_prefers_the_less_visited_side);
    return TEST_REPORT();
}
