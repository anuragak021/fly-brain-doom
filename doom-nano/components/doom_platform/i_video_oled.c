/**
 * @file i_video_oled.c
 * @brief Engine video interface (i_video.h) on the 128x64 SSD1306: the engine draws 320x200
 *        palette indices into a static framebuffer; I_FinishUpdate runs the Phase 1 pipeline
 *        (LUT + crop + area downscale, display style, page pack) and writes the panel.
 *
 * Display style (Kconfig DOOM_OLED_STYLE): nano (default), layers or outline. The first two
 * need masks from the engine:
 * - Sprite mask: the engine calls I_SpritesBegin / I_SpritesEnd (doom-esp32 patch in
 *   R_DrawMasked) around the drawing of sprites. Layers: the difference of the crop window
 *   between those two points is exactly the pixels of monsters, items and the player's weapon.
 *   Nano: the engine also reports each sprite and column post it draws, and i_sprite_ids.c
 *   builds a map of which sprite (id, class) covers each pixel instead.
 * - Wall mask (nano only): the engine calls I_WallsBegin / I_WallsEnd (doom-esp32 patch in
 *   R_RenderPlayerView) around the wall pass. Begin fills the view with a marker palette index;
 *   after the walls, every pixel that is not the marker is a wall.
 * Frames without a rendered 3D view (title, intermission, automap, screen wipes) have no masks,
 * and the nano style also skips frames with the menu open; those use the outline style.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bl_client.h"
#include "bl_features.h"
#include "doom_platform.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "i_brain_input.h"
#include "i_sprite_ids.h"
#include "oled_ssd1306.h"
#include "op_gray.h"
#include "op_hud.h"
#include "op_mask.h"
#include "op_nano.h"
#include "op_outline.h"
#include "op_pack.h"
#include "op_scale.h"
#include "sdkconfig.h"

/* Engine headers. */
#include "d_items.h"
#include "doomdef.h"
#include "doomstat.h"
#include "i_video.h"
#include "r_main.h"
#include "st_stuff.h"
#include "v_video.h"
#include "w_wad.h"

static const char *TAG = "video";

#define FB_BYTES (OP_DOOM_SRC_W * OP_DOOM_SRC_H)
#define PLAYPAL_ONE_BYTES (OP_PALETTE_BYTES)
#define PLAYPAL_COUNT (14U) /* palettes in PLAYPAL: normal, 8 red, 4 gold, 1 green */
#define OLED_PIXELS (OP_OLED_WIDTH_PX * OP_OLED_HEIGHT_PX)

#if defined(CONFIG_DOOM_OLED_STYLE_NANO)
#define SPRITE_MASK_ON (true)
#define WALL_MASK_ON (true)
#elif defined(CONFIG_DOOM_OLED_STYLE_LAYERS)
#define SPRITE_MASK_ON (true)
#define WALL_MASK_ON (false)
#else
#define SPRITE_MASK_ON (false)
#define WALL_MASK_ON (false)
#endif

/** How one frame is drawn. */
typedef enum {
    DRAW_OUTLINE = 0, /**< Whole 128x64, no masks needed. */
    DRAW_LAYERS = 1,  /**< Whole 128x64 with the sprite mask. */
    DRAW_NANO = 2,    /**< 128x56 view with both masks, HUD line below. */
} draw_mode_t;

/* Framebuffer in internal RAM: the renderer writes every pixel every frame, and PSRAM through
 * the cache would slow the column and span drawers. 64000 bytes fits in the S3's DRAM. */
static DRAM_ATTR uint8_t s_fb[FB_BYTES];
static uint8_t s_lut[OP_LUT_ENTRIES];
static uint8_t s_gray[OLED_PIXELS];
static uint8_t s_bits[OLED_PIXELS];
static uint8_t s_packed[OP_OLED_PACKED_BYTES];
static op_scale_cfg_t s_scale; /* full panel: outline and layers */
static op_scale_cfg_t s_view;  /* the 3D view of the configured style (nano: 128x56) */
static op_outline_cfg_t s_outline;
static op_layers_cfg_t s_layers;
static op_nano_cfg_t s_nano;
static op_gray_cfg_t s_gray_cfg;

