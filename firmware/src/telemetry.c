/**
 * @file telemetry.c
 * @brief Live ANSI dashboard of the brain. See telemetry.h.
 */
#include "telemetry.h"

#include <math.h>
#include <stddef.h>

#define ANSI_RESET "\x1b[0m"
#define ANSI_BOLD "\x1b[1m"
#define ANSI_DIM "\x1b[2m"
#define ANSI_RED "\x1b[31m"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_CYAN "\x1b[36m"
#define ANSI_HOME "\x1b[H"
#define ANSI_CLEAR_HIDE_CURSOR "\x1b[2J\x1b[?25l"
#define ANSI_CLEAR_BELOW "\x1b[J"
#define EOL "\x1b[K\r\n" /* erase the rest of the line, so shorter text leaves nothing behind */

#define GLYPH_FULL "\xE2\x96\x88"  /* U+2588 */
#define GLYPH_EMPTY "\xE2\x96\x91" /* U+2591 */
#define GLYPH_RULE "\xE2\x94\x80"  /* U+2500 */
#define GLYPH_LEFT "\xE2\x97\x80"  /* U+25C0 */
#define GLYPH_RIGHT "\xE2\x96\xB6" /* U+25B6 */
#define GLYPH_STRAIGHT "\xC2\xB7"  /* U+00B7 */

#define BAR_CELLS 10U
#define HALF_BAR_CELLS 8U
#define RULE_CELLS 62U
#define NUM_TMP_BYTES 16U
#define PEAK_FLOOR 0.01f
#define PEAK_DECAY 0.995f
#define RATE_WINDOW_MS 1000U
#define RATE_SCALE_X10 10000U /* steps * 1000 ms * 10 */
#define MS_PER_S 1000U
#define S_PER_MIN 60U
#define S_PER_HOUR 3600U
#define US_PER_MS 1000U
#define TENTHS_MS_PER_US 100U
#define DECIMAL_BASE 10U
#define KHZ_PER_MHZ 1000U
#define MAX_PRINTABLE 9999.0f
#define ROUND_HALF 0.5f

#define HIST_STRAIGHT 0U
#define HIST_LEFT 1U
#define HIST_RIGHT 2U

/** Bounded text builder: never writes past the buffer, remembers an overflow. */
typedef struct {
    char *b;
    size_t cap;
    size_t len;
    bool overflow;
} sb_t;

static void sb_char(sb_t *s, char c)
{
    if ((s->len + 1U) < s->cap) {
        s->b[s->len] = c;
        s->len++;
    } else {
        s->overflow = true;
    }
}

static void sb_str(sb_t *s, const char *str)
{
    for (size_t i = 0U; str[i] != '\0'; i++) {
        sb_char(s, str[i]);
    }
}

static void sb_repeat(sb_t *s, const char *str, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++) {
        sb_str(s, str);
    }
}

static void sb_u32(sb_t *s, uint32_t v)
{
    char tmp[NUM_TMP_BYTES];
    size_t n = 0U;
    do {
        tmp[n] = (char)('0' + (char)(v % DECIMAL_BASE));
        n++;
        v /= DECIMAL_BASE;
    } while ((v != 0U) && (n < sizeof(tmp)));
    while (n > 0U) {
        n--;
        sb_char(s, tmp[n]);
    }
}

static void sb_u32_2d(sb_t *s, uint32_t v)
{
    sb_char(s, (char)('0' + (char)((v / DECIMAL_BASE) % DECIMAL_BASE)));
    sb_char(s, (char)('0' + (char)(v % DECIMAL_BASE)));
}

/** Signed fixed point with @p decimals digits (1..3), no printf/float support needed. */
static void sb_fixed(sb_t *s, float v, uint32_t decimals)
{
    uint32_t scale = 1U;
    for (uint32_t i = 0U; i < decimals; i++) {
        scale *= DECIMAL_BASE;
    }
    float mag = fabsf(v);
    if (!(mag <= MAX_PRINTABLE)) { /* also catches NaN */
        mag = MAX_PRINTABLE;
    }
    const uint32_t n = (uint32_t)((mag * (float)scale) + ROUND_HALF);
    if ((v < 0.0f) && (n != 0U)) {
        sb_char(s, '-');
    }
    sb_u32(s, n / scale);
    sb_char(s, '.');
    uint32_t div = scale / DECIMAL_BASE;
    while (div != 0U) {
        sb_char(s, (char)('0' + (char)((n / div) % DECIMAL_BASE)));
        div /= DECIMAL_BASE;
    }
}

