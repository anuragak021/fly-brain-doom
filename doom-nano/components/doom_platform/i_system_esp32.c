/**
 * @file i_system_esp32.c
 * @brief Engine system interface (i_system.h, i_main.h) on the ESP32-S3, plus the entry
 *        points doom_platform_init() and doom_platform_run().
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "doom_platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "op_scale.h"

/* Engine headers. */
#include "d_main.h"
#include "doomdef.h"
#include "i_brain_input.h"
#include "i_main.h"
#include "i_system.h"
#include "m_argv.h"
#include "r_fps.h"
#include "sdkconfig.h"
#include "z_zone.h"

static const char *TAG = "doom";

#define MS_PER_S (1000LL)
#define US_PER_MS (1000LL)
#define CLOCK_RATE_DENOM (100LL) /* realtic_clock_rate is a percentage */
#if defined(CONFIG_DOOM_BRAIN_PLAY)
/* Start E1M<CONFIG_DOOM_BRAIN_MAP> directly (skips the title screen and the demos) at the
 * configured skill. */
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
#define ARGV_SLOTS (10U)
static const char *s_argv[ARGV_SLOTS] = {"doom",
                                         "-iwad",
                                         DP_IWAD_NAME,
                                         "-nosound",
                                         "-warp",
                                         "1",
                                         STRINGIFY(CONFIG_DOOM_BRAIN_MAP),
                                         "-skill",
                                         STRINGIFY(CONFIG_DOOM_BRAIN_SKILL),
                                         NULL};
#else
#define ARGV_SLOTS (5U)
static const char *s_argv[ARGV_SLOTS] = {"doom", "-iwad", DP_IWAD_NAME, "-nosound", NULL};
#endif
static bool s_ready;

dp_status_t doom_platform_init(const dp_cfg_t *cfg)
{
    if (cfg == NULL) {
        return DP_ERR_ARG;
    }
    const uint8_t *wad = NULL;
    size_t len = 0U;
    dp_status_t st = dp_wad_map(&wad, &len);
    if (st == DP_OK) {
        st = dp_video_init(cfg);
    }
    s_ready = (st == DP_OK);
    return st;
}

void doom_platform_run(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "doom_platform_init() did not succeed; not starting the engine");
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    SCREENWIDTH = (int)OP_DOOM_SRC_W;
    SCREENHEIGHT = (int)OP_DOOM_SRC_H;
    myargv = s_argv;
    myargc = (int)(ARGV_SLOTS - 1U);
    /* Prefer PSRAM for every malloc (the engine's zone, lump caches, level data). Only the
     * framebuffer is in internal RAM, as a static. Same policy as the reference port. */
    heap_caps_malloc_extmem_enable(0);
    ESP_LOGI(TAG, "starting engine: internal free %u, PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* Watch the engine task itself: I_FinishUpdate feeds the watchdog every frame, so a hang in
     * the engine loop is reported (warn only, no panic: a slow level load must not reboot). */
    const esp_err_t wdt = esp_task_wdt_add(NULL);
    if (wdt != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog not watching the engine: %s", esp_err_to_name(wdt));
    }
    Z_Init();
    D_DoomMain(); /* never returns: the engine's main loop */
    ESP_LOGE(TAG, "D_DoomMain returned");
    abort();
}

void I_Init(void)
{
    /* No settings store yet: engine defaults. Sound is off (silent stubs). */
}

int I_GetTimeMS(void)
{
    return (int)(esp_timer_get_time() / US_PER_MS);
}

int I_GetTime(void)
{
    /* Tics since boot, in 64 bit: the reference port's int expression ms * 35 * 100 overflows
     * after about 10 minutes of uptime. */
    const int64_t ms = esp_timer_get_time() / US_PER_MS;
    return (int)((ms * TICRATE * realtic_clock_rate) / (MS_PER_S * CLOCK_RATE_DENOM));
}

void I_uSleep(unsigned long usecs)
{
    const TickType_t ticks = pdMS_TO_TICKS(usecs / (unsigned long)US_PER_MS);
    if (ticks > 0U) {
        vTaskDelay(ticks); /* yields, so the idle task can feed the watchdog */
    } else {
        esp_rom_delay_us((uint32_t)usecs);
    }
}

const char *I_DoomExeDir(void)
{
    return "/nofs"; /* no filesystem: config and savegame opens fail and defaults are used */
}

void I_SafeExit(int rc)
{
    ESP_LOGE(TAG, "engine exit, code %d; restarting", rc);
    esp_restart();
}

/* newlib on ESP-IDF declares access() but no VFS provides it. There is no filesystem in this
 * build (WADs come from flash through I_MapWadFile), so every path reports "not found",
 * which is what the engine expects for a missing config, savegame or demo file. */
int access(const char *path, int mode)
{
    (void)path;
    (void)mode;
    errno = ENOENT;
    return -1;
}

void I_StartTic(void)
{
#if defined(CONFIG_DOOM_BRAIN_PLAY)
    ib_poll(); /* the brain's replies become key events */
#endif
    /* Without CONFIG_DOOM_BRAIN_PLAY there is no input and the engine runs attract mode:
     * title screen and the WAD's demos. */
}