/* Layers: the view's crop window before the sprites, then (after op_mask_diff) the 0/1 change
 * map. Nano: the sprite id map of the crop window (i_sprite_ids.c).
 * In PSRAM (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY): sized for the largest window any
 * style may use (the whole 3D view, 53,760 bytes), which internal RAM cannot spare, and it is
 * only touched twice per frame (one copy, one compare). */
static EXT_RAM_BSS_ATTR uint8_t s_snap[OP_MASK_MAX_SRC_BYTES];
static size_t s_win_offset; /* the view's crop window in s_fb (op_mask_window) */
static size_t s_win_bytes;
static uint8_t s_cov[OLED_PIXELS];      /* sprite coverage on the view grid */
static uint8_t s_wall_cov[OLED_PIXELS]; /* wall coverage on the view grid */
static uint8_t s_ids[OLED_PIXELS];      /* nano: front-most sprite id on the view grid */
static uint8_t s_modes[OP_NANO_IDS];    /* nano: contrast mode op_nano chose per id */
static bool s_snap_taken;               /* I_SpritesBegin ran for the frame being rendered */
static bool s_walls_taken;              /* I_WallsBegin ran for the frame being rendered */
static bool s_cov_valid;   /* s_cov belongs to the frame in s_fb; cleared after each update */
static bool s_wall_valid;  /* s_wall_cov likewise */
static int64_t s_mask_us;  /* sprite mask time of the current frame */
static int64_t s_walls_us; /* wall mask time of the current frame */

/* Statistics: sums over the current period, published averages, and the last frame time. */
typedef struct {
    uint64_t frame_us, walls_us, mask_us, scale_us, render_us, pack_us, i2c_us;
    uint32_t n, styled_n;
} stat_sums_t;
static stat_sums_t s_sums;
static dp_frame_stats_t s_stats;
static int64_t s_last_frame_us;

_Static_assert(FB_BYTES == 64000U, "Doom framebuffer is 320x200");
_Static_assert(OP_MASK_MAX_SRC_BYTES <= FB_BYTES, "largest crop window inside fb");

dp_status_t dp_video_init(const dp_cfg_t *cfg)
{
    if (cfg == NULL) {
        return DP_ERR_ARG;
    }
    if ((op_scale_doom_default(&s_scale) != OP_OK) ||
        (op_outline_cfg_default(&s_outline) != OP_OK) ||
        (op_layers_cfg_default(&s_layers) != OP_OK) || (op_nano_cfg_default(&s_nano) != OP_OK) ||
        (op_gray_cfg_default(&s_gray_cfg) != OP_OK)) {
        return DP_ERR_ARG;
    }
    s_view = s_scale;
    if (WALL_MASK_ON && (op_scale_doom_hud_view(&s_view) != OP_OK)) {
        return DP_ERR_ARG;
    }
    if (op_mask_window(&s_view, &s_win_offset, &s_win_bytes) != OP_OK) {
        return DP_ERR_ARG;
    }
    const oled_cfg_t oled = {
        .sda = cfg->oled_sda,
        .scl = cfg->oled_scl,
        .addr = cfg->oled_addr,
        .freq_hz = cfg->oled_freq_hz,
        .internal_pullup = true,
    };
    const oled_status_t st = oled_init(&oled);
    if (st != OLED_OK) {
        ESP_LOGE(TAG, "OLED init failed: %d", (int)st);
        return DP_ERR_DISPLAY;
    }
    return DP_OK;
}

void doom_platform_get_stats(dp_frame_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}

