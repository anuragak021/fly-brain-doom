/**
 * @file i_brain_input.c
 * @brief Brain replies to Doom key events, and the kill counter. See i_brain_input.h.
 */
#include "i_brain_input.h"

#include <stdbool.h>
#include <stddef.h>

#include "bl_client.h"
#include "bl_map.h"
#include "bl_policy.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Engine headers. */
#include "d_event.h"
#include "d_main.h"
#include "doomstat.h"
#include "g_game.h"
#include "p_inter.h"
#include "p_mobj.h"
#include "p_setup.h"
#include "r_main.h"

static const char *TAG = "play";

#define USE_PULSE_MS (250LL) /* use is held for this long, then released, then held again */
#define US_PER_MS (1000LL)
#define DIAG_PERIOD_US (2000000LL) /* one diagnostic line every 2 s */
#define FP_SHIFT (16)              /* 16.16 map coordinates to whole map units */
#define CLEAR_EXIT_TICS (105U)     /* 3 s of game time after the last monster dies */
#define REWARD_PER_KILL (1.0F)     /* a monster killed by the player */
#define REWARD_PER_HEALTH (0.01F)  /* subtracted per health point lost */

/** The keys we drive. */
typedef enum { K_LEFT = 0, K_RIGHT, K_FIRE, K_FORWARD, K_BACK, K_USE, K_COUNT } key_slot_t;

static bool s_down[K_COUNT];
static bl_stuck_t s_stuck;
static bl_scan_t s_scan;
static bl_probe_t s_probe;
static bool s_wanted_forward;
static int s_prev_leveltime = -1;
static int32_t s_session_kills;
static uint32_t s_deaths;
static bool s_was_dead;
static uint32_t s_cleared_tics;
static bool s_exit_sent;
static int32_t s_rewarded_kills;
static int s_prev_health = -1; /* -1: not in a level, nothing to compare with */
static EXT_RAM_BSS_ATTR bl_map_t s_map; /* 48 KB: PSRAM */
static int s_map_episode = -1;          /* the map s_map describes */
static int s_map_number = -1;

int32_t ib_session_kills(void)
{
    return s_session_kills;
}

static int key_code(key_slot_t slot)
{
    switch (slot) {
    case K_LEFT:
        return key_left;
    case K_RIGHT:
        return key_right;
    case K_FIRE:
        return key_fire;
    case K_FORWARD:
        return key_up;
    case K_BACK:
        return key_down;
    case K_USE:
    default:
        return key_use;
    }
}

/* The engine drops events while gametic < 3 and clears its key table on every level load, so a
 * key sent once can be lost. Each key is therefore re-posted once per tic (refresh) as well as on
 * every change; repeated key-down events for movement keys are harmless. */
static void set_key(key_slot_t slot, bool want, bool refresh)
{
    if ((s_down[slot] == want) && !refresh) {
        return;
    }
    event_t ev = {want ? ev_keydown : ev_keyup, key_code(slot), 0, 0};
    D_PostEvent(&ev);
    s_down[slot] = want;
}

static void release_all(void)
{
    for (int i = 0; i < (int)K_COUNT; i++) {
        set_key((key_slot_t)i, false, false);
    }
}

/* Only kills by the player itself (p_player_kills, an engine hook): in single player the engine's
 * killcount also counts monsters killed by other monsters or barrels. */
static void count_kills(void)
{
    if (p_player_kills > s_session_kills) {
        s_session_kills = p_player_kills;
        ESP_LOGI(TAG, "kill: %d by the player, %d by anything this level, %u deaths",
                 (int)s_session_kills, players[consoleplayer].killcount, (unsigned)s_deaths);
    }
}

static void apply(const bl_controls_t *c, bool refresh)
{
    set_key(K_LEFT, c->turn_left, refresh);
    set_key(K_RIGHT, c->turn_right, refresh);
    set_key(K_FIRE, c->fire, refresh);
    set_key(K_FORWARD, c->forward, refresh);
    set_key(K_BACK, c->back, refresh);
    set_key(K_USE, c->use, refresh);
}

/* The damage direction features (brain_protocol.h). player->attacker is a plain pointer in
 * PrBoom (not reference counted); it is only read while damagecount > 0, i.e. within 100 tics of
 * the hit, the same way the engine's own death view (p_user.c) reads it. Self-damage (own rocket)
 * and damage without a source (slime, crushers) give no direction. */
