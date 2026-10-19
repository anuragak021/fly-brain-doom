/**
 * @file test_plasticity.c
 * @brief Host test: C plasticity vs Python reference (learn_golden.csv) plus API checks.
 *
 * Usage: test_plasticity <learn_golden.csv>
 *
 * Each golden row is one frame: the model steps on the row's features with the live
 * (learning) weights, turns are decoded against the moving references, then the rule runs
 * with the row's reward. Outputs and drift accumulate any float difference between numpy and
 * C over the whole run, so the tolerance is looser than the rate model's.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "plasticity.h"
#include "rate_model.h"

#define LINE_BYTES 512U
#define GOLDEN_COLUMNS 14
#define COL_REWARD 6
#define COL_STEER_LEFT 7
#define COL_STEER_RIGHT 8
#define COL_FORWARD 9
#define COL_BACKWARD 10
#define COL_TURN_LEFT 11
#define COL_TURN_RIGHT 12
#define COL_DRIFT 13
#define ABS_TOL 1.0e-4F
#define REL_TOL 1.0e-3F
/** A turn bit may differ only when the decoded difference sits this close to the deadzone. */
#define DEADZONE_MARGIN 1.0e-4F
#define EXPECTED_LEARNABLE 320U
#define API_STEPS 50U

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static bool close_enough(float got, float want)
{
    return fabsf(got - want) <= (ABS_TOL + (REL_TOL * fabsf(want)));
}

static float turn_diff(const pl_state_t *pl, const rm_outputs_t *out)
{
    return ((out->steer_right - pl->ref_right) - (out->steer_left - pl->ref_left)) *
           MODEL_TURN_SIGN;
}

static void test_api(void)
{
    static pl_state_t pl;
    static rm_state_t st;
    static float drive[MODEL_N_NEURONS];
    const float feat[RM_N_FEATURES] = {0.3F, 0.6F, 0.0F, 0.8F, 0.5F, 0.0F};
    rm_outputs_t out;
    bool tl = false;
    bool tr = false;

    CHECK(pl_init(NULL) == PL_ERR_NULL);
    CHECK(pl_reset_weights(NULL) == PL_ERR_NULL);
    CHECK(pl_decode_turn(NULL, &out, &tl, &tr) == PL_ERR_NULL);
    CHECK(pl_update(NULL, st.r, &out, 0.0F) == PL_ERR_NULL);
    CHECK(pl_drift(NULL) == 0.0F);

    CHECK(pl_init(&pl) == PL_OK);
    CHECK(pl.n == EXPECTED_LEARNABLE);
    CHECK(pl_drift(&pl) == 0.0F);
    CHECK(pl_update(&pl, NULL, &out, 0.0F) == PL_ERR_NULL);
    CHECK(pl_update(&pl, st.r, NULL, 0.0F) == PL_ERR_NULL);
    CHECK(pl_decode_turn(&pl, NULL, &tl, &tr) == PL_ERR_NULL);

    CHECK(rm_reset(&st) == RM_OK);
    CHECK(rm_make_drive(feat, drive) == RM_OK);
    for (uint32_t i = 0U; i < API_STEPS; i++) {
        CHECK(rm_step(&st, pl.w, drive, MODEL_SUBSTEPS_PER_FRAME) == RM_OK);
        CHECK(rm_outputs(&st, &out) == RM_OK);
        CHECK(pl_update(&pl, st.r, &out, ((i % 5U) == 0U) ? 1.0F : 0.0F) == PL_OK);
    }
    CHECK(pl_drift(&pl) > 0.0F);

    /* Only the steering rows change, every weight keeps its sign, |w| <= 2 |w0|. */
    static bool learns[MODEL_NNZ];
    (void)memset(learns, 0, sizeof(learns));
    for (uint32_t e = 0U; e < pl.n; e++) {
        learns[pl.entry[e]] = true;
    }
    for (uint32_t k = 0U; k < MODEL_NNZ; k++) {
        const float w0 = model_values[k];
        if (learns[k]) {
            CHECK((pl.w[k] * w0) >= 0.0F);
            CHECK(fabsf(pl.w[k]) <= (MODEL_WEIGHT_MAX_FACTOR * fabsf(w0)));
        } else {
            CHECK(pl.w[k] == w0);
        }
    }

    /* Synaptic scaling keeps each learning row's sum |w| at most the original. */
    for (uint32_t j = 0U; j < PL_N_ROWS; j++) {
        float total = 0.0F;
        for (uint32_t e = pl.row_begin[j]; e < pl.row_begin[j + 1U]; e++) {
            total += fabsf(pl.w[pl.entry[e]]);
        }
        CHECK(total <= (pl.row_abs0[j] * 1.0001F));
    }

    CHECK(pl_reset_weights(&pl) == PL_OK);
    CHECK(pl_drift(&pl) == 0.0F);
    CHECK((pl.ref_left == MODEL_REST_STEER_LEFT) && (pl.ref_right == MODEL_REST_STEER_RIGHT));
    for (uint32_t k = 0U; k < MODEL_NNZ; k++) {
        CHECK(pl.w[k] == model_values[k]);
    }
}