void I_InitGraphics(void)
{
    /* Same screen setup as the reference port: three full screens and the status bar. Only
     * screens[0] is static; the engine allocates the others from its zone (PSRAM). */
    for (int i = 0; i < 3; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENWIDTH;
    }
    screens[0].data = s_fb;
    screens[0].not_on_heap = true;
    screens[4].width = SCREENWIDTH;
    screens[4].height = (ST_SCALED_HEIGHT + 1);
    screens[4].byte_pitch = SCREENWIDTH;
    s_last_frame_us = esp_timer_get_time();
}

void I_SetPalette(int pal)
{
    /* Gray LUT from the raw PLAYPAL bytes, not V_BuildPalette (which returns RGB565 with the
     * engine's gamma applied). Damage and pickup flashes switch pal, so they show on the OLED. */
    if ((pal < 0) || ((uint32_t)pal >= PLAYPAL_COUNT)) {
        return;
    }
    const int lump = W_GetNumForName("PLAYPAL");
    if ((uint32_t)W_LumpLength(lump) < (PLAYPAL_COUNT * PLAYPAL_ONE_BYTES)) {
        ESP_LOGE(TAG, "PLAYPAL too short");
        return;
    }
    const uint8_t *playpal = (const uint8_t *)W_CacheLumpNum(lump);
    const op_status_t st =
        op_gray_build_lut(&playpal[(size_t)pal * PLAYPAL_ONE_BYTES], &s_gray_cfg, s_lut);
    W_UnlockLumpNum(lump);
    if (st != OP_OK) {
        ESP_LOGE(TAG, "gray LUT failed: %d", (int)st);
    }
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) {
        return lo;
    }
    return (v > hi) ? hi : v;
}

/* Called by the engine (doom-esp32 patch in r_main.c R_RenderPlayerView) just before the wall
 * pass. Fills the part of the crop window that the 3D view covers with the marker; walls then
 * overwrite it. Pixels of the crop outside the view (only with a shrunk view, screen size below
 * 10) keep the border graphic and count as wall. */
void I_WallsBegin(void)
{
    if (!WALL_MASK_ON) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    const int32_t win_y0 = (int32_t)s_view.crop_y;
    const int32_t win_y1 = win_y0 + (int32_t)s_view.crop_h;
    const int32_t y0 = clamp_i32(viewwindowy, win_y0, win_y1);
    const int32_t y1 = clamp_i32(viewwindowy + viewheight, y0, win_y1);
    const int32_t x0 = clamp_i32(viewwindowx, 0, (int32_t)OP_DOOM_SRC_W);
    const int32_t x1 = clamp_i32(viewwindowx + viewwidth, x0, (int32_t)OP_DOOM_SRC_W);
    for (int32_t y = y0; y < y1; y++) {
        (void)memset(&s_fb[((size_t)y * OP_DOOM_SRC_W) + (size_t)x0], (int)OP_MASK_WALL_MARKER,
                     (size_t)(x1 - x0));
    }
    s_walls_taken = true;
    s_walls_us = esp_timer_get_time() - t0;
}

/* Called by the engine right after the wall pass (before floors, ceilings and sprites). */
void I_WallsEnd(void)
{
    if (!s_walls_taken) {
        return;
    }
    s_walls_taken = false;
    const int64_t t0 = esp_timer_get_time();
    const op_status_t st =
        op_mask_wall_coverage(&s_view, s_fb, (uint8_t)OP_MASK_WALL_MARKER, s_wall_cov);
    s_wall_valid = (st == OP_OK);
    if (st != OP_OK) {
        ESP_LOGE(TAG, "wall mask failed: %d", (int)st);
    }
    s_walls_us += esp_timer_get_time() - t0;
}

/* Called by the engine (doom-esp32 patch in r_things.c R_DrawMasked) after the level and
 * before any sprite is drawn. */
void I_SpritesBegin(void)
{
    if (!SPRITE_MASK_ON) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    if (WALL_MASK_ON) {
        sid_begin(s_snap, s_view.crop_y, s_view.crop_h);
    } else {
        (void)memcpy(s_snap, &s_fb[s_win_offset], s_win_bytes);
    }
    s_snap_taken = true;
    s_mask_us = esp_timer_get_time() - t0;
}

