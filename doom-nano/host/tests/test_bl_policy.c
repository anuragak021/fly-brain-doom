/** @file test_bl_policy.c @brief Turn priorities, fire/forward heuristics, stuck and scan. */
#include <stdbool.h>
#include <stdint.h>

#include "bl_policy.h"
#include "brain_protocol.h"
#include "test_runner.h"

#define WALK_FP (8 * 65536) /* well above BL_STUCK_MIN_MOVE_FP per tic */

static bl_features_t view(bool enemy, float off, bool wall_ahead, bool open_left)
{
    bl_features_t v = {0};
    v.enemy_visible = enemy;
    v.enemy_offset = off;
    v.wall_ahead = wall_ahead;
    v.open_left = open_left;
    return v;
}

static bl_stuck_t unsticking(bool left)
{
    bl_stuck_t st;
    bl_stuck_reset(&st);
    st.unstick_left = BL_UNSTICK_TICS;
    st.turn_left = left;
    return st;
}

static bl_scan_t no_scan(void)
{
    bl_scan_t sc;
    bl_scan_reset(&sc);
    return sc;
}

static bl_controls_t decide_hint(uint8_t actions, bl_features_t v, bl_hint_t h, bl_stuck_t st,
                                 bl_scan_t sc)
{
    bl_controls_t c;
    const bl_probe_t pr = {0U, 0U};
    CHECK_EQ_INT(bl_policy_decide(actions, &v, &h, &st, &sc, &pr, &c), BL_OK);
    return c;
}

static bl_controls_t decide_scan(uint8_t actions, bl_features_t v, bl_stuck_t st, bl_scan_t sc)
{
    const bl_hint_t none = {false, false};
    return decide_hint(actions, v, none, st, sc);
}

static bl_controls_t decide(uint8_t actions, bl_features_t v, bl_stuck_t st)
{
    return decide_scan(actions, v, st, no_scan());
}

static bl_scan_t scanning(bool left)
{
    bl_scan_t sc = no_scan();
    sc.scan_left = BL_SCAN_TICS;
    sc.turn_left = left;
    return sc;
}

static bl_stuck_t idle(void)
{
    bl_stuck_t st;
    bl_stuck_reset(&st);
    return st;
}

static void test_brain_turns(void)
{
    bl_controls_t c = decide(BRAIN_ACTION_TURN_LEFT, view(false, 0.0F, false, false), idle());
    CHECK(c.turn_left && !c.turn_right && (c.source == BL_TURN_BRAIN));
    c = decide(BRAIN_ACTION_TURN_RIGHT, view(true, -0.9F, false, false), idle());
    CHECK(c.turn_right && !c.turn_left && (c.source == BL_TURN_BRAIN)); /* enemy side ignored */
    c = decide(0U, view(true, 0.9F, false, false), idle());
    CHECK(!c.turn_left && !c.turn_right && (c.source == BL_TURN_NONE));
    c = decide(BRAIN_ACTION_TURN_LEFT | BRAIN_ACTION_TURN_RIGHT, view(false, 0.0F, false, false),
               idle());
    CHECK(!c.turn_left && !c.turn_right); /* contradictory bits: no turn */
}

static void test_brain_wins_while_a_monster_is_visible(void)
{
    /* Neither unstick nor a wall overrides the brain with a monster on screen. */
    bl_controls_t c =
        decide(BRAIN_ACTION_TURN_LEFT, view(true, -0.5F, true, false), unsticking(false));
    CHECK(c.turn_left && !c.turn_right && (c.source == BL_TURN_BRAIN) && !c.use && !c.back);
    /* Not even when the brain asks for no turn. */
    c = decide_scan(0U, view(true, -0.5F, true, true), unsticking(true), scanning(true));
    CHECK(!c.turn_left && !c.turn_right && (c.source == BL_TURN_NONE));
}

static void test_heuristics_without_a_monster(void)
{
    /* Unstick first, in its own direction: back up, turn, use for doors. */
    bl_controls_t c = decide_scan(BRAIN_ACTION_TURN_RIGHT, view(false, 0.0F, true, false),
                                  unsticking(true), scanning(false));
    CHECK(c.turn_left && !c.turn_right && c.use && c.back && !c.forward &&
          (c.source == BL_TURN_UNSTICK));
    /* Then the scan: turn in place. */
    c = decide_scan(BRAIN_ACTION_TURN_RIGHT, view(false, 0.0F, true, false), idle(),
                    scanning(true));
    CHECK(c.turn_left && !c.turn_right && !c.forward && !c.back && (c.source == BL_TURN_SCAN));
    /* Then walls: turn toward the open side. */
    c = decide(BRAIN_ACTION_TURN_RIGHT, view(false, 0.0F, true, true), idle());
    CHECK(c.turn_left && !c.turn_right && !c.use && (c.source == BL_TURN_WALL));
    c = decide(0U, view(false, 0.0F, true, false), idle());
    CHECK(c.turn_right && !c.turn_left && (c.source == BL_TURN_WALL));
}

