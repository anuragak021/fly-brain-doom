/**
 * @file brain_server.h
 * @brief Brain protocol request handler: frame in, response frame out. Pure logic, no HAL.
 *
 * Owns the neuron state. The cycle counter is injected so the same code runs on the STM32
 * (DWT->CYCCNT) and on the host (a fake counter in the tests).
 */
#ifndef BRAIN_SERVER_H
#define BRAIN_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brain_protocol.h"
#include "rate_model.h"

/** Returns a free-running 32-bit cycle counter (wraps; differences are taken unsigned). */
typedef uint32_t (*brain_cycle_fn_t)(void);

/** Server state: neuron activities plus the platform hooks. */
typedef struct {
    rm_state_t model;
    uint32_t core_clock_khz;
    brain_cycle_fn_t cycles;
} brain_server_t;

/**
 * @brief Initialise the server and zero the neuron state.
 * @param[out] srv             Server to initialise.
 * @param[in]  core_clock_khz  Core clock, reported in INFO so cycle counts can be converted.
 * @param[in]  cycles          Cycle counter source (must not be NULL).
 * @return BRAIN_OK or BRAIN_ERR_NULL.
 */
brain_status_t brain_server_init(brain_server_t *srv, uint32_t core_clock_khz,
                                 brain_cycle_fn_t cycles);

/**
 * @brief Handle one CRC-valid request frame and build the response frame.
 *
 * Wrong VERSION, unknown TYPE, wrong payload LEN and negative features produce a
 * BRAIN_MSG_ERROR response that echoes the request SEQ. The response is always produced
 * for a valid frame, so `*out_len` is non-zero on BRAIN_OK.
 *
 * STEP cycle count covers rm_make_drive, rm_step, rm_outputs and rm_decode_turn.
 *
 * @param[in,out] srv      Server.
 * @param[in]     req      Request frame (already CRC-checked by the parser).
 * @param[out]    out      Buffer for the complete response frame.
 * @param[in]     cap      Capacity of `out`; BRAIN_MAX_FRAME_BYTES is always enough.
 * @param[out]    out_len  Bytes written to `out`.
 * @return BRAIN_OK, BRAIN_ERR_NULL or BRAIN_ERR_SIZE.
 */
brain_status_t brain_server_handle(brain_server_t *srv, const brain_frame_t *req, uint8_t *out,
                                   size_t cap, size_t *out_len);

#endif /* BRAIN_SERVER_H */