/* Called by the engine at the end of R_DrawMasked, after all sprites and the weapon. */
void I_SpritesEnd(void)
{
    if (!s_snap_taken) {
        return;
    }
    s_snap_taken = false;
    const int64_t t0 = esp_timer_get_time();
    op_status_t st = OP_OK;
    if (WALL_MASK_ON) {
        sid_end();
        st = op_mask_ids(&s_view, s_snap, s_ids);
    } else {
        st = op_mask_diff(s_snap, &s_fb[s_win_offset], s_win_bytes);
    }
    if (st == OP_OK) {
        st = op_mask_coverage_cfg(&s_view, s_snap, s_cov);
    }
    s_cov_valid = (st == OP_OK);
    if (st != OP_OK) {
        ESP_LOGE(TAG, "sprite mask failed: %d", (int)st);
    }
    s_mask_us += esp_timer_get_time() - t0;
}

static draw_mode_t pick_mode(void)
{
    if (WALL_MASK_ON) {
        /* The menu is drawn over the view after the masks were taken; the nano style would
         * black out its text on floors and flatten it on walls. */
        return (s_cov_valid && s_wall_valid && (menuactive == 0)) ? DRAW_NANO : DRAW_OUTLINE;
    }
    if (SPRITE_MASK_ON) {
        return s_cov_valid ? DRAW_LAYERS : DRAW_OUTLINE;
    }
    return DRAW_OUTLINE;
}

/** Health, the brain's kill count and the ready weapon's ammo of the player shown. */
static op_hud_t hud_values(void)
{
    const player_t *p = &players[displayplayer];
    op_hud_t hud = {p->health, ib_session_kills(), 0, false};
    if (((uint32_t)p->readyweapon < (uint32_t)NUMWEAPONS)) {
        const ammotype_t ammo = weaponinfo[p->readyweapon].ammo;
        if ((ammo != am_noammo) && ((uint32_t)ammo < (uint32_t)NUMAMMO)) {
            hud.ammo = p->ammo[ammo];
            hud.show_ammo = true;
        }
    }
    return hud;
}

/** Stage 3 for the chosen mode, from s_gray (already scaled for that mode) into s_bits. */
static op_status_t render_style(draw_mode_t mode)
{
    switch (mode) {
    case DRAW_NANO: {
        const op_nano_sprites_t spr = {s_cov, s_ids, sid_classes(), sid_prev_modes(), s_modes};
        const op_status_t st =
            op_nano(s_gray, s_wall_cov, &spr, s_view.dst_w, s_view.dst_h, &s_nano, s_bits);
        if (st != OP_OK) {
            return st;
        }
        sid_store_modes(s_modes);
        const op_hud_t hud = hud_values();
        return op_hud_draw(&hud, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, s_bits);
    }
    case DRAW_LAYERS:
        return op_layers(s_gray, s_cov, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, &s_layers, s_bits);
    case DRAW_OUTLINE:
    default:
        return op_outline(s_gray, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, &s_outline, s_bits);
    }
}

static void publish_stats_if_due(void)
{
    s_sums.n++;
    if (s_sums.n < DP_STATS_PERIOD_FRAMES) {
        return;
    }
    const uint64_t n = s_sums.n;
    s_stats.frame_us = (uint32_t)(s_sums.frame_us / n);
    s_stats.walls_us = (uint32_t)(s_sums.walls_us / n);
    s_stats.mask_us = (uint32_t)(s_sums.mask_us / n);
    s_stats.styled_frames = s_sums.styled_n;
    s_stats.scale_us = (uint32_t)(s_sums.scale_us / n);
    s_stats.render_us = (uint32_t)(s_sums.render_us / n);
    s_stats.pack_us = (uint32_t)(s_sums.pack_us / n);
    s_stats.i2c_us = (uint32_t)(s_sums.i2c_us / n);
    const uint32_t fps_x10 = (s_stats.frame_us > 0U) ? (10000000U / s_stats.frame_us) : 0U;
    ESP_LOGI(TAG,
             "frames %lu: %lu.%lu fps | frame %lu us = walls %lu + sprites %lu + scale %lu + "
             "style %lu + pack %lu + i2c %lu + engine | styled %lu/%lu | i2c errors %lu",
             (unsigned long)s_stats.frames, (unsigned long)(fps_x10 / 10U),
             (unsigned long)(fps_x10 % 10U), (unsigned long)s_stats.frame_us,
             (unsigned long)s_stats.walls_us, (unsigned long)s_stats.mask_us,
             (unsigned long)s_stats.scale_us, (unsigned long)s_stats.render_us,
             (unsigned long)s_stats.pack_us, (unsigned long)s_stats.i2c_us,
             (unsigned long)s_stats.styled_frames, (unsigned long)n,
             (unsigned long)s_stats.i2c_errors);
    s_sums = (stat_sums_t){0};
}

