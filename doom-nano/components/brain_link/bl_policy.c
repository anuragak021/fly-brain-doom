/**
 * @file bl_policy.c
 * @brief Controls from brain actions plus heuristics. See bl_policy.h.
 */
#include "bl_policy.h"

#include <math.h>
#include <stddef.h>

#include "brain_protocol.h"

void bl_stuck_reset(bl_stuck_t *st)
{
    if (st == NULL) {
        return;
    }
    st->last_x = 0;
    st->last_y = 0;
    st->have_last = false;
    st->still_tics = 0U;
    st->unstick_left = 0U;
    st->since_end = BL_UNSTICK_REPEAT_TICS;
    st->turn_left = false;
}

static int64_t abs_diff(int32_t a, int32_t b)
{
    const int64_t d = (int64_t)a - (int64_t)b;
    return (d < 0) ? -d : d;
}

bool bl_stuck_update(bl_stuck_t *st, int32_t x, int32_t y, bool wanted_forward, uint32_t tics)
{
    if (st == NULL) {
        return false;
    }
    const uint32_t n = (tics == 0U) ? 1U : tics;
    if (st->unstick_left > 0U) {
        st->unstick_left = (st->unstick_left > n) ? (st->unstick_left - n) : 0U;
    } else if (st->since_end < BL_UNSTICK_REPEAT_TICS) {
        st->since_end += n;
    } else {
        /* saturated */
    }
    if (st->have_last) {
        const int64_t moved = abs_diff(x, st->last_x) + abs_diff(y, st->last_y);
        if (wanted_forward && (moved < ((int64_t)BL_STUCK_MIN_MOVE_FP * (int64_t)n)) &&
            (st->unstick_left == 0U)) {
            st->still_tics += n;
        } else {
            st->still_tics = 0U;
        }
    }
    st->last_x = x;
    st->last_y = y;
    st->have_last = true;
    if (st->still_tics >= BL_STUCK_TICS) {
        st->still_tics = 0U;
        st->unstick_left = BL_UNSTICK_TICS;
        if (st->since_end >= BL_UNSTICK_REPEAT_TICS) {
            st->turn_left = !st->turn_left; /* a new obstacle: alternate */
        } /* else stuck again right away: keep turning the same way, or two turns cancel out */
        st->since_end = 0U;
    }
    return st->unstick_left > 0U;
}

void bl_scan_reset(bl_scan_t *sc)
{
    if (sc == NULL) {
        return;
    }
    sc->unseen_tics = 0U;
    sc->scan_left = 0U;
    sc->turn_left = false;
}

bool bl_scan_update(bl_scan_t *sc, bool enemy_visible, uint32_t tics)
{
    if (sc == NULL) {
        return false;
    }
    const uint32_t n = (tics == 0U) ? 1U : tics;
    if (enemy_visible) {
        sc->unseen_tics = 0U;
        sc->scan_left = 0U;
    } else if (sc->scan_left > 0U) {
        sc->scan_left = (sc->scan_left > n) ? (sc->scan_left - n) : 0U;
    } else {
        sc->unseen_tics += n;
        if (sc->unseen_tics >= BL_SCAN_AFTER_TICS) {
            sc->unseen_tics = 0U;
            sc->scan_left = BL_SCAN_TICS;
            sc->turn_left = !sc->turn_left;
        }
    }
    return sc->scan_left > 0U;
}

void bl_probe_reset(bl_probe_t *pr)
{
    if (pr == NULL) {
        return;
    }
    pr->probe_left = 0U;
    pr->cooldown = 0U;
}

static uint32_t count_down(uint32_t v, uint32_t n)
{
    return (v > n) ? (v - n) : 0U;
}

bool bl_probe_update(bl_probe_t *pr, bool wall_ahead, bool enemy_visible, uint32_t tics)
{
    if (pr == NULL) {
        return false;
    }
    const uint32_t n = (tics == 0U) ? 1U : tics;
    pr->cooldown = count_down(pr->cooldown, n);
    if (enemy_visible) {
        pr->probe_left = 0U;
    } else if (pr->probe_left > 0U) {
        pr->probe_left = count_down(pr->probe_left, n);
    } else if (wall_ahead && (pr->cooldown == 0U)) {
        pr->probe_left = BL_PROBE_TICS;
        pr->cooldown = BL_PROBE_COOLDOWN_TICS;
    } else {
        /* nothing to do */
    }
    return pr->probe_left > 0U;
}

static void turn(bl_controls_t *out, bool left, bl_turn_source_t source)
{
    out->turn_left = left;
    out->turn_right = !left;
    out->source = source;
}

bl_status_t bl_policy_decide(uint8_t actions, const bl_features_t *view, const bl_hint_t *hint,
                             const bl_stuck_t *stuck, const bl_scan_t *scan,
                             const bl_probe_t *probe, bl_controls_t *out)
{
    if ((view == NULL) || (hint == NULL) || (stuck == NULL) || (scan == NULL) || (probe == NULL) ||
        (out == NULL)) {
        return BL_ERR_NULL;
    }
    const bool left = (actions & BRAIN_ACTION_TURN_LEFT) != 0U;
    const bool right = (actions & BRAIN_ACTION_TURN_RIGHT) != 0U;
    const bool brain_turns = (left != right);
    out->turn_left = false;
    out->turn_right = false;
    out->source = BL_TURN_NONE;
    out->fire = view->enemy_visible && (fabsf(view->enemy_offset) < BL_FIRE_CONE);
    out->forward = !out->fire;
    out->back = false;
    out->use = false;

    const bool hurt = (view->feature[BL_FEAT_DAMAGE_LEFT] + view->feature[BL_FEAT_DAMAGE_RIGHT]) >=
                      BL_PAIN_MIN;
    if (view->enemy_visible) {
        if (brain_turns) {
            turn(out, left, BL_TURN_BRAIN);
        }
    } else if (hurt && brain_turns) {
        turn(out, left, BL_TURN_PAIN);
    } else if (stuck->unstick_left > 0U) {
        turn(out, stuck->turn_left, BL_TURN_UNSTICK);
        out->forward = false;
        out->back = true;
        out->use = true;
    } else if (scan->scan_left > 0U) {
        turn(out, scan->turn_left, BL_TURN_SCAN);
        out->forward = false;
    } else if (probe->probe_left > 0U) {
        out->use = true; /* straight ahead, forward stays on */
    } else if (view->wall_ahead && hint->explore_known) {
        turn(out, hint->explore_left, BL_TURN_EXPLORE);
    } else if (view->wall_ahead) {
        turn(out, view->open_left, BL_TURN_WALL);
    } else if (brain_turns) {
        turn(out, left, BL_TURN_BRAIN);
    }
    return BL_OK;
}
