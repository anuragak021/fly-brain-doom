/** @file test_hud.c @brief Tests for the HUD line (separator, digits, icons, clamping). */
#include <stdint.h>
#include <string.h>

#include "op_hud.h"
#include "op_scale.h"
#include "test_runner.h"

#define W (OP_OLED_WIDTH_PX)
#define H (OP_OLED_HEIGHT_PX)
#define HUD_TOP (H - OP_HUD_ROWS_PX)
#define GLYPH_TOP (HUD_TOP + 2U)
#define SENTINEL (2U)
#define FIELD_X0 (2U)
#define DIGITS_X_OFFSET (7U) /* icon 5 + gap 2 */
#define DIGIT_PITCH (4U)

static uint8_t g_bits[W * H];

/* Expected glyphs, written out independently of the module's table. */
static const char *const DIGIT_0[5] = {"###", "#.#", "#.#", "#.#", "###"};
static const char *const DIGIT_1[5] = {".#.", "##.", ".#.", ".#.", "###"};
static const char *const DIGIT_8[5] = {"###", "#.#", "###", "#.#", "###"};
static const char *const DIGIT_9[5] = {"###", "#.#", "###", "..#", "###"};
static const char *const BLANK[5] = {"...", "...", "...", "...", "..."};

static uint8_t px(uint32_t x, uint32_t y)
{
    return g_bits[(y * W) + x];
}

/** True if digit place @p place (0 = hundreds) of field @p field shows @p glyph. */
static int digit_is(uint32_t field, uint32_t place, const char *const glyph[5])
{
    const uint32_t x0 =
        FIELD_X0 + (field * OP_HUD_FIELD_PITCH_PX) + DIGITS_X_OFFSET + (place * DIGIT_PITCH);
    for (uint32_t r = 0U; r < 5U; r++) {
        for (uint32_t c = 0U; c < 3U; c++) {
            const uint8_t expect = (glyph[r][c] == '#') ? 1U : 0U;
            if (px(x0 + c, GLYPH_TOP + r) != expect) {
                return 0;
            }
        }
    }
    return 1;
}

static void draw(int32_t health, int32_t kills, int32_t ammo, bool show_ammo)
{
    const op_hud_t hud = {health, kills, ammo, show_ammo};
    memset(g_bits, SENTINEL, sizeof g_bits);
    CHECK_EQ_INT(op_hud_draw(&hud, W, H, g_bits), OP_OK);
}

static void test_layout_and_view_untouched(void)
{
    draw(100, 0, 50, true);
    for (uint32_t i = 0U; i < (HUD_TOP * W); i++) {
        CHECK_EQ_INT(g_bits[i], SENTINEL);
    }
    for (uint32_t x = 0U; x < W; x++) {
        CHECK_EQ_INT(px(x, HUD_TOP), 1U);      /* separator */
        CHECK_EQ_INT(px(x, HUD_TOP + 1U), 0U); /* blank */
        CHECK_EQ_INT(px(x, H - 1U), 0U);       /* blank */
    }
    for (uint32_t i = HUD_TOP * W; i < (W * H); i++) {
        CHECK(g_bits[i] <= 1U);
    }
}

static void test_values_right_aligned(void)
{
    draw(100, 8, 19, true);
    CHECK(digit_is(0U, 0U, DIGIT_1) && digit_is(0U, 1U, DIGIT_0) && digit_is(0U, 2U, DIGIT_0));
    CHECK(digit_is(1U, 0U, BLANK) && digit_is(1U, 1U, BLANK) && digit_is(1U, 2U, DIGIT_8));
    CHECK(digit_is(2U, 0U, BLANK) && digit_is(2U, 1U, DIGIT_1) && digit_is(2U, 2U, DIGIT_9));
}

static void test_values_clamped(void)
{
    draw(-40, 1234, 0, true);
    CHECK(digit_is(0U, 0U, BLANK) && digit_is(0U, 1U, BLANK) && digit_is(0U, 2U, DIGIT_0));
    CHECK(digit_is(1U, 0U, DIGIT_9) && digit_is(1U, 1U, DIGIT_9) && digit_is(1U, 2U, DIGIT_9));
    CHECK(digit_is(2U, 2U, DIGIT_0));
}

/* The kills icon is a skull; written out independently of the module's table. */
static void test_kills_icon_is_a_skull(void)
{
    static const char *const SKULL[5] = {".###.", "#.#.#", ".###.", ".###.", ".#.#."};
    draw(1, 2, 3, true);
    const uint32_t x0 = FIELD_X0 + OP_HUD_FIELD_PITCH_PX;
    for (uint32_t r = 0U; r < 5U; r++) {
        for (uint32_t c = 0U; c < 5U; c++) {
            const uint8_t expect = (SKULL[r][c] == '#') ? 1U : 0U;
            CHECK_EQ_INT(px(x0 + c, GLYPH_TOP + r), expect);
        }
    }
}

static void test_ammo_hidden(void)
{
    draw(100, 0, 50, false);
    const uint32_t x0 = FIELD_X0 + (2U * OP_HUD_FIELD_PITCH_PX);
    uint32_t lit = 0U;
    for (uint32_t y = GLYPH_TOP; y < (GLYPH_TOP + 5U); y++) {
        for (uint32_t x = x0; x < W; x++) {
            lit += px(x, y);
        }
    }
    CHECK_EQ_INT(lit, 0U);
}

static void test_rejects_bad_args(void)
{
    const op_hud_t hud = {1, 2, 3, true};
    CHECK_EQ_INT(op_hud_draw(NULL, W, H, g_bits), OP_ERR_NULL);
    CHECK_EQ_INT(op_hud_draw(&hud, W, H, NULL), OP_ERR_NULL);
    CHECK_EQ_INT(op_hud_draw(&hud, W - 1U, H, g_bits), OP_ERR_SIZE);
    CHECK_EQ_INT(op_hud_draw(&hud, W, OP_HUD_ROWS_PX - 1U, g_bits), OP_ERR_SIZE);
}

int main(void)
{
    RUN(test_layout_and_view_untouched);
    RUN(test_values_right_aligned);
    RUN(test_values_clamped);
    RUN(test_kills_icon_is_a_skull);
    RUN(test_ammo_hidden);
    RUN(test_rejects_bad_args);
    return TEST_REPORT();
}