static bl_features_t hurt(float left, float right)
{
    bl_features_t v = view(false, 0.0F, true, true); /* wall ahead, open to the left */
    v.feature[BL_FEAT_DAMAGE_LEFT] = left;
    v.feature[BL_FEAT_DAMAGE_RIGHT] = right;
    return v;
}

static void test_pain_gives_the_brain_priority_only_when_it_turns(void)
{
    /* Hurt and the brain turns: its turn beats unstick, scan and walls. */
    bl_controls_t c =
        decide_scan(BRAIN_ACTION_TURN_RIGHT, hurt(0.0F, 0.4F), unsticking(true), scanning(true));
    CHECK(c.turn_right && !c.turn_left && c.forward && !c.back && (c.source == BL_TURN_PAIN));
    /* Hurt but the brain does not turn: the heuristics run as usual. */
    c = decide_scan(0U, hurt(0.0F, 0.4F), unsticking(true), no_scan());
    CHECK(c.turn_left && (c.source == BL_TURN_UNSTICK));
    c = decide(0U, hurt(0.4F, 0.0F), idle());
    CHECK(c.turn_left && (c.source == BL_TURN_WALL));
    /* Below the threshold it is not pain: the wall rule wins over a brain turn. */
    c = decide(BRAIN_ACTION_TURN_RIGHT, hurt(0.04F, 0.04F), idle());
    CHECK(c.turn_left && (c.source == BL_TURN_WALL));
    /* A visible monster is rule 1, not pain. */
    bl_features_t v = hurt(0.5F, 0.0F);
    v.enemy_visible = true;
    v.enemy_offset = -0.5F;
    c = decide(BRAIN_ACTION_TURN_LEFT, v, idle());
    CHECK(c.turn_left && (c.source == BL_TURN_BRAIN));
}

static void test_explore_hint_steers_wall_turns(void)
{
    const bl_hint_t go_left = {true, true};
    const bl_hint_t unsure = {false, true};
    /* Wall ahead, open side on screen is the right; map memory says the left is less visited. */
    bl_controls_t c =
        decide_hint(0U, view(false, 0.0F, true, false), go_left, idle(), no_scan());
    CHECK(c.turn_left && !c.turn_right && (c.source == BL_TURN_EXPLORE));
    /* No clear answer from the map: the screen decides, as before. */
    c = decide_hint(0U, view(false, 0.0F, true, false), unsure, idle(), no_scan());
    CHECK(c.turn_right && (c.source == BL_TURN_WALL));
    /* No wall ahead: the hint does nothing. */
    c = decide_hint(0U, view(false, 0.0F, false, false), go_left, idle(), no_scan());
    CHECK(!c.turn_left && !c.turn_right && (c.source == BL_TURN_NONE));
    /* It never overrides the brain with a monster on screen, pain, unstick or scan. */
    c = decide_hint(BRAIN_ACTION_TURN_RIGHT, view(true, 0.5F, true, false), go_left, idle(),
                    no_scan());
    CHECK(c.turn_right && (c.source == BL_TURN_BRAIN));
    c = decide_hint(0U, view(false, 0.0F, true, false), go_left, unsticking(false), no_scan());
    CHECK(c.turn_right && (c.source == BL_TURN_UNSTICK));
    c = decide_hint(0U, view(false, 0.0F, true, false), go_left, idle(), scanning(false));
    CHECK(c.turn_right && (c.source == BL_TURN_SCAN));
}

static void test_fire_and_forward_heuristics(void)
{
    bl_controls_t c = decide(0U, view(false, 0.0F, false, false), idle());
    CHECK(!c.fire && c.forward); /* nothing in sight: walk */
    c = decide(0U, view(true, 0.0F, false, false), idle());
    CHECK(c.fire && !c.forward); /* aimed: stand and shoot */
    c = decide(0U, view(true, BL_FIRE_CONE - 0.01F, false, false), idle());
    CHECK(c.fire);
    c = decide(0U, view(true, -(BL_FIRE_CONE - 0.01F), false, false), idle());
    CHECK(c.fire);
    c = decide(0U, view(true, BL_FIRE_CONE, false, false), idle());
    CHECK(!c.fire && c.forward); /* at the cone's edge: not aimed */
    c = decide(0U, view(true, -0.5F, false, false), idle());
    CHECK(!c.fire && c.forward); /* off-centre: keep walking while the brain turns */
}