static float clamp01(float v)
{
    if (!(v > 0.0f)) {
        return 0.0f;
    }
    return (v > 1.0f) ? 1.0f : v;
}

/** A bar of BAR_CELLS cells, @p frac of it filled, in @p color. */
static void sb_bar(sb_t *s, float frac, const char *color)
{
    const uint32_t full = (uint32_t)((clamp01(frac) * (float)BAR_CELLS) + ROUND_HALF);
    sb_str(s, color);
    sb_repeat(s, GLYPH_FULL, full);
    sb_str(s, ANSI_DIM);
    sb_repeat(s, GLYPH_EMPTY, BAR_CELLS - full);
    sb_str(s, ANSI_RESET);
}

/** Two-sided bar: negative to the left in red, positive to the right in green. */
static void sb_signed_bar(sb_t *s, float v)
{
    const float mag = clamp01(fabsf(v));
    const uint32_t cells = (uint32_t)((mag * (float)HALF_BAR_CELLS) + ROUND_HALF);
    const bool neg = v < 0.0f;
    sb_str(s, ANSI_DIM);
    sb_repeat(s, GLYPH_EMPTY, neg ? (HALF_BAR_CELLS - cells) : HALF_BAR_CELLS);
    sb_str(s, ANSI_RED);
    sb_repeat(s, GLYPH_FULL, neg ? cells : 0U);
    sb_str(s, ANSI_RESET "|" ANSI_GREEN);
    sb_repeat(s, GLYPH_FULL, neg ? 0U : cells);
    sb_str(s, ANSI_DIM);
    sb_repeat(s, GLYPH_EMPTY, neg ? HALF_BAR_CELLS : (HALF_BAR_CELLS - cells));
    sb_str(s, ANSI_RESET);
}

static void sb_label(sb_t *s, const char *name)
{
    sb_str(s, ANSI_BOLD " ");
    sb_str(s, name);
    sb_str(s, ANSI_RESET " ");
}

static void sb_section(sb_t *s, const char *title)
{
    sb_str(s, ANSI_CYAN " ");
    sb_repeat(s, GLYPH_RULE, 2U);
    sb_str(s, " ");
    sb_str(s, title);
    sb_str(s, " ");
    sb_str(s, ANSI_DIM);
    sb_repeat(s, GLYPH_RULE, 4U);
    sb_str(s, ANSI_RESET EOL);
}

/** Left and right value with a bar each; the bar fills to value / @p full_scale. */
static void sb_pair(sb_t *s, const char *name, float left, float right, float full_scale,
                    const char *color)
{
    sb_label(s, name);
    sb_str(s, " L ");
    sb_bar(s, left / full_scale, color);
    sb_char(s, ' ');
    sb_fixed(s, left, 2U);
    sb_str(s, "   R ");
    sb_bar(s, right / full_scale, color);
    sb_char(s, ' ');
    sb_fixed(s, right, 2U);
    sb_str(s, EOL);
}

void telemetry_init(telemetry_t *t)
{
    if (t == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < TELEMETRY_HISTORY_STEPS; i++) {
        t->history[i] = HIST_STRAIGHT;
    }
    t->seen_steps = 0U;
    t->rate_steps = 0U;
    t->rate_ms = 0U;
    t->rate_x10_hz = 0U;
    t->peak = PEAK_FLOOR;
    t->cleared = false;
}

static uint8_t history_code(uint8_t actions)
{
    if ((actions & (uint8_t)BRAIN_ACTION_TURN_LEFT) != 0U) {
        return HIST_LEFT;
    }
    return ((actions & (uint8_t)BRAIN_ACTION_TURN_RIGHT) != 0U) ? HIST_RIGHT : HIST_STRAIGHT;
}

