/**
 * @file test_bl_features.c
 * @brief Feature extraction vs the Python reference (golden vectors from game/features.py) and
 *        argument checks.
 *
 * Usage: test_bl_features <bl_feature_vectors.txt>
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bl_features.h"
#include "test_runner.h"

#define LINE_BYTES 1024U
#define MAX_PIXELS 8192U
#define MAX_RECTS 8U
#define TOLERANCE 2.0e-6F
#define ID_COUNT 256U

static const char *g_golden_path;

typedef struct {
    int id, cls, x0, x1, y0, y1;
} rect_t;

static void build(int w, int h, int a, int b, const rect_t *rects, int n, uint8_t *gray,
                  uint8_t *ids, uint8_t *cls)
{
    memset(ids, 0, (size_t)(w * h));
    memset(cls, 0, ID_COUNT);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            gray[(y * w) + x] = (uint8_t)(((x * a) + (y * b)) % 256);
        }
    }
    for (int i = 0; i < n; i++) {
        cls[rects[i].id] = (uint8_t)rects[i].cls;
        for (int y = rects[i].y0; y <= rects[i].y1; y++) {
            for (int x = rects[i].x0; x <= rects[i].x1; x++) {
                ids[(y * w) + x] = (uint8_t)rects[i].id;
            }
        }
    }
}

static int near(float got, float want)
{
    return fabsf(got - want) <= TOLERANCE;
}

/** Parse "CASE w h a b n r... | f0 f1 f2 f3 vis offset" and check one case. */
static void check_case(char *line, unsigned lineno)
{
    static uint8_t gray[MAX_PIXELS], ids[MAX_PIXELS], cls[ID_COUNT];
    rect_t rects[MAX_RECTS];
    char *tok = strtok(line, " \n"); /* CASE */
    int v[5];
    for (int i = 0; i < 5; i++) {
        tok = strtok(NULL, " \n");
        CHECK(tok != NULL);
        if (tok == NULL) {
            return;
        }
        v[i] = atoi(tok);
    }
    const int w = v[0], h = v[1], a = v[2], b = v[3], n = v[4];
    CHECK(n <= (int)MAX_RECTS);
    for (int i = 0; (i < n) && (i < (int)MAX_RECTS); i++) {
        int f[6];
        for (int k = 0; k < 6; k++) {
            tok = strtok(NULL, " \n");
            f[k] = (tok != NULL) ? atoi(tok) : -1;
        }
        rects[i] = (rect_t){f[0], f[1], f[2], f[3], f[4], f[5]};
    }
    tok = strtok(NULL, " \n");
    CHECK((tok != NULL) && (tok[0] == '|'));
    float want[BL_N_FEATURES + 2U]; /* features, visible, offset */
    for (unsigned k = 0U; k < (BL_N_FEATURES + 2U); k++) {
        tok = strtok(NULL, " \n");
        CHECK(tok != NULL);
        want[k] = (tok != NULL) ? strtof(tok, NULL) : 0.0F;
    }
    build(w, h, a, b, rects, n, gray, ids, cls);
    bl_features_t got;
    CHECK_EQ_INT(bl_features_make(gray, ids, cls, (uint16_t)w, (uint16_t)h, &got), BL_OK);
    for (unsigned k = 0U; k < BL_N_FEATURES; k++) {
        if (!near(got.feature[k], want[k])) {
            printf("line %u feature %u: C %.9g vs Python %.9g\n", lineno, k, (double)got.feature[k],
                   (double)want[k]);
        }
        CHECK(near(got.feature[k], want[k]));
    }
    CHECK_EQ_INT(got.enemy_visible ? 1 : 0, (int)want[BL_N_FEATURES]);
    CHECK(near(got.enemy_offset, want[BL_N_FEATURES + 1U]));
}