static void test_stuck_detector_counts_tics(void)
{
    bl_stuck_t st;
    bl_stuck_reset(&st);
    int32_t x = 0;
    for (uint32_t i = 0U; i < (3U * BL_STUCK_TICS); i++) {
        x += WALK_FP;
        CHECK(!bl_stuck_update(&st, x, 0, true, 1U));
    }
    /* Blocked, one tic per call: the manoeuvre starts after BL_STUCK_TICS tics. */
    uint32_t first = 0U;
    for (uint32_t i = 1U; i <= (2U * BL_STUCK_TICS); i++) {
        if (bl_stuck_update(&st, x, 0, true, 1U) && (first == 0U)) {
            first = i;
        }
    }
    CHECK_EQ_INT(first, BL_STUCK_TICS);

    /* Blocked, 3 tics per call (engine catching up): same game time, a third of the calls. */
    bl_stuck_reset(&st);
    (void)bl_stuck_update(&st, 0, 0, true, 3U);
    uint32_t calls = 0U;
    while (!bl_stuck_update(&st, 0, 0, true, 3U) && (calls < 100U)) {
        calls++;
    }
    CHECK_EQ_INT(calls + 1U, (BL_STUCK_TICS + 2U) / 3U);

    /* Moving 3 units in 3 tics is walking, not stuck. */
    bl_stuck_reset(&st);
    x = 0;
    for (uint32_t i = 0U; i < (2U * BL_STUCK_TICS); i++) {
        x += 3 * WALK_FP;
        CHECK(!bl_stuck_update(&st, x, 0, true, 3U));
    }
}

static void test_standing_still_on_purpose_is_not_stuck(void)
{
    bl_stuck_t st;
    bl_stuck_reset(&st);
    for (uint32_t i = 0U; i < (4U * BL_STUCK_TICS); i++) {
        CHECK(!bl_stuck_update(&st, 0, 0, false, 1U)); /* shooting, not walking */
    }
}

static void test_unstick_lasts_then_ends(void)
{
    bl_stuck_t st;
    bl_stuck_reset(&st);
    uint32_t active = 0U;
    for (uint32_t i = 0U; i < (BL_STUCK_TICS + BL_UNSTICK_TICS + 5U); i++) {
        active += bl_stuck_update(&st, 0, 0, true, 1U) ? 1U : 0U;
    }
    CHECK_EQ_INT(active, BL_UNSTICK_TICS);
}

/* Run until a manoeuvre starts; @p walk_tics of free walking first. Returns its direction. */
static bool next_unstick(bl_stuck_t *st, uint32_t walk_tics)
{
    int32_t x = 0;
    for (uint32_t i = 0U; i < walk_tics; i++) {
        x += WALK_FP;
        (void)bl_stuck_update(st, x, 0, true, 1U);
    }
    for (uint32_t i = 0U; (i < (4U * BL_STUCK_TICS)) && !bl_stuck_update(st, x, 0, true, 1U); i++) {
    }
    for (uint32_t i = 0U; i < BL_UNSTICK_TICS; i++) {
        (void)bl_stuck_update(st, x, 0, false, 1U); /* finish the manoeuvre */
    }
    return st->turn_left;
}

static void test_unstick_escalates_then_alternates(void)
{
    bl_stuck_t st;
    bl_stuck_reset(&st);
    const bool first = next_unstick(&st, 0U);
    /* Stuck again within BL_UNSTICK_REPEAT_TICS: same way, or the two turns cancel out. */
    CHECK(next_unstick(&st, 0U) == first);
    /* A new obstacle after walking freely for a while: the other way. */
    CHECK(next_unstick(&st, BL_UNSTICK_REPEAT_TICS + 5U) != first);
}

static void test_scan(void)
{
    bl_scan_t sc;
    bl_scan_reset(&sc);
    uint32_t first = 0U;
    for (uint32_t i = 1U; i <= (BL_SCAN_AFTER_TICS + 10U); i++) {
        if (bl_scan_update(&sc, false, 1U) && (first == 0U)) {
            first = i;
        }
    }
    CHECK_EQ_INT(first, BL_SCAN_AFTER_TICS);
    CHECK(!bl_scan_update(&sc, true, 1U)); /* a monster on screen stops it at once */
    CHECK_EQ_INT(sc.unseen_tics, 0U);
    /* Seeing a monster now and then keeps the scan from starting. */
    bl_scan_reset(&sc);
    for (uint32_t i = 1U; i <= (3U * BL_SCAN_AFTER_TICS); i++) {
        CHECK(!bl_scan_update(&sc, (i % (BL_SCAN_AFTER_TICS / 2U)) == 0U, 1U));
    }
    /* A full scan runs BL_SCAN_TICS, then waits again; 4 tics per call count as 4. */
    bl_scan_reset(&sc);
    uint32_t calls = 0U;
    while (!bl_scan_update(&sc, false, 4U) && (calls < 1000U)) {
        calls++;
    }
    CHECK_EQ_INT(calls + 1U, (BL_SCAN_AFTER_TICS + 3U) / 4U);
    CHECK(!bl_scan_update(NULL, false, 1U));
    bl_scan_reset(NULL);
}

