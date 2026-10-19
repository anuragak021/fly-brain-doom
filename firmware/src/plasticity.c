/**
 * @file plasticity.c
 * @brief Reward-modulated plasticity. See plasticity.h and model/plasticity.py.
 */
#include "plasticity.h"

#include <math.h>
#include <stddef.h>

_Static_assert(MODEL_NNZ <= 65535U, "CSR indices must fit uint16_t");

static float abs_sum_row(const pl_state_t *pl, uint32_t j)
{
    float total = 0.0F;
    for (uint32_t m = pl->row_begin[j]; m < pl->row_begin[j + 1U]; m++) {
        total += fabsf(pl->w[pl->entry[m]]);
    }
    return total;
}

static bool row_is_left(uint32_t j)
{
    return j < MODEL_N_STEER_LEFT;
}

pl_status_t pl_init(pl_state_t *pl)
{
    if (pl == NULL) {
        return PL_ERR_NULL;
    }
    uint32_t n = 0U;
    for (uint32_t j = 0U; j < PL_N_ROWS; j++) {
        const uint16_t neuron =
            row_is_left(j) ? model_steer_left[j] : model_steer_right[j - MODEL_N_STEER_LEFT];
        pl->row_neuron[j] = neuron;
        pl->row_begin[j] = (uint16_t)n;
        for (uint32_t k = model_row_ptr[neuron]; k < model_row_ptr[neuron + 1U]; k++) {
            if (n >= PL_MAX_LEARNABLE) {
                return PL_ERR_SIZE;
            }
            pl->entry[n] = (uint16_t)k;
            n++;
        }
    }
    pl->row_begin[PL_N_ROWS] = (uint16_t)n;
    pl->n = (uint16_t)n;
    for (uint32_t k = 0U; k < MODEL_NNZ; k++) {
        pl->w[k] = model_values[k];
    }
    for (uint32_t j = 0U; j < PL_N_ROWS; j++) {
        pl->row_abs0[j] = abs_sum_row(pl, j);
    }
    return pl_reset_weights(pl);
}

pl_status_t pl_reset_weights(pl_state_t *pl)
{
    if (pl == NULL) {
        return PL_ERR_NULL;
    }
    for (uint32_t m = 0U; m < pl->n; m++) {
        pl->w[pl->entry[m]] = model_values[pl->entry[m]];
        pl->trace[m] = 0.0F;
    }
    for (uint32_t j = 0U; j < PL_N_ROWS; j++) {
        pl->base[j] = row_is_left(j) ? MODEL_REST_STEER_LEFT : MODEL_REST_STEER_RIGHT;
    }
    pl->reward_mean = 0.0F;
    pl->ref_left = MODEL_REST_STEER_LEFT;
    pl->ref_right = MODEL_REST_STEER_RIGHT;
    return PL_OK;
}

pl_status_t pl_decode_turn(const pl_state_t *pl, const rm_outputs_t *out, bool *turn_left,
                           bool *turn_right)
{
    if ((pl == NULL) || (out == NULL) || (turn_left == NULL) || (turn_right == NULL)) {
        return PL_ERR_NULL;
    }
    const float diff =
        ((out->steer_right - pl->ref_right) - (out->steer_left - pl->ref_left)) * MODEL_TURN_SIGN;
    *turn_left = (diff > MODEL_TURN_DEADZONE);
    *turn_right = (diff < -MODEL_TURN_DEADZONE);
    return PL_OK;
}

static void update_traces(pl_state_t *pl, const float *r)
{
    for (uint32_t j = 0U; j < PL_N_ROWS; j++) {
        const float post_r = r[pl->row_neuron[j]];
        const float post = post_r - pl->base[j];
        for (uint32_t m = pl->row_begin[j]; m < pl->row_begin[j + 1U]; m++) {
            const float pre = r[model_col_idx[pl->entry[m]]];
            pl->trace[m] = (MODEL_TRACE_DECAY * pl->trace[m]) + (pre * post);
        }
        pl->base[j] = pl->base[j] + (MODEL_BASELINE_RATE * (post_r - pl->base[j]));
    }
}

static float bounded_weight(float w, float w0)
{
    const float mag0 = fabsf(w0);
    const float limit = MODEL_WEIGHT_MAX_FACTOR * mag0;
    float v = ((w * w0) < 0.0F) ? 0.0F : w; /* the sign belongs to the presynaptic neuron */
    v = (v < -limit) ? -limit : v;
    return (v > limit) ? limit : v;
}

static void update_weights(pl_state_t *pl, float surprise)
{
    const float gain = MODEL_LEARN_RATE * surprise;
    for (uint32_t j = 0U; j < PL_N_ROWS; j++) {
        for (uint32_t m = pl->row_begin[j]; m < pl->row_begin[j + 1U]; m++) {
            const uint16_t k = pl->entry[m];
            const float w0 = model_values[k];
            float w = pl->w[k] + ((gain * pl->trace[m]) * fabsf(w0));
            w = w + (MODEL_FORGET_RATE * (w0 - w));
            pl->w[k] = bounded_weight(w, w0);
        }
        const float total = abs_sum_row(pl, j);
        if (total > 0.0F) {
            /* Synaptic scaling keeps the row sum at most the original (stability); clipping
             * again afterwards keeps the cap exact. */
            const float scale = pl->row_abs0[j] / total;
            for (uint32_t m = pl->row_begin[j]; m < pl->row_begin[j + 1U]; m++) {
                const uint16_t k = pl->entry[m];
                pl->w[k] = bounded_weight(pl->w[k] * scale, model_values[k]);
            }
        }
    }
}

pl_status_t pl_update(pl_state_t *pl, const float *r, const rm_outputs_t *out, float reward)
{
    if ((pl == NULL) || (r == NULL) || (out == NULL)) {
        return PL_ERR_NULL;
    }
    update_traces(pl, r);
    const float surprise = reward - pl->reward_mean;
    pl->reward_mean = pl->reward_mean + (MODEL_REWARD_MEAN_RATE * surprise);
    update_weights(pl, surprise);
    pl->ref_left = pl->ref_left + (MODEL_REST_RATE * (out->steer_left - pl->ref_left));
    pl->ref_right = pl->ref_right + (MODEL_REST_RATE * (out->steer_right - pl->ref_right));
    return PL_OK;
}

float pl_drift(const pl_state_t *pl)
{
    if (pl == NULL) {
        return 0.0F;
    }
    float num = 0.0F;
    float den = 0.0F;
    for (uint32_t m = 0U; m < pl->n; m++) {
        const float w0 = model_values[pl->entry[m]];
        const float d = pl->w[pl->entry[m]] - w0;
        num += d * d;
        den += w0 * w0;
    }
    return (den > 0.0F) ? sqrtf(num / den) : 0.0F;
}