static void test_golden_vectors(void)
{
    FILE *fp = fopen(g_golden_path, "r");
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    char line[LINE_BYTES];
    unsigned lineno = 0U, cases = 0U;
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        lineno++;
        if (strncmp(line, "CASE ", 5U) == 0) {
            check_case(line, lineno);
            cases++;
        }
    }
    (void)fclose(fp);
    CHECK(cases >= 10U);
}

static void test_bad_arguments(void)
{
    uint8_t gray[4] = {0}, ids[4] = {0}, cls[ID_COUNT] = {0};
    bl_features_t out;
    CHECK_EQ_INT(bl_features_make(NULL, ids, cls, 2U, 2U, &out), BL_ERR_NULL);
    CHECK_EQ_INT(bl_features_make(gray, NULL, cls, 2U, 2U, &out), BL_ERR_NULL);
    CHECK_EQ_INT(bl_features_make(gray, ids, NULL, 2U, 2U, &out), BL_ERR_NULL);
    CHECK_EQ_INT(bl_features_make(gray, ids, cls, 2U, 2U, NULL), BL_ERR_NULL);
    CHECK_EQ_INT(bl_features_make(gray, ids, cls, 1U, 2U, &out), BL_ERR_SIZE);
    CHECK_EQ_INT(bl_features_make(gray, ids, cls, 2U, 0U, &out), BL_ERR_SIZE);
}

static void test_all_white_and_black(void)
{
    uint8_t gray[16], ids[16] = {0}, cls[ID_COUNT] = {0};
    bl_features_t out;
    memset(gray, 255, sizeof gray);
    CHECK_EQ_INT(bl_features_make(gray, ids, cls, 4U, 4U, &out), BL_OK);
    CHECK(near(out.feature[BL_FEAT_LUM_LEFT], 1.0F) && near(out.feature[BL_FEAT_LUM_RIGHT], 1.0F));
    memset(gray, 0, sizeof gray);
    CHECK_EQ_INT(bl_features_make(gray, ids, cls, 4U, 4U, &out), BL_OK);
    CHECK(near(out.feature[BL_FEAT_LUM_LEFT], 0.0F) && !out.enemy_visible);
}

#define WALL_W 16U
#define WALL_H 8U

/* Fill columns [x0, x1) of every row with coverage @p v. */
static void wall_cols(uint8_t *cov, uint32_t x0, uint32_t x1, uint8_t v)
{
    for (uint32_t y = 0U; y < WALL_H; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            cov[(y * WALL_W) + x] = v;
        }
    }
}

static void test_walls(void)
{
    uint8_t cov[WALL_W * WALL_H];
    bl_features_t out = {0};
    out.enemy_visible = true; /* must be left alone */

    memset(cov, 0, sizeof cov);
    CHECK_EQ_INT(bl_walls_make(cov, WALL_W, WALL_H, &out), BL_OK);
    CHECK(near(out.wall_centre, 0.0F) && !out.wall_ahead && out.enemy_visible);

    /* Wall in the centre band (columns 6..9), more wall on the right: open to the left. */
    wall_cols(cov, 6U, 10U, 255U);
    wall_cols(cov, 12U, 16U, 255U);
    CHECK_EQ_INT(bl_walls_make(cov, WALL_W, WALL_H, &out), BL_OK);
    CHECK(near(out.wall_centre, 1.0F) && out.wall_ahead && out.open_left);

    /* Mirror: more wall on the left, open to the right. */
    memset(cov, 0, sizeof cov);
    wall_cols(cov, 6U, 10U, 255U);
    wall_cols(cov, 0U, 4U, 255U);
    CHECK_EQ_INT(bl_walls_make(cov, WALL_W, WALL_H, &out), BL_OK);
    CHECK(out.wall_ahead && !out.open_left);

    /* Half covered centre is below the threshold. */
    memset(cov, 0, sizeof cov);
    wall_cols(cov, 6U, 10U, 128U);
    CHECK_EQ_INT(bl_walls_make(cov, WALL_W, WALL_H, &out), BL_OK);
    CHECK((out.wall_centre < BL_WALL_AHEAD_FRAC) && !out.wall_ahead);

    CHECK_EQ_INT(bl_walls_make(NULL, WALL_W, WALL_H, &out), BL_ERR_NULL);
    CHECK_EQ_INT(bl_walls_make(cov, WALL_W, WALL_H, NULL), BL_ERR_NULL);
    CHECK_EQ_INT(bl_walls_make(cov, 7U, WALL_H, &out), BL_ERR_SIZE);
    CHECK_EQ_INT(bl_walls_make(cov, WALL_W, 0U, &out), BL_ERR_SIZE);
}

