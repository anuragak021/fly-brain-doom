/**
 * @file rate_model.h
 * @brief Leaky rate model on a CSR connectome subgraph. Pure logic, no HAL.
 *
 *   tau * dr/dt = -r + relu(W r + I)      (explicit Euler, step fraction MODEL_ALPHA)
 *
 * W[post, pre] is stored in CSR (see model_weights.h). All state lives in a
 * caller owned rm_state_t; there is no dynamic allocation.
 */
#ifndef RATE_MODEL_H
#define RATE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "model_weights.h"

/** Number of values in the feature vector (luminance L/R, enemy L/R, damage L/R). */
#define RM_N_FEATURES 6U

/** Status codes; RM_OK is zero. */
typedef enum { RM_OK = 0, RM_ERR_NULL = 1, RM_ERR_RANGE = 2 } rm_status_t;

/** Index into the feature vector. */
typedef enum {
    RM_FEAT_LUM_LEFT = 0,
    RM_FEAT_LUM_RIGHT = 1,
    RM_FEAT_ENEMY_LEFT = 2,
    RM_FEAT_ENEMY_RIGHT = 3,
    RM_FEAT_DAMAGE_LEFT = 4,
    RM_FEAT_DAMAGE_RIGHT = 5
} rm_feature_t;

/** Mean activity of each output group. */
typedef struct {
    float steer_left;
    float steer_right;
    float forward;
    float backward;
} rm_outputs_t;

/** Model state: activities and a scratch buffer for the synchronous update. */
typedef struct {
    float r[MODEL_N_NEURONS];
    float next[MODEL_N_NEURONS];
} rm_state_t;

/**
 * @brief Set all activities to zero.
 * @param[out] st  State to clear.
 * @return RM_OK, or RM_ERR_NULL if st is NULL.
 */
rm_status_t rm_reset(rm_state_t *st);

/**
 * @brief Build the per neuron drive vector from a feature vector.
 *
 * Non-input neurons get MODEL_TONIC_DRIVE. Input neurons on each side get
 * MODEL_INPUT_GAIN * (enemy + MODEL_LUM_WEIGHT * luminance + MODEL_DAMAGE_WEIGHT * damage)
 * for that side.
 *
 * @param[in]  features  RM_N_FEATURES values, each expected in 0..1.
 * @param[out] drive     MODEL_N_NEURONS values.
 * @return RM_OK, RM_ERR_NULL for NULL pointers, RM_ERR_RANGE for non-finite features.
 */
rm_status_t rm_make_drive(const float *features, float *drive);

/**
 * @brief Advance the model by `substeps` Euler steps under a constant drive.
 * @param[in,out] st        Model state.
 * @param[in]     values    MODEL_NNZ weights in CSR order: model_values (the connectome) or a
 *                          learning copy of it (plasticity.h).
 * @param[in]     drive     MODEL_N_NEURONS values.
 * @param[in]     substeps  Number of steps; must be in 1..RM_MAX_SUBSTEPS.
 * @return RM_OK, RM_ERR_NULL, or RM_ERR_RANGE.
 */
rm_status_t rm_step(rm_state_t *st, const float *values, const float *drive, uint32_t substeps);

/**
 * @brief Pool the output groups into mean activities.
 * @param[in]  st   Model state.
 * @param[out] out  Group means.
 * @return RM_OK or RM_ERR_NULL.
 */
rm_status_t rm_outputs(const rm_state_t *st, rm_outputs_t *out);

/**
 * @brief Decode turn actions from output activities, relative to the resting state.
 *
 * diff = ((steer_right - rest_right) - (steer_left - rest_left)) * MODEL_TURN_SIGN;
 * turn left if diff > MODEL_TURN_DEADZONE, turn right if diff < -MODEL_TURN_DEADZONE.
 * The two outputs are mutually exclusive. Same rule as `decode_action` in game/controller.py.
 *
 * @param[in]  out         Group means from rm_outputs().
 * @param[out] turn_left   True to turn left.
 * @param[out] turn_right  True to turn right.
 * @return RM_OK or RM_ERR_NULL.
 */
rm_status_t rm_decode_turn(const rm_outputs_t *out, bool *turn_left, bool *turn_right);

/** Upper bound on substeps per call (guards against absurd external input). */
#define RM_MAX_SUBSTEPS 64U

#endif /* RATE_MODEL_H */
