/**
 * @file i_sprite_ids.c
 * @brief Sprite id map, id classes and per-mobj state for the nano OLED style.
 */
#include "i_sprite_ids.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "op_nano.h"
#include "op_scale.h"

/* Engine headers. */
#include "doomstat.h"
#include "info.h"
#include "r_main.h"

#define MOBJ_SLOTS (64U) /* sprites tracked across frames; more on screen share the oldest */
#define HIT_TICS (6)     /* hit flash length, 35 tics per second */
#define HIT_FRAMES (2U)  /* ...and at least this many drawn frames, so slow frames show it */
#define NO_SLOT (0U)     /* s_id_slot value: id has no mobj */

/** What the platform remembers about one mobj between frames. */
typedef struct {
    const mobj_t *mo;   /* compared only, never dereferenced */
    int32_t seen_tic;   /* leveltime it was last drawn or hit */
    int32_t hit_until;  /* hit flash while leveltime < this */
    uint8_t hit_frames; /* or while this many drawn frames are left */
    uint8_t mode;       /* op_spr_mode_t op_nano chose last */
} slot_t;

_Static_assert(MOBJ_SLOTS < UINT8_MAX, "slot index + 1 fits a byte");

static slot_t s_slots[MOBJ_SLOTS];
static uint8_t s_cls[OP_NANO_IDS];
static uint8_t s_prev[OP_NANO_IDS];
static uint8_t s_id_slot[OP_NANO_IDS]; /* slot index + 1, or NO_SLOT */
static uint8_t *s_map;                 /* NULL while not recording */
static uint32_t s_win_y0;
static uint32_t s_rows;
static uint32_t s_last_id; /* last world id handed out this frame */
static uint8_t s_cur;      /* id I_SpritePost writes; 0 = none */
static bool s_mid_next;    /* the next post starts a new mid texture id */
static int32_t s_last_tic;

/** Slot of @p mo, or a new one (empty, else least recently seen) if it has none. */
static uint32_t slot_of(const mobj_t *mo)
{
    uint32_t victim = 0U;
    for (uint32_t i = 0U; i < MOBJ_SLOTS; i++) {
        if (s_slots[i].mo == mo) {
            return i;
        }
        if ((s_slots[victim].mo != NULL) &&
            ((s_slots[i].mo == NULL) || (s_slots[i].seen_tic < s_slots[victim].seen_tic))) {
            victim = i;
        }
    }
    s_slots[victim] = (slot_t){mo, leveltime, 0, 0U, (uint8_t)OP_SPR_MODE_UNKNOWN};
    return victim;
}

/** Next world id (ids past OP_NANO_ID_LAST_THING share the last one) with its class. */
static uint8_t new_id(op_spr_class_t cls, uint32_t slot1)
{
    if (s_last_id < OP_NANO_ID_LAST_THING) {
        s_last_id++;
    }
    s_cls[s_last_id] = (uint8_t)cls;
    s_id_slot[s_last_id] = (uint8_t)slot1;
    s_prev[s_last_id] =
        (slot1 != NO_SLOT) ? s_slots[slot1 - 1U].mode : (uint8_t)OP_SPR_MODE_UNKNOWN;
    return (uint8_t)s_last_id;
}

static bool is_monster(const mobj_t *mo)
{
    return ((mo->flags & MF_COUNTKILL) != 0U) || (mo->type == MT_SKULL);
}

void sid_begin(uint8_t *map, uint32_t win_y0, uint32_t rows)
{
    if (map == NULL) {
        return;
    }
    if (leveltime < s_last_tic) {
        (void)memset(s_slots, 0, sizeof s_slots); /* new level or loaded game */
    }
    s_last_tic = leveltime;
    (void)memset(map, 0, (size_t)OP_DOOM_SRC_W * rows);
    (void)memset(s_cls, 0, sizeof s_cls);
    (void)memset(s_prev, 0, sizeof s_prev);
    (void)memset(s_id_slot, 0, sizeof s_id_slot);
    s_cls[OP_NANO_ID_WEAPON] = (uint8_t)OP_SPR_WEAPON;
    s_map = map;
    s_win_y0 = win_y0;
    s_rows = rows;
    s_last_id = 0U;
    s_cur = 0U;
    s_mid_next = true; /* masked segs behind the first sprite come before it */
}