static void test_probe_timing(void)
{
    bl_probe_t pr;
    bl_probe_reset(&pr);
    CHECK(!bl_probe_update(&pr, false, false, 1U)); /* no wall: nothing */
    CHECK(bl_probe_update(&pr, true, false, 1U));   /* wall: starts at once */
    uint32_t active = 1U;
    for (uint32_t i = 0U; i < (2U * BL_PROBE_TICS); i++) {
        active += bl_probe_update(&pr, true, false, 1U) ? 1U : 0U;
    }
    CHECK_EQ_INT(active, BL_PROBE_TICS); /* then the cooldown holds it off */
    for (uint32_t i = 0U; i < (BL_PROBE_COOLDOWN_TICS - (2U * BL_PROBE_TICS) - 1U); i++) {
        CHECK(!bl_probe_update(&pr, true, false, 1U));
    }
    CHECK(bl_probe_update(&pr, true, false, 1U)); /* 140 tics after the first start */
    CHECK(!bl_probe_update(&pr, true, true, 1U)); /* a monster on screen stops it */
}

static void test_probe_walks_straight_and_uses(void)
{
    const bl_features_t v = view(false, 0.0F, true, true);
    const bl_stuck_t st = idle();
    const bl_scan_t sc = no_scan();
    const bl_probe_t pr = {BL_PROBE_TICS, BL_PROBE_COOLDOWN_TICS};
    const bl_hint_t h = {true, false};
    bl_controls_t c;
    CHECK_EQ_INT(bl_policy_decide(BRAIN_ACTION_TURN_LEFT, &v, &h, &st, &sc, &pr, &c), BL_OK);
    CHECK(c.forward && c.use && !c.turn_left && !c.turn_right && (c.source == BL_TURN_NONE));
    /* Unstick still wins over a probe (a plain wall: back out). */
    const bl_stuck_t un = unsticking(true);
    CHECK_EQ_INT(bl_policy_decide(0U, &v, &h, &un, &sc, &pr, &c), BL_OK);
    CHECK(c.back && (c.source == BL_TURN_UNSTICK));
}

static void test_null_arguments(void)
{
    const bl_features_t v = view(false, 0.0F, false, false);
    const bl_stuck_t st = idle();
    bl_controls_t c;
    const bl_scan_t sc = no_scan();
    const bl_probe_t pr = {0U, 0U};
    const bl_hint_t h = {false, false};
    CHECK_EQ_INT(bl_policy_decide(0U, &v, &h, &st, &sc, &pr, NULL), BL_ERR_NULL);
    CHECK_EQ_INT(bl_policy_decide(0U, NULL, &h, &st, &sc, &pr, &c), BL_ERR_NULL);
    CHECK_EQ_INT(bl_policy_decide(0U, &v, NULL, &st, &sc, &pr, &c), BL_ERR_NULL);
    CHECK_EQ_INT(bl_policy_decide(0U, &v, &h, NULL, &sc, &pr, &c), BL_ERR_NULL);
    CHECK_EQ_INT(bl_policy_decide(0U, &v, &h, &st, NULL, &pr, &c), BL_ERR_NULL);
    CHECK_EQ_INT(bl_policy_decide(0U, &v, &h, &st, &sc, NULL, &c), BL_ERR_NULL);
    CHECK(!bl_probe_update(NULL, true, false, 1U));
    bl_probe_reset(NULL);
    CHECK(!bl_stuck_update(NULL, 0, 0, true, 1U));
    bl_stuck_reset(NULL);
}

int main(void)
{
    RUN(test_brain_turns);
    RUN(test_brain_wins_while_a_monster_is_visible);
    RUN(test_heuristics_without_a_monster);
    RUN(test_pain_gives_the_brain_priority_only_when_it_turns);
    RUN(test_explore_hint_steers_wall_turns);
    RUN(test_fire_and_forward_heuristics);
    RUN(test_stuck_detector_counts_tics);
    RUN(test_standing_still_on_purpose_is_not_stuck);
    RUN(test_unstick_lasts_then_ends);
    RUN(test_unstick_escalates_then_alternates);
    RUN(test_scan);
    RUN(test_probe_timing);
    RUN(test_probe_walks_straight_and_uses);
    RUN(test_null_arguments);
    return TEST_REPORT();
}
