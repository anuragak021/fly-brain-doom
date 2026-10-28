/**
 * @file telemetry.h
 * @brief Live text dashboard of the brain, drawn with ANSI escapes for a terminal on the
 *        ST-Link virtual COM port. Pure logic, no HAL: it formats text into a caller buffer.
 *
 * Shown only while the ESP32 plays and no laptop master has spoken on the VCP (see main.c), so
 * it never mixes with protocol frames. Values are exactly those in the last STEP request and
 * response; nothing is invented. Bars of the steering neurons are scaled to their recent peak
 * (labelled as such on screen) because the raw activities have no natural 0..1 range.
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brain_server.h"

/** Steps of action history drawn as one strip. */
#define TELEMETRY_HISTORY_STEPS 40U
/** Upper bound of one rendered screen, bytes. Size buffers with it. */
#define TELEMETRY_MAX_BYTES 2048U

/** Dashboard state: action history, step rate and bar scaling. */
typedef struct {
    uint8_t history[TELEMETRY_HISTORY_STEPS]; /**< 0 straight, 1 left, 2 right; oldest first */
    uint32_t seen_steps;     /**< step_count already pushed into the history */
    uint32_t rate_steps;     /**< step_count at the last rate sample */
    uint32_t rate_ms;        /**< time of the last rate sample */
    uint32_t rate_x10_hz;    /**< steps per second times ten */
    float peak;              /**< recent peak of the steering activities */
    bool cleared;            /**< the screen clear was already sent */
} telemetry_t;

/** @brief Reset the state. */
void telemetry_init(telemetry_t *t);

/**
 * @brief Take note of new steps (cheap; call every main loop pass, or at least once per STEP).
 * @param[in,out] t     State.
 * @param[in]     last  Server's last-step record.
 */
void telemetry_observe(telemetry_t *t, const brain_last_step_t *last);

/**
 * @brief Draw the whole screen.
 * @param[in,out] t         State (step rate is sampled here).
 * @param[in]     last      Server's last-step record.
 * @param[in]     now_ms    Millisecond clock.
 * @param[in]     core_khz  Core clock, to convert cycles to microseconds.
 * @param[in]     link      Name of the master link shown on screen (not NULL).
 * @param[out]    buf       Output text.
 * @param[in]     cap       Capacity of @p buf; TELEMETRY_MAX_BYTES is always enough.
 * @return Bytes written, or 0 if the screen did not fit or an argument was NULL.
 */
size_t telemetry_render(telemetry_t *t, const brain_last_step_t *last, uint32_t now_ms,
                        uint32_t core_khz, const char *link, char *buf, size_t cap);

#endif /* TELEMETRY_H */