void I_FinishUpdate(void)
{
    const int64_t t0 = esp_timer_get_time();
    const draw_mode_t mode = pick_mode();
    const op_scale_cfg_t *scale = (mode == DRAW_NANO) ? &s_view : &s_scale;
    op_status_t st = op_scale_area(scale, s_fb, s_lut, s_gray);
    const int64_t t1 = esp_timer_get_time();
#if defined(CONFIG_DOOM_BRAIN_PLAY)
    if ((st == OP_OK) && (mode == DRAW_NANO)) {
        /* Features only from frames with both masks; other screens leave the last ones to age. */
        bl_features_t feat;
        /* DRAW_NANO implies s_wall_valid. The wall fields feed heuristics only, not the brain. */
        if ((bl_features_make(s_gray, s_ids, sid_classes(), s_view.dst_w, s_view.dst_h, &feat) ==
             BL_OK) &&
            (bl_walls_make(s_wall_cov, s_view.dst_w, s_view.dst_h, &feat) == BL_OK)) {
            bl_client_submit(&feat);
        }
    }
#endif
    if (st == OP_OK) {
        st = render_style(mode);
    }
    /* The masks belong to this rendered frame only; wipes and non-3D screens call
     * I_FinishUpdate again without rendering, and must not reuse them. */
    s_cov_valid = false;
    s_wall_valid = false;
    const int64_t mask_us = s_mask_us;
    const int64_t walls_us = s_walls_us;
    s_mask_us = 0;
    s_walls_us = 0;
    const int64_t t2 = esp_timer_get_time();
    if (st == OP_OK) {
        st =
            op_pack_ssd1306(s_bits, OP_OLED_WIDTH_PX, OP_OLED_HEIGHT_PX, s_packed, sizeof s_packed);
    }
    const int64_t t3 = esp_timer_get_time();
    if (st != OP_OK) {
        ESP_LOGE(TAG, "pipeline failed: %d", (int)st);
        return;
    }
    if (oled_write_frame(s_packed) != OLED_OK) {
        s_stats.i2c_errors++;
    }
    const int64_t t4 = esp_timer_get_time();
    s_sums.frame_us += (uint64_t)(t4 - s_last_frame_us);
    s_sums.walls_us += (uint64_t)walls_us;
    s_sums.mask_us += (uint64_t)mask_us;
    s_sums.styled_n += (mode != DRAW_OUTLINE) ? 1U : 0U;
    s_sums.scale_us += (uint64_t)(t1 - t0);
    s_sums.render_us += (uint64_t)(t2 - t1);
    s_sums.pack_us += (uint64_t)(t3 - t2);
    s_sums.i2c_us += (uint64_t)(t4 - t3);
    s_last_frame_us = t4;
    s_stats.frames++;
    (void)esp_task_wdt_reset(); /* ESP_ERR_NOT_FOUND only if the add in doom_platform_run failed */
    publish_stats_if_due();
}

void I_UpdateNoBlit(void)
{
}

void I_StartFrame(void)
{
}

bool I_StartDisplay(void)
{
    return true;
}

void I_EndDisplay(void)
{
}
