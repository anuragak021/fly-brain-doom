/**
 * @file test_rate_model.c
 * @brief Host test: C rate model vs Python reference (golden CSV) plus API checks.
 *
 * Usage: test_rate_model <golden.csv>
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rate_model.h"

#define LINE_BYTES 256U
#define GOLDEN_COLUMNS 10
#define ABS_TOL 1.0e-5F
#define REL_TOL 1.0e-4F

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static int close_enough(float got, float want)
{
    return fabsf(got - want) <= (ABS_TOL + (REL_TOL * fabsf(want)));
}

static void test_api_errors(void)
{
    static rm_state_t st;
    static float drive[MODEL_N_NEURONS];
    const float feat[RM_N_FEATURES] = {0.0F, 0.0F, 0.0F, 0.0F};
    const float bad[RM_N_FEATURES] = {0.0F, NAN, 0.0F, 0.0F};
    rm_outputs_t out;

    CHECK(rm_reset(NULL) == RM_ERR_NULL);
    CHECK(rm_make_drive(NULL, drive) == RM_ERR_NULL);
    CHECK(rm_make_drive(feat, NULL) == RM_ERR_NULL);
    CHECK(rm_make_drive(bad, drive) == RM_ERR_RANGE);
    CHECK(rm_step(NULL, drive, 1U) == RM_ERR_NULL);
    CHECK(rm_reset(&st) == RM_OK);
    CHECK(rm_step(&st, drive, 0U) == RM_ERR_RANGE);
    CHECK(rm_step(&st, drive, RM_MAX_SUBSTEPS + 1U) == RM_ERR_RANGE);
    CHECK(rm_outputs(NULL, &out) == RM_ERR_NULL);
    CHECK(rm_outputs(&st, NULL) == RM_ERR_NULL);
}

static void test_zero_state_stays_zero_without_drive(void)
{
    static rm_state_t st;
    static float drive[MODEL_N_NEURONS];
    rm_outputs_t out;

    CHECK(rm_reset(&st) == RM_OK);
    CHECK(rm_step(&st, drive, MODEL_SUBSTEPS_PER_FRAME) == RM_OK);
    CHECK(rm_outputs(&st, &out) == RM_OK);
    CHECK(out.steer_left == 0.0F);
    CHECK(out.forward == 0.0F);
}

static void test_golden(const char *path)
{
    static rm_state_t st;
    static float drive[MODEL_N_NEURONS];
    char line[LINE_BYTES];
    uint32_t frame = 0U;
    FILE *fp = fopen(path, "r");

    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    CHECK(rm_reset(&st) == RM_OK);
    CHECK(fgets(line, (int)sizeof(line), fp) != NULL); /* header */
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        float v[GOLDEN_COLUMNS];
        bool turn_left;
        bool turn_right;
        rm_outputs_t out;
        const int n = sscanf(line, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3],
                             &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
        CHECK(n == GOLDEN_COLUMNS);
        CHECK(rm_make_drive(v, drive) == RM_OK);
        CHECK(rm_step(&st, drive, MODEL_SUBSTEPS_PER_FRAME) == RM_OK);
        CHECK(rm_outputs(&st, &out) == RM_OK);
        CHECK(rm_decode_turn(&out, &turn_left, &turn_right) == RM_OK);
        CHECK(turn_left == (v[8] > 0.5F));
        CHECK(turn_right == (v[9] > 0.5F));
        if (!close_enough(out.steer_left, v[4]) || !close_enough(out.steer_right, v[5]) ||
            !close_enough(out.forward, v[6]) || !close_enough(out.backward, v[7])) {
            (void)printf("frame %u mismatch: C %.7g %.7g %.7g %.7g vs Python %.7g %.7g %.7g %.7g\n",
                         (unsigned)frame, (double)out.steer_left, (double)out.steer_right,
                         (double)out.forward, (double)out.backward, (double)v[4], (double)v[5],
                         (double)v[6], (double)v[7]);
            g_failures++;
        }
        frame++;
    }
    CHECK(frame > 0U);
    (void)printf("golden frames compared: %u\n", (unsigned)frame);
    (void)fclose(fp);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)printf("usage: %s <golden.csv>\n", argv[0]);
        return 2;
    }
    test_api_errors();
    test_zero_state_stays_zero_without_drive();
    test_golden(argv[1]);
    if (g_failures != 0) {
        (void)printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    (void)printf("all checks passed\n");
    return 0;
}