static void damage_features(const player_t *p, float *left, float *right)
{
    const mobj_t *mo = p->mo;
    const mobj_t *att = p->attacker;
    if ((mo == NULL) || (att == NULL) || (att == mo) || (p->damagecount <= 0)) {
        (void)bl_damage_make(0U, 0, left, right);
        return;
    }
    const angle_t to = R_PointToAngle2(mo->x, mo->y, att->x, att->y);
    (void)bl_damage_make((uint32_t)(to - mo->angle), (int32_t)p->damagecount, left, right);
}

/* Reward since the last poll: +1 per monster the player killed, minus health lost / 100. Health
 * gained (pickups) is not rewarded. */
static float take_reward(const player_t *p, bool in_level)
{
    float reward = 0.0F;
    if (p_player_kills > s_rewarded_kills) {
        reward += REWARD_PER_KILL * (float)(p_player_kills - s_rewarded_kills);
    }
    s_rewarded_kills = p_player_kills;
    if (!in_level) {
        s_prev_health = -1;
        return reward;
    }
    if ((s_prev_health >= 0) && (p->health < s_prev_health)) {
        reward -= REWARD_PER_HEALTH * (float)(s_prev_health - p->health);
    }
    s_prev_health = p->health;
    return reward;
}

/* Diagnostics, per DIAG_PERIOD_US window and for the whole session: game tics, tics with a
 * monster on screen, and turning tics by source (connectome vs heuristics). */
typedef struct {
    uint32_t tics;
    uint32_t enemy;
    uint32_t fire;
    uint32_t probe;
    uint32_t hurt;          /* tics with damage features >= BL_PAIN_MIN */
    uint32_t probe_skipped; /* tics a probe was held back because one failed here before */
    uint32_t turn[BL_TURN_SOURCES]; /* indexed by bl_turn_source_t; [0] is unused */
} diag_counts_t;

static diag_counts_t s_win;
static diag_counts_t s_total;

static void diag_add(diag_counts_t *d, const bl_snapshot_t *snap, const bl_controls_t *c,
                     uint32_t tics)
{
    d->tics += tics;
    d->enemy += snap->view.enemy_visible ? tics : 0U;
    d->fire += c->fire ? tics : 0U;
    d->probe += (s_probe.probe_left > 0U) ? tics : 0U;
    d->hurt += ((snap->view.feature[BL_FEAT_DAMAGE_LEFT] +
                 snap->view.feature[BL_FEAT_DAMAGE_RIGHT]) >= BL_PAIN_MIN)
                   ? tics
                   : 0U;
    d->turn[c->source] += (c->source != BL_TURN_NONE) ? tics : 0U;
}

static void diag(const bl_snapshot_t *snap, const bl_controls_t *c, uint32_t tics)
{
    static int64_t s_last_us;
    diag_add(&s_win, snap, c, tics);
    diag_add(&s_total, snap, c, tics);
    const int64_t now = esp_timer_get_time();
    if ((now - s_last_us) < DIAG_PERIOD_US) {
        return;
    }
    s_last_us = now;
    const mobj_t *mo = players[consoleplayer].mo;
    ESP_LOGI(TAG,
             "diag: tics %u enemy %u turn brain %u unstick %u wall %u | act 0x%02x steer L %.4f "
             "R %.4f | feat %.3f %.3f %.3f %.3f wallC %.2f | pos %d,%d",
             (unsigned)s_win.tics, (unsigned)s_win.enemy, (unsigned)s_win.turn[BL_TURN_BRAIN],
             (unsigned)s_win.turn[BL_TURN_UNSTICK], (unsigned)s_win.turn[BL_TURN_WALL],
             (unsigned)snap->actions, (double)snap->steer_left, (double)snap->steer_right,
             (double)snap->view.feature[0], (double)snap->view.feature[1],
             (double)snap->view.feature[2], (double)snap->view.feature[3],
             (double)snap->view.wall_centre, (mo != NULL) ? (int)(mo->x >> FP_SHIFT) : 0,
             (mo != NULL) ? (int)(mo->y >> FP_SHIFT) : 0);
    ESP_LOGI(TAG,
             "total: tics %u enemy %u fire %u probe %u (skipped %u, passed %u, failed %u) hurt %u "
             "turn brain %u pain %u unstick %u scan %u wall %u explore %u | health %d kills %d "
             "deaths %u | live %d of %d | drift %.4f",
             (unsigned)s_total.tics, (unsigned)s_total.enemy, (unsigned)s_total.fire,
             (unsigned)s_total.probe, (unsigned)s_total.probe_skipped,
             (unsigned)s_map.passed_probes, (unsigned)s_map.failed_probes,
             (unsigned)s_total.hurt, (unsigned)s_total.turn[BL_TURN_BRAIN],
             (unsigned)s_total.turn[BL_TURN_PAIN], (unsigned)s_total.turn[BL_TURN_UNSTICK],
             (unsigned)s_total.turn[BL_TURN_SCAN], (unsigned)s_total.turn[BL_TURN_WALL],
             (unsigned)s_total.turn[BL_TURN_EXPLORE], players[consoleplayer].health,
             (int)s_session_kills, (unsigned)s_deaths, totallive, totalkills,
             (double)snap->weight_drift);
    s_win = (diag_counts_t){0};
}