static float max3(float a, float b, float c)
{
    const float m = (a > b) ? a : b;
    return (m > c) ? m : c;
}

void telemetry_observe(telemetry_t *t, const brain_last_step_t *last)
{
    if ((t == NULL) || (last == NULL) || (last->step_count == t->seen_steps)) {
        return;
    }
    uint32_t fresh = last->step_count - t->seen_steps; /* unsigned: wrap-safe */
    if (fresh > TELEMETRY_HISTORY_STEPS) {
        fresh = TELEMETRY_HISTORY_STEPS;
    }
    for (uint32_t k = 0U; k < fresh; k++) {
        for (uint32_t i = 1U; i < TELEMETRY_HISTORY_STEPS; i++) {
            t->history[i - 1U] = t->history[i];
        }
        t->history[TELEMETRY_HISTORY_STEPS - 1U] = history_code(last->resp.actions);
    }
    t->seen_steps = last->step_count;
    const float m = max3(last->resp.steer_left, last->resp.steer_right, last->resp.forward);
    const float decayed = t->peak * PEAK_DECAY;
    t->peak = (m > decayed) ? m : decayed;
    if (t->peak < PEAK_FLOOR) {
        t->peak = PEAK_FLOOR;
    }
}

static void sample_rate(telemetry_t *t, const brain_last_step_t *last, uint32_t now_ms)
{
    const uint32_t dt = now_ms - t->rate_ms;
    if (dt >= RATE_WINDOW_MS) {
        const uint64_t steps = (uint64_t)(last->step_count - t->rate_steps);
        t->rate_x10_hz = (uint32_t)((steps * RATE_SCALE_X10) / dt);
        t->rate_steps = last->step_count;
        t->rate_ms = now_ms;
    }
}

static void render_header(sb_t *s, const telemetry_t *t, const brain_last_step_t *last,
                          uint32_t now_ms, uint32_t core_khz, const char *link)
{
    sb_str(s, ANSI_BOLD ANSI_CYAN " FLY BRAIN " ANSI_RESET ANSI_DIM
                        "connectome rate model on STM32H753ZI @ ");
    sb_u32(s, core_khz / KHZ_PER_MHZ);
    sb_str(s, " MHz" ANSI_RESET EOL EOL);

    const uint32_t up_s = now_ms / MS_PER_S;
    sb_label(s, "master");
    sb_str(s, link);
    sb_str(s, "   steps ");
    sb_u32(s, last->step_count);
    sb_str(s, "   rate ");
    sb_u32(s, t->rate_x10_hz / DECIMAL_BASE);
    sb_char(s, '.');
    sb_u32(s, t->rate_x10_hz % DECIMAL_BASE);
    sb_str(s, " Hz   up ");
    sb_u32_2d(s, up_s / S_PER_HOUR);
    sb_char(s, ':');
    sb_u32_2d(s, (up_s / S_PER_MIN) % S_PER_MIN);
    sb_char(s, ':');
    sb_u32_2d(s, up_s % S_PER_MIN);
    sb_str(s, EOL);

    const uint32_t us = (core_khz == 0U)
                            ? 0U
                            : (uint32_t)(((uint64_t)last->resp.step_cycles * US_PER_MS) / core_khz);
    const uint32_t tenths = us / TENTHS_MS_PER_US;
    const bool learning = (last->flags & BRAIN_STEP_FLAG_LEARN) != 0U;
    sb_label(s, "model ");
    sb_str(s, "step ");
    sb_u32(s, tenths / DECIMAL_BASE);
    sb_char(s, '.');
    sb_u32(s, tenths % DECIMAL_BASE);
    sb_str(s, " ms   learning ");
    sb_str(s, learning ? ANSI_GREEN "ON" ANSI_RESET : ANSI_DIM "off" ANSI_RESET);
    sb_str(s, "   weight drift ");
    sb_fixed(s, last->resp.weight_drift, 3U);
    sb_str(s, "   errors ");
    sb_u32(s, last->step_errors);
    sb_str(s, EOL EOL);
}