static bool parse_row(const char *line, float *v)
{
    const int n =
        sscanf(line, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13]);
    return n == GOLDEN_COLUMNS;
}

static void check_turns(uint32_t frame, float diff, bool tl, bool tr, const float *v)
{
    const bool want_l = v[COL_TURN_LEFT] > 0.5F;
    const bool want_r = v[COL_TURN_RIGHT] > 0.5F;
    if ((tl == want_l) && (tr == want_r)) {
        return;
    }
    if (fabsf(fabsf(diff) - MODEL_TURN_DEADZONE) <= DEADZONE_MARGIN) {
        (void)printf("frame %u: turn bit differs at the deadzone edge (diff %.7g), allowed\n",
                     (unsigned)frame, (double)diff);
        return;
    }
    (void)printf("frame %u turn mismatch: C %d %d vs Python %d %d (diff %.7g)\n", (unsigned)frame,
                 (int)tl, (int)tr, (int)want_l, (int)want_r, (double)diff);
    g_failures++;
}

static void test_golden(const char *path)
{
    static pl_state_t pl;
    static rm_state_t st;
    static float drive[MODEL_N_NEURONS];
    char line[LINE_BYTES];
    uint32_t frame = 0U;
    FILE *fp = fopen(path, "r");

    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    CHECK(pl_init(&pl) == PL_OK);
    CHECK(rm_reset(&st) == RM_OK);
    CHECK(fgets(line, (int)sizeof(line), fp) != NULL); /* header */
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        float v[GOLDEN_COLUMNS];
        rm_outputs_t out;
        bool tl = false;
        bool tr = false;
        CHECK(parse_row(line, v));
        CHECK(rm_make_drive(v, drive) == RM_OK);
        CHECK(rm_step(&st, pl.w, drive, MODEL_SUBSTEPS_PER_FRAME) == RM_OK);
        CHECK(rm_outputs(&st, &out) == RM_OK);
        CHECK(pl_decode_turn(&pl, &out, &tl, &tr) == PL_OK);
        check_turns(frame, turn_diff(&pl, &out), tl, tr, v);
        CHECK(pl_update(&pl, st.r, &out, v[COL_REWARD]) == PL_OK);
        const float drift = pl_drift(&pl);
        if (!close_enough(out.steer_left, v[COL_STEER_LEFT]) ||
            !close_enough(out.steer_right, v[COL_STEER_RIGHT]) ||
            !close_enough(out.forward, v[COL_FORWARD]) ||
            !close_enough(out.backward, v[COL_BACKWARD]) || !close_enough(drift, v[COL_DRIFT])) {
            (void)printf("frame %u mismatch: C %.7g %.7g %.7g %.7g drift %.7g vs Python %.7g %.7g "
                         "%.7g %.7g drift %.7g\n",
                         (unsigned)frame, (double)out.steer_left, (double)out.steer_right,
                         (double)out.forward, (double)out.backward, (double)drift,
                         (double)v[COL_STEER_LEFT], (double)v[COL_STEER_RIGHT],
                         (double)v[COL_FORWARD], (double)v[COL_BACKWARD], (double)v[COL_DRIFT]);
            g_failures++;
        }
        frame++;
    }
    CHECK(frame > 0U);
    CHECK(pl_drift(&pl) > 0.0F);
    (void)printf("learning frames compared: %u, final drift %.5f\n", (unsigned)frame,
                 (double)pl_drift(&pl));
    (void)fclose(fp);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)printf("usage: %s <learn_golden.csv>\n", argv[0]);
        return 2;
    }
    test_api();
    test_golden(argv[1]);
    if (g_failures != 0) {
        (void)printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    (void)printf("all checks passed\n");
    return 0;
}