/** Degrees (positive = left) to a wrapped Doom angle, as R_PointToAngle2 - mo->angle gives. */
static uint32_t bam_from_deg(double deg)
{
    const double turns = deg / 360.0;
    const double wrapped = turns - (double)(long long)turns + ((turns < 0.0) ? 1.0 : 0.0);
    return (uint32_t)(wrapped * 4294967296.0);
}

static void test_damage(void)
{
    float l = -1.0F, r = -1.0F;
    CHECK_EQ_INT(bl_damage_make(0U, 10, NULL, &r), BL_ERR_NULL);
    CHECK_EQ_INT(bl_damage_make(0U, 10, &l, NULL), BL_ERR_NULL);
    /* not in pain: nothing, whatever the angle */
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(45.0), 0, &l, &r), BL_OK);
    CHECK(near(l, 0.0F) && near(r, 0.0F));
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(45.0), -3, &l, &r), BL_OK);
    CHECK(near(l, 0.0F) && near(r, 0.0F));
    /* straight ahead: no side */
    CHECK_EQ_INT(bl_damage_make(0U, 25, &l, &r), BL_OK);
    CHECK(near(l, 0.0F) && near(r, 0.0F));
    /* 45 degrees left, full pain: left 0.5 */
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(45.0), 25, &l, &r), BL_OK);
    CHECK(near(l, 0.5F) && near(r, 0.0F));
    /* the wrap: 350 degrees is 10 degrees to the RIGHT; 10 degrees is left */
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(350.0), 25, &l, &r), BL_OK);
    CHECK(near(l, 0.0F) && near(r, 10.0F / 90.0F));
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(10.0), 25, &l, &r), BL_OK);
    CHECK(near(l, 10.0F / 90.0F) && near(r, 0.0F));
    /* side and behind count fully; exactly behind (2^31) counts as right */
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(90.0), 25, &l, &r), BL_OK);
    CHECK(near(l, 1.0F) && near(r, 0.0F));
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(150.0), 25, &l, &r), BL_OK);
    CHECK(near(l, 1.0F) && near(r, 0.0F));
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(-120.0), 25, &l, &r), BL_OK);
    CHECK(near(l, 0.0F) && near(r, 1.0F));
    CHECK_EQ_INT(bl_damage_make(0x80000000UL, 25, &l, &r), BL_OK);
    CHECK(near(l, 0.0F) && near(r, 1.0F));
    CHECK_EQ_INT(bl_damage_make(0xFFFFFFFFUL, 25, &l, &r), BL_OK); /* a hair right of centre */
    CHECK(near(l, 0.0F) && near(r, 0.0F));
    /* pain scales and saturates: 10 points = 0.4, 100 points = 1 */
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(90.0), 10, &l, &r), BL_OK);
    CHECK(near(l, 0.4F) && near(r, 0.0F));
    CHECK_EQ_INT(bl_damage_make(bam_from_deg(-90.0), 100, &l, &r), BL_OK);
    CHECK(near(l, 0.0F) && near(r, 1.0F));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: test_bl_features <bl_feature_vectors.txt>\n");
        return 2;
    }
    g_golden_path = argv[1];
    RUN(test_golden_vectors);
    RUN(test_bad_arguments);
    RUN(test_all_white_and_black);
    RUN(test_walls);
    RUN(test_damage);
    return TEST_REPORT();
}