void sid_end(void)
{
    s_map = NULL;
}

const uint8_t *sid_classes(void)
{
    return s_cls;
}

const uint8_t *sid_prev_modes(void)
{
    return s_prev;
}

void sid_store_modes(const uint8_t *mode)
{
    if (mode == NULL) {
        return;
    }
    for (uint32_t k = 1U; k <= OP_NANO_ID_LAST_THING; k++) {
        if (s_id_slot[k] == NO_SLOT) {
            continue;
        }
        slot_t *slot = &s_slots[s_id_slot[k] - 1U];
        if (mode[k] != (uint8_t)OP_SPR_MODE_UNKNOWN) {
            slot->mode = mode[k];
        }
        if (slot->hit_frames > 0U) {
            slot->hit_frames--;
        }
    }
}

void I_SpriteThing(const mobj_t *thing, int is_weapon, int is_fuzz)
{
    if (s_map == NULL) {
        return;
    }
    s_mid_next = false;
    if (is_weapon != 0) {
        s_cur = (uint8_t)OP_NANO_ID_WEAPON;
        return;
    }
    if ((thing == NULL) || (is_fuzz != 0)) {
        s_cur = 0U;
        return;
    }
    op_spr_facts_t facts = {
        .fx = ((thing->flags & MF_MISSILE) != 0U) || (thing->type == MT_PUFF) ||
              (thing->type == MT_BLOOD),
        .corpse = (thing->flags & MF_CORPSE) != 0U,
        .still = thing->tics == -1,
        .monster = is_monster(thing),
        .item = (thing->flags & MF_SPECIAL) != 0U,
    };
    uint32_t slot1 = NO_SLOT;
    if (!facts.fx) {
        const uint32_t i = slot_of(thing);
        s_slots[i].seen_tic = leveltime;
        facts.hit = (leveltime < s_slots[i].hit_until) || (s_slots[i].hit_frames > 0U);
        slot1 = i + 1U;
    }
    s_cur = new_id(op_nano_classify(&facts), slot1);
}

void I_SpritePost(int x, int yl, int yh)
{
    if (s_map == NULL) {
        return;
    }
    if (s_mid_next) {
        s_cur = new_id(OP_SPR_MID, NO_SLOT);
        s_mid_next = false;
    }
    const int32_t fx = (int32_t)x + viewwindowx;
    if ((s_cur == 0U) || (fx < 0) || (fx >= (int32_t)OP_DOOM_SRC_W)) {
        return;
    }
    const int32_t win_y1 = (int32_t)(s_win_y0 + s_rows) - 1;
    const int32_t y0 = ((int32_t)yl + viewwindowy > (int32_t)s_win_y0) ? (int32_t)yl + viewwindowy
                                                                       : (int32_t)s_win_y0;
    const int32_t y1 = ((int32_t)yh + viewwindowy < win_y1) ? (int32_t)yh + viewwindowy : win_y1;
    for (int32_t y = y0; y <= y1; y++) {
        s_map[((size_t)((uint32_t)y - s_win_y0) * OP_DOOM_SRC_W) + (size_t)fx] = s_cur;
    }
}

void I_SpriteDone(void)
{
    s_cur = 0U;
    s_mid_next = true;
}

void I_ThingDamaged(const mobj_t *target)
{
    if ((target == NULL) || (target->player != NULL) || !is_monster(target)) {
        return;
    }
    const uint32_t i = slot_of(target);
    s_slots[i].seen_tic = leveltime;
    s_slots[i].hit_until = leveltime + HIT_TICS;
    s_slots[i].hit_frames = HIT_FRAMES;
}
