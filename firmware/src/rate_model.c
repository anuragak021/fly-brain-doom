/**
 * @file rate_model.c
 * @brief Leaky rate model on a CSR connectome subgraph. See rate_model.h.
 */
#include "rate_model.h"

#include <math.h>
#include <stddef.h>

static float mean_of(const float *r, const uint16_t *idx, uint32_t count)
{
    float sum = 0.0F;
    if (count == 0U) {
        return 0.0F;
    }
    for (uint32_t i = 0U; i < count; i++) {
        sum += r[idx[i]];
    }
    return sum / (float)count;
}

static void set_drive(float *drive, const uint16_t *idx, uint32_t count, float value)
{
    for (uint32_t i = 0U; i < count; i++) {
        drive[idx[i]] = value;
    }
}

rm_status_t rm_reset(rm_state_t *st)
{
    if (st == NULL) {
        return RM_ERR_NULL;
    }
    for (uint32_t i = 0U; i < MODEL_N_NEURONS; i++) {
        st->r[i] = 0.0F;
        st->next[i] = 0.0F;
    }
    return RM_OK;
}

rm_status_t rm_make_drive(const float *features, float *drive)
{
    if ((features == NULL) || (drive == NULL)) {
        return RM_ERR_NULL;
    }
    for (uint32_t i = 0U; i < RM_N_FEATURES; i++) {
        if (isfinite(features[i]) == 0) {
            return RM_ERR_RANGE;
        }
    }
    for (uint32_t i = 0U; i < MODEL_N_NEURONS; i++) {
        drive[i] = MODEL_TONIC_DRIVE;
    }
    const float left =
        (features[RM_FEAT_ENEMY_LEFT] + (MODEL_LUM_WEIGHT * features[RM_FEAT_LUM_LEFT])) +
        (MODEL_DAMAGE_WEIGHT * features[RM_FEAT_DAMAGE_LEFT]);
    const float right =
        (features[RM_FEAT_ENEMY_RIGHT] + (MODEL_LUM_WEIGHT * features[RM_FEAT_LUM_RIGHT])) +
        (MODEL_DAMAGE_WEIGHT * features[RM_FEAT_DAMAGE_RIGHT]);
    set_drive(drive, model_input_left, MODEL_N_INPUT_LEFT, MODEL_INPUT_GAIN * left);
    set_drive(drive, model_input_right, MODEL_N_INPUT_RIGHT, MODEL_INPUT_GAIN * right);
    return RM_OK;
}

rm_status_t rm_step(rm_state_t *st, const float *values, const float *drive, uint32_t substeps)
{
    if ((st == NULL) || (values == NULL) || (drive == NULL)) {
        return RM_ERR_NULL;
    }
    if ((substeps == 0U) || (substeps > RM_MAX_SUBSTEPS)) {
        return RM_ERR_RANGE;
    }
    for (uint32_t s = 0U; s < substeps; s++) {
        for (uint32_t row = 0U; row < MODEL_N_NEURONS; row++) {
            float acc = drive[row];
            for (uint32_t k = model_row_ptr[row]; k < model_row_ptr[row + 1U]; k++) {
                acc += values[k] * st->r[model_col_idx[k]];
            }
            const float target = (acc > 0.0F) ? acc : 0.0F;
            st->next[row] = st->r[row] + (MODEL_ALPHA * (target - st->r[row]));
        }
        for (uint32_t i = 0U; i < MODEL_N_NEURONS; i++) {
            st->r[i] = st->next[i];
        }
    }
    return RM_OK;
}

rm_status_t rm_outputs(const rm_state_t *st, rm_outputs_t *out)
{
    if ((st == NULL) || (out == NULL)) {
        return RM_ERR_NULL;
    }
    out->steer_left = mean_of(st->r, model_steer_left, MODEL_N_STEER_LEFT);
    out->steer_right = mean_of(st->r, model_steer_right, MODEL_N_STEER_RIGHT);
    out->forward = mean_of(st->r, model_forward, MODEL_N_FORWARD);
    out->backward = mean_of(st->r, model_backward, MODEL_N_BACKWARD);
    return RM_OK;
}

rm_status_t rm_decode_turn(const rm_outputs_t *out, bool *turn_left, bool *turn_right)
{
    if ((out == NULL) || (turn_left == NULL) || (turn_right == NULL)) {
        return RM_ERR_NULL;
    }
    const float diff =
        (((out->steer_right - MODEL_REST_STEER_RIGHT) - (out->steer_left - MODEL_REST_STEER_LEFT)) *
         MODEL_TURN_SIGN);
    *turn_left = (diff > MODEL_TURN_DEADZONE);
    *turn_right = (diff < -MODEL_TURN_DEADZONE);
    return RM_OK;
}
