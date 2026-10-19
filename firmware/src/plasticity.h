/**
 * @file plasticity.h
 * @brief Reward-modulated plasticity on the steering neurons' input synapses. Pure logic.
 *
 * C twin of model/plasticity.py (the rule, its constants and the reasons for each part are
 * documented there; tests/c/test_plasticity.c checks this file against its golden output).
 *
 * The live weights the rate model uses are a RAM copy of the connectome weights in flash
 * (model_values). Only the CSR rows of the steering neurons (MODEL_STEER_LEFT, then
 * MODEL_STEER_RIGHT) ever change. Nothing is written to flash: learning is lost on reset.
 */
#ifndef PLASTICITY_H
#define PLASTICITY_H

#include <stdbool.h>
#include <stdint.h>

#include "model_weights.h"
#include "rate_model.h"

/** Upper bound on learning synapses (the extracted graph has 320). */
#define PL_MAX_LEARNABLE 512U
/** Learning rows: the steering neurons. */
#define PL_N_ROWS (MODEL_N_STEER_LEFT + MODEL_N_STEER_RIGHT)

/** Status codes; PL_OK is zero. */
typedef enum { PL_OK = 0, PL_ERR_NULL = 1, PL_ERR_SIZE = 2 } pl_status_t;

/** Learning state. Large (about 30 KB): keep one static instance. */
typedef struct {
    float w[MODEL_NNZ];                 /**< Live weights, CSR order; read by rm_step(). */
    float trace[PL_MAX_LEARNABLE];      /**< Eligibility trace per learning synapse. */
    uint16_t entry[PL_MAX_LEARNABLE];   /**< CSR index of each learning synapse. */
    uint16_t n;                         /**< Number of learning synapses. */
    uint16_t row_begin[PL_N_ROWS + 1U]; /**< Learning synapses of row j: row_begin[j] .. [j+1]. */
    uint16_t row_neuron[PL_N_ROWS];     /**< Neuron index of each learning row. */
    float row_abs0[PL_N_ROWS];          /**< Original sum |w| of each learning row. */
    float base[PL_N_ROWS];              /**< Running mean activity of each learning row. */
    float reward_mean;
    float ref_left;  /**< Moving turn reference for the left steering group. */
    float ref_right; /**< Moving turn reference for the right steering group. */
} pl_state_t;

/**
 * @brief Copy the connectome weights to RAM and find the learning synapses.
 * @param[out] pl  State.
 * @return PL_OK, PL_ERR_NULL, or PL_ERR_SIZE if there are more than PL_MAX_LEARNABLE.
 */
pl_status_t pl_init(pl_state_t *pl);

/**
 * @brief Back to the connectome weights; traces, baselines, reward mean and turn references
 *        reset. The list of learning synapses is kept.
 * @param[in,out] pl  State initialised by pl_init().
 * @return PL_OK or PL_ERR_NULL.
 */
pl_status_t pl_reset_weights(pl_state_t *pl);

/**
 * @brief Decode turn actions against the moving references (used while learning).
 * @param[in]  pl          State.
 * @param[in]  out         Group means from rm_outputs().
 * @param[out] turn_left   True to turn left.
 * @param[out] turn_right  True to turn right.
 * @return PL_OK or PL_ERR_NULL.
 */
pl_status_t pl_decode_turn(const pl_state_t *pl, const rm_outputs_t *out, bool *turn_left,
                           bool *turn_right);

/**
 * @brief One frame of learning, after the model step.
 * @param[in,out] pl      State.
 * @param[in]     r       MODEL_N_NEURONS activities after the step.
 * @param[in]     out     Group means of the same step.
 * @param[in]     reward  This frame's reward, -1 .. +1.
 * @return PL_OK or PL_ERR_NULL.
 */
pl_status_t pl_update(pl_state_t *pl, const float *r, const rm_outputs_t *out, float reward);

/**
 * @brief Relative L2 distance of the learning weights from the connectome (0 = unchanged).
 * @param[in] pl  State.
 * @return The drift; 0 if pl is NULL.
 */
float pl_drift(const pl_state_t *pl);

#endif /* PLASTICITY_H */
