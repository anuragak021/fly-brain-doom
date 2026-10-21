/**
 * @file bl_client.h
 * @brief ESP32 side of the brain link: a task that talks the brain protocol (UART1) to the STM32
 *        and hands the latest actions to the engine. The engine never waits for the brain.
 *
 * The engine task calls bl_client_submit() once per rendered 3D frame and bl_client_snapshot()
 * once per tic. The link task, pinned to the other core, steps the brain continuously with the
 * latest features (a STEP round trip measured 28 to 29 ms with the STM32 at 64 MHz, about the length of
 * a Doom tic, so waiting for it in the engine would halve the game rate).
 *
 * Protocol: owned by the drosophila project (firmware/include/brain_protocol.h), compiled into
 * this component from there and never redefined.
 */
#ifndef BL_CLIENT_H
#define BL_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "bl_features.h"
#include "driver/gpio.h"

/** Link wiring and speed. */
typedef struct {
    gpio_num_t tx; /**< ESP32 TX, to the STM32 USART1 RX (PB7, Nucleo D0). */
    gpio_num_t rx; /**< ESP32 RX, from the STM32 USART1 TX (PB6, Nucleo D1). */
    uint32_t baud;
} bl_cfg_t;

/** What the engine needs for one tic. */
typedef struct {
    bool brain_ok;   /**< A STEP reply arrived recently enough to act on. */
    uint8_t actions; /**< BRAIN_ACTION_* bits of the latest reply. */
    uint32_t reply_age_ms;
    float steer_left;   /**< Latest reply's mean left steering activity (for logging). */
    float steer_right;  /**< Latest reply's mean right steering activity. */
    float weight_drift; /**< Latest reply's learned-weight drift (0 = connectome weights). */
    bl_features_t view; /**< Latest submitted features, enemy and wall information. */
} bl_snapshot_t;

/**
 * @brief Install the UART driver and start the link task.
 * @param cfg Wiring; must not be NULL.
 * @return BL_OK, BL_ERR_NULL, or BL_ERR_SIZE if the UART or task could not be set up.
 */
bl_status_t bl_client_start(const bl_cfg_t *cfg);

/** @brief Publish the features of the frame just rendered (engine task). */
void bl_client_submit(const bl_features_t *features);

/**
 * @brief Game signals for the next STEP (engine task, once per poll).
 *
 * The damage pair replaces the previous one (it is sent as features 4 and 5). The reward is
 * added up until the link task sends it (clamped to -1..+1 per STEP) and then zeroed.
 * Learning is requested only while @p learn_ok is true and CONFIG_DOOM_BRAIN_LEARN is set.
 *
 * @param damage_left   DAMAGE_LEFT, 0..1 (bl_damage_make).
 * @param damage_right  DAMAGE_RIGHT, 0..1.
 * @param reward        Reward earned since the last call (kills minus health lost).
 * @param learn_ok      The player is alive in a level (not menu, pause, death or intermission).
 */
void bl_client_game(float damage_left, float damage_right, float reward, bool learn_ok);

/** @brief Latest actions and enemy position (engine task). */
void bl_client_snapshot(bl_snapshot_t *out);

#endif /* BL_CLIENT_H */
