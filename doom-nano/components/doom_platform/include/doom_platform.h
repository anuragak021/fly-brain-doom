/**
 * @file doom_platform.h
 * @brief ESP32-S3 platform layer for the PrBoom engine: WAD from a flash partition, video
 *        through the OLED pipeline, ESP timer clock, silent sound.
 *
 * Written for doom-esp32 (original work). It implements the engine's I_* interface
 * (i_video.h, i_system.h, i_main.h, i_sound.h) plus I_MapWadFile, the hook added to
 * D_AddFile by the single marked engine patch.
 */
#ifndef DOOM_PLATFORM_H
#define DOOM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

#define DP_WAD_PARTITION_LABEL "wad"  /**< Data partition holding the IWAD (partitions.csv). */
#define DP_IWAD_NAME "doom1.wad"      /**< Name passed to -iwad; the only name the hook maps. */
#define DP_STATS_PERIOD_FRAMES (100U) /**< Frame timing is logged every this many frames. */

/** Platform status. */
typedef enum {
    DP_OK = 0,
    DP_ERR_ARG = 1,     /**< NULL or invalid configuration. */
    DP_ERR_NO_WAD = 2,  /**< No partition labelled DP_WAD_PARTITION_LABEL. */
    DP_ERR_MMAP = 3,    /**< esp_partition_mmap failed (MMU space or flash error). */
    DP_ERR_BAD_WAD = 4, /**< Partition does not hold a valid IWAD (see wad_check). */
    DP_ERR_DISPLAY = 5, /**< OLED init failed. */
} dp_status_t;

/** Board wiring. */
typedef struct {
    gpio_num_t oled_sda;
    gpio_num_t oled_scl;
    uint8_t oled_addr;
    uint32_t oled_freq_hz;
} dp_cfg_t;

/** Per stage timing, averaged over the last DP_STATS_PERIOD_FRAMES frames (microseconds). */
typedef struct {
    uint32_t frames;        /**< Frames shown since boot. */
    uint32_t frame_us;      /**< Wall time between consecutive I_FinishUpdate calls. */
    uint32_t walls_us;      /**< Wall mask (nano): marker fill and coverage (engine hooks). */
    uint32_t mask_us;       /**< Sprite mask: crop copy, compare and coverage (engine hooks). */
    uint32_t scale_us;      /**< Stage 1+2: LUT and area downscale. */
    uint32_t render_us;     /**< Stage 3: configured style (+ HUD), or outline without masks. */
    uint32_t pack_us;       /**< Stage 4: SSD1306 pack. */
    uint32_t i2c_us;        /**< I2C frame write. */
    uint32_t i2c_errors;    /**< Failed frame writes since boot. */
    uint32_t styled_frames; /**< Frames of the last period drawn in the configured style. */
} dp_frame_stats_t;

/**
 * @brief Map and validate the WAD partition, and bring up the OLED.
 * @param cfg Board wiring.
 * @return DP_OK or the first failure; on failure nothing that was set up is released, the
 *         caller is expected to stop (there is nothing to play).
 */
dp_status_t doom_platform_init(const dp_cfg_t *cfg);

/**
 * @brief Set up engine arguments and run the engine. Does not return.
 *
 * Requires a successful doom_platform_init(). Runs attract mode (title screen and the WAD's
 * demos) when there is no input.
 */
void doom_platform_run(void) __attribute__((noreturn));

/**
 * @brief Latest averaged frame timings.
 * @param out Output; ignored if NULL.
 */
void doom_platform_get_stats(dp_frame_stats_t *out);

/* ---- Internal, shared between the platform files (not for the application) ---- */

/** Map the WAD partition; returns DP_OK and the mapped bytes, validated. */
dp_status_t dp_wad_map(const uint8_t **data, size_t *len);

/** Video side init (OLED) and the frame statistics owner. */
dp_status_t dp_video_init(const dp_cfg_t *cfg);

#endif /* DOOM_PLATFORM_H */