static void render_action(sb_t *s, uint8_t actions)
{
    sb_label(s, "action ");
    if ((actions & (uint8_t)BRAIN_ACTION_TURN_LEFT) != 0U) {
        sb_str(s, ANSI_BOLD ANSI_YELLOW GLYPH_LEFT " TURN LEFT " ANSI_RESET);
    } else if ((actions & (uint8_t)BRAIN_ACTION_TURN_RIGHT) != 0U) {
        sb_str(s, ANSI_BOLD ANSI_YELLOW "TURN RIGHT " GLYPH_RIGHT ANSI_RESET);
    } else {
        sb_str(s, ANSI_DIM "no turn (the game decides forward and fire)" ANSI_RESET);
    }
    sb_str(s, EOL);
}

static void render_history(sb_t *s, const telemetry_t *t)
{
    sb_label(s, "history");
    sb_str(s, ANSI_DIM " last ");
    sb_u32(s, TELEMETRY_HISTORY_STEPS);
    sb_str(s, " steps: " ANSI_RESET);
    for (uint32_t i = 0U; i < TELEMETRY_HISTORY_STEPS; i++) {
        if (t->history[i] == HIST_LEFT) {
            sb_str(s, ANSI_YELLOW GLYPH_LEFT ANSI_RESET);
        } else if (t->history[i] == HIST_RIGHT) {
            sb_str(s, ANSI_YELLOW GLYPH_RIGHT ANSI_RESET);
        } else {
            sb_str(s, ANSI_DIM GLYPH_STRAIGHT ANSI_RESET);
        }
    }
    sb_str(s, EOL);
}

size_t telemetry_render(telemetry_t *t, const brain_last_step_t *last, uint32_t now_ms,
                        uint32_t core_khz, const char *link, char *buf, size_t cap)
{
    if ((t == NULL) || (last == NULL) || (link == NULL) || (buf == NULL) || (cap == 0U)) {
        return 0U;
    }
    sample_rate(t, last, now_ms);
    sb_t s = {buf, cap, 0U, false};
    if (!t->cleared) {
        sb_str(&s, ANSI_CLEAR_HIDE_CURSOR);
        t->cleared = true;
    }
    sb_str(&s, ANSI_HOME);
    render_header(&s, t, last, now_ms, core_khz, link);

    sb_section(&s, "inputs from the game (0..1)");
    sb_pair(&s, "light ", last->features[BRAIN_FEAT_LUM_LEFT], last->features[BRAIN_FEAT_LUM_RIGHT],
            1.0f, ANSI_CYAN);
    sb_pair(&s, "enemy ", last->features[BRAIN_FEAT_ENEMY_LEFT],
            last->features[BRAIN_FEAT_ENEMY_RIGHT], 1.0f, ANSI_YELLOW);
    sb_pair(&s, "damage", last->features[BRAIN_FEAT_DAMAGE_LEFT],
            last->features[BRAIN_FEAT_DAMAGE_RIGHT], 1.0f, ANSI_RED);
    sb_label(&s, "reward");
    sb_str(&s, " ");
    sb_signed_bar(&s, last->reward);
    sb_char(&s, ' ');
    sb_fixed(&s, last->reward, 2U);
    sb_str(&s, EOL EOL);

    sb_section(&s, "motor neurons (bars: share of recent peak)");
    sb_pair(&s, "steer ", last->resp.steer_left, last->resp.steer_right, t->peak, ANSI_GREEN);
    sb_label(&s, "walk  ");
    sb_str(&s, " F ");
    sb_bar(&s, last->resp.forward / t->peak, ANSI_MAGENTA);
    sb_str(&s, "        B ");
    sb_bar(&s, last->resp.backward / t->peak, ANSI_MAGENTA);
    sb_str(&s, EOL EOL);

    render_action(&s, last->resp.actions);
    render_history(&s, t);
    sb_str(&s, ANSI_CLEAR_BELOW);
    if (s.overflow) {
        return 0U;
    }
    return s.len;
}
