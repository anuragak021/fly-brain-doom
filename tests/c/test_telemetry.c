/**
 * @file test_telemetry.c
 * @brief Host test: the dashboard text contains the real numbers, stays inside the buffer and
 *        the history strip follows the actions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "telemetry.h"

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static brain_last_step_t sample(void)
{
    brain_last_step_t l;
    memset(&l, 0, sizeof(l));
    l.step_count = 7U;
    l.step_errors = 2U;
    l.features[BRAIN_FEAT_ENEMY_LEFT] = 0.5f;
    l.features[BRAIN_FEAT_DAMAGE_RIGHT] = 0.25f;
    l.reward = -0.75f;
    l.flags = BRAIN_STEP_FLAG_LEARN;
    l.resp.actions = BRAIN_ACTION_TURN_LEFT;
    l.resp.step_cycles = 1363200U; /* 21.3 ms at 64 MHz */
    l.resp.steer_left = 0.3f;
    l.resp.steer_right = 0.1f;
    l.resp.weight_drift = 0.18f;
    return l;
}

static void test_contents(void)
{
    telemetry_t t;
    char buf[TELEMETRY_MAX_BYTES];
    telemetry_init(&t);
    const brain_last_step_t l = sample();
    telemetry_observe(&t, &l);
    const size_t n = telemetry_render(&t, &l, 5000U, 64000U, "ESP32 USART1", buf, sizeof(buf));
    CHECK(n > 0U);
    CHECK(n < sizeof(buf));
    buf[n] = '\0';
    CHECK(strstr(buf, "64 MHz") != NULL);
    CHECK(strstr(buf, "ESP32 USART1") != NULL);
    CHECK(strstr(buf, "steps 7") != NULL);
    CHECK(strstr(buf, "errors 2") != NULL);
    CHECK(strstr(buf, "step 21.3 ms") != NULL);
    CHECK(strstr(buf, "drift 0.180") != NULL);
    CHECK(strstr(buf, "-0.75") != NULL);
    CHECK(strstr(buf, "0.50") != NULL);
    CHECK(strstr(buf, "TURN LEFT") != NULL);
    CHECK(strstr(buf, "ON") != NULL);
    CHECK(strstr(buf, "00:00:05") != NULL);
    CHECK(strstr(buf, "\x1b[2J") != NULL); /* first frame clears the screen */
    const size_t n2 = telemetry_render(&t, &l, 5250U, 64000U, "x", buf, sizeof(buf));
    CHECK(n2 > 0U);
    buf[n2] = '\0';
    CHECK(strstr(buf, "\x1b[2J") == NULL); /* later frames only home the cursor */
}

static void test_history_and_rate(void)
{
    telemetry_t t;
    char buf[TELEMETRY_MAX_BYTES];
    telemetry_init(&t);
    brain_last_step_t l = sample();
    l.step_count = 1U;
    l.resp.actions = BRAIN_ACTION_TURN_LEFT;
    telemetry_observe(&t, &l);
    l.step_count = 2U;
    l.resp.actions = BRAIN_ACTION_TURN_RIGHT;
    telemetry_observe(&t, &l);
    l.step_count = 3U;
    l.resp.actions = 0U;
    telemetry_observe(&t, &l);
    CHECK(t.history[TELEMETRY_HISTORY_STEPS - 3U] == 1U);
    CHECK(t.history[TELEMETRY_HISTORY_STEPS - 2U] == 2U);
    CHECK(t.history[TELEMETRY_HISTORY_STEPS - 1U] == 0U);
    telemetry_observe(&t, &l); /* no new step: unchanged */
    CHECK(t.seen_steps == 3U);
    /* 30 steps in 1000 ms is 30.0 Hz */
    l.step_count = 30U;
    (void)telemetry_render(&t, &l, 1000U, 64000U, "x", buf, sizeof(buf));
    CHECK(t.rate_x10_hz == 300U);
}

static void test_small_buffer_and_nulls(void)
{
    telemetry_t t;
    char tiny[32];
    telemetry_init(&t);
    const brain_last_step_t l = sample();
    CHECK(telemetry_render(&t, &l, 0U, 64000U, "x", tiny, sizeof(tiny)) == 0U);
    CHECK(telemetry_render(NULL, &l, 0U, 64000U, "x", tiny, sizeof(tiny)) == 0U);
    CHECK(telemetry_render(&t, NULL, 0U, 64000U, "x", tiny, sizeof(tiny)) == 0U);
    CHECK(telemetry_render(&t, &l, 0U, 64000U, NULL, tiny, sizeof(tiny)) == 0U);
    CHECK(telemetry_render(&t, &l, 0U, 64000U, "x", NULL, 10U) == 0U);
}

static void test_odd_values(void)
{
    telemetry_t t;
    char buf[TELEMETRY_MAX_BYTES];
    telemetry_init(&t);
    brain_last_step_t l = sample();
    l.resp.steer_left = 1.0e9f;      /* absurd: must still fit and not overflow */
    l.resp.weight_drift = -5.0f;
    l.resp.step_cycles = 0xFFFFFFFFU;
    l.reward = 1.0f;
    telemetry_observe(&t, &l);
    CHECK(telemetry_render(&t, &l, 0xFFFFFFFFU, 0U, "x", buf, sizeof(buf)) > 0U);
}

int main(void)
{
    test_contents();
    test_history_and_rate();
    test_small_buffer_and_nulls();
    test_odd_values();
    if (g_failures != 0) {
        (void)printf("%d failures\n", g_failures);
        return 1;
    }
    (void)printf("telemetry: all checks passed\n");
    return 0;
}