/* Heuristic, not the connectome, and a shortcut, not play: once every monster on the level is
 * dead (the engine's totallive, which also counts deaths not caused by the player), end the level
 * as the exit switch would (G_ExitLevel). The player cannot navigate to the exit by itself. The
 * intermission screens are then dismissed with use and the next map starts. */
static void exit_when_cleared(uint32_t tics)
{
    if ((totalkills <= 0) || (totallive > 0) || s_exit_sent) {
        s_cleared_tics = 0U;
        return;
    }
    s_cleared_tics += tics;
    if (s_cleared_tics >= CLEAR_EXIT_TICS) {
        s_exit_sent = true;
        ESP_LOGI(TAG, "level cleared (%d monsters): exiting to the next map", totalkills);
        G_ExitLevel();
    }
}

/* Map memory (heuristic): a new grid only when a different map is loaded, so what was learned
 * about a map's walls survives the player's deaths and respawns on it. */
static void map_memory_for_this_level(void)
{
    if ((gameepisode == s_map_episode) && (gamemap == s_map_number)) {
        bl_map_probe_cancel(&s_map);
        return;
    }
    s_map_episode = gameepisode;
    s_map_number = gamemap;
    const bl_status_t st = bl_map_reset(&s_map, bmaporgx, bmaporgy, bmapwidth, bmapheight);
    ESP_LOGI(TAG, "map memory: E%dM%d, %dx%d cells%s", gameepisode, gamemap, bmapwidth,
             bmapheight, (st == BL_OK) ? "" : " (empty: bad blockmap size)");
}

/* Door probe with map memory: a probe is not started where one failed before (same cell, same
 * heading octant); a running probe is judged by the player's progress along its heading. */
static void probe_with_memory(const mobj_t *mo, const bl_features_t *view, uint32_t tics)
{
    const bool remembered = bl_map_probe_failed(&s_map, mo->x, mo->y, mo->angle);
    const bool was_probing = (s_probe.probe_left > 0U);
    if (view->wall_ahead && remembered && !was_probing && (s_probe.cooldown == 0U)) {
        s_win.probe_skipped += tics;
        s_total.probe_skipped += tics;
    }
    (void)bl_probe_update(&s_probe, view->wall_ahead && !remembered, view->enemy_visible, tics);
    if (view->enemy_visible) {
        bl_map_probe_cancel(&s_map);
    } else if (!was_probing && (s_probe.probe_left > 0U)) {
        bl_map_probe_begin(&s_map, mo->x, mo->y, mo->angle);
    } else {
        const bl_probe_result_t r = bl_map_probe_update(&s_map, mo->x, mo->y, tics);
        if (r == BL_PROBE_FAILED) {
            ESP_LOGI(TAG, "probe failed at %d,%d: remembered (%u so far)",
                     (int)(mo->x >> FP_SHIFT), (int)(mo->y >> FP_SHIFT),
                     (unsigned)s_map.failed_probes);
        } else if (r == BL_PROBE_PASSED) {
            ESP_LOGI(TAG, "probe got through at %d,%d", (int)(mo->x >> FP_SHIFT),
                     (int)(mo->y >> FP_SHIFT));
        } else {
            /* still judging, or no probe */
        }
    }
}

/** use pulses (down, up, down ...) so a held key does not look like one long press. */
static bool use_pulse(void)
{
    return ((esp_timer_get_time() / (USE_PULSE_MS * US_PER_MS)) & 1LL) != 0LL;
}

void ib_poll(void)
{
    count_kills();
    bl_snapshot_t snap;
    bl_client_snapshot(&snap);
    const player_t *p = &players[consoleplayer];
    const bool alive_in_level = (gamestate == GS_LEVEL) && (p->playerstate == PST_LIVE);
    const float reward = take_reward(p, alive_in_level);
    float dmg_left = 0.0F;
    float dmg_right = 0.0F;
    if (alive_in_level) {
        damage_features(p, &dmg_left, &dmg_right);
    }
    const bool learn_ok = alive_in_level && snap.brain_ok && !menuactive && !paused;
    bl_client_game(dmg_left, dmg_right, reward, learn_ok);
    snap.view.feature[BL_FEAT_DAMAGE_LEFT] = dmg_left; /* the policy's pain rule sees it now */
    snap.view.feature[BL_FEAT_DAMAGE_RIGHT] = dmg_right;
    if (!snap.brain_ok || menuactive || paused) {
        release_all(); /* no brain, no action */
        s_wanted_forward = false;
        return;
    }
    bl_controls_t c = {false, false, false, false, false, false, BL_TURN_NONE};
    const bool dead = (gamestate == GS_LEVEL) && (p->playerstate == PST_DEAD);
    if (dead && !s_was_dead) {
        s_deaths++;
        ESP_LOGI(TAG, "died (%u deaths, %d kills in total)", (unsigned)s_deaths,
                 (int)s_session_kills);
    }
    s_was_dead = dead;
    if ((gamestate != GS_LEVEL) || dead) {
        /* Heuristic: respawn after death, dismiss the intermission screens. */
        c.use = use_pulse();
        s_prev_leveltime = -1;
        bl_map_probe_cancel(&s_map);
        bl_stuck_reset(&s_stuck);
        bl_scan_reset(&s_scan);
        bl_probe_reset(&s_probe);
        apply(&c, false);
        s_wanted_forward = false;
        return;
    }
    if (leveltime < s_prev_leveltime) {
        bl_stuck_reset(&s_stuck); /* new level or respawn */
        bl_scan_reset(&s_scan);
        bl_probe_reset(&s_probe);
    }
    if (s_prev_leveltime < 0) {
        s_exit_sent = false; /* back in a level after death or intermission */
        s_cleared_tics = 0U;
        map_memory_for_this_level();
    }
    /* The engine can run several tics between two polls; timings are in tics, not polls. */
    const bool new_tic = (leveltime != s_prev_leveltime);
    const uint32_t tics =
        (new_tic && (s_prev_leveltime >= 0)) ? (uint32_t)(leveltime - s_prev_leveltime) : 1U;
    if (new_tic && (p->mo != NULL)) {
        (void)bl_stuck_update(&s_stuck, p->mo->x, p->mo->y, s_wanted_forward, tics);
    }
    if (new_tic) {
        (void)bl_scan_update(&s_scan, snap.view.enemy_visible, tics);
        if (p->mo != NULL) {
            bl_map_visit(&s_map, p->mo->x, p->mo->y, tics);
            probe_with_memory(p->mo, &snap.view, tics);
        }
        exit_when_cleared(tics);
    }
    s_prev_leveltime = leveltime;
    bl_hint_t hint = {false, false};
    if (p->mo != NULL) {
        hint.explore_known =
            bl_map_explore(&s_map, p->mo->x, p->mo->y, p->mo->angle, &hint.explore_left);
    }
    (void)bl_policy_decide(snap.actions, &snap.view, &hint, &s_stuck, &s_scan, &s_probe, &c);
    if (c.use) {
        c.use = use_pulse();
    }
    s_wanted_forward = c.forward;
    apply(&c, new_tic);
    if (new_tic) {
        diag(&snap, &c, tics);
    }
}
