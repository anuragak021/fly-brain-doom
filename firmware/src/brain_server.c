/**
 * @file brain_server.c
 * @brief Brain protocol request handler. See brain_server.h.
 */
#include "brain_server.h"

#include <stddef.h>

#define U16_SHIFT_8 8U
#define BYTE_MASK 0xFFU
#define SHIFT_8 8U
#define SHIFT_16 16U
#define SHIFT_24 24U

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & BYTE_MASK);
    p[1] = (uint8_t)((v >> SHIFT_8) & BYTE_MASK);
    p[2] = (uint8_t)((v >> SHIFT_16) & BYTE_MASK);
    p[3] = (uint8_t)((v >> SHIFT_24) & BYTE_MASK);
}

static brain_status_t reply(const brain_frame_t *req, uint8_t type, const uint8_t *payload,
                            uint8_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    return brain_frame_encode(type, req->seq, payload, len, out, cap, out_len);
}

static brain_status_t reply_error(const brain_frame_t *req, uint8_t code, uint8_t *out, size_t cap,
                                  size_t *out_len)
{
    uint8_t payload[BRAIN_ERROR_PAYLOAD_BYTES];
    payload[BRAIN_ERROR_OFF_CODE] = code;
    payload[BRAIN_ERROR_OFF_REQUEST_TYPE] = req->type;
    return reply(req, BRAIN_MSG_ERROR, payload, BRAIN_ERROR_PAYLOAD_BYTES, out, cap, out_len);
}

static brain_status_t handle_ping(const brain_server_t *srv, const brain_frame_t *req, uint8_t *out,
                                  size_t cap, size_t *out_len)
{
    uint8_t p[BRAIN_INFO_PAYLOAD_BYTES];
    p[BRAIN_INFO_OFF_PROTO_VERSION] = (uint8_t)BRAIN_PROTOCOL_VERSION;
    p[BRAIN_INFO_OFF_N_FEATURES] = (uint8_t)BRAIN_N_FEATURES;
    p[BRAIN_INFO_OFF_N_NEURONS] = (uint8_t)(MODEL_N_NEURONS & BYTE_MASK);
    p[BRAIN_INFO_OFF_N_NEURONS + 1U] = (uint8_t)(MODEL_N_NEURONS >> U16_SHIFT_8);
    p[BRAIN_INFO_OFF_SUBSTEPS] = (uint8_t)MODEL_SUBSTEPS_PER_FRAME;
    put_u32(&p[BRAIN_INFO_OFF_CORE_CLOCK_KHZ], srv->core_clock_khz);
    return reply(req, BRAIN_MSG_INFO, p, BRAIN_INFO_PAYLOAD_BYTES, out, cap, out_len);
}

static uint8_t action_bits(bool turn_left, bool turn_right)
{
    uint8_t bits = 0U;
    if (turn_left) {
        bits |= (uint8_t)BRAIN_ACTION_TURN_LEFT;
    }
    if (turn_right) {
        bits |= (uint8_t)BRAIN_ACTION_TURN_RIGHT;
    }
    return bits;
}

static brain_status_t handle_step(brain_server_t *srv, const brain_frame_t *req, uint8_t *out,
                                  size_t cap, size_t *out_len)
{
    float features[BRAIN_N_FEATURES];
    float drive[MODEL_N_NEURONS];
    rm_outputs_t outputs;
    bool turn_left = false;
    bool turn_right = false;
    uint8_t payload[BRAIN_STEP_RESP_PAYLOAD_BYTES];

    const brain_status_t dec = brain_step_request_decode(req->payload, req->len, features);
    if (dec == BRAIN_ERR_LENGTH) {
        return reply_error(req, BRAIN_ERR_BAD_LENGTH, out, cap, out_len);
    }
    if (dec != BRAIN_OK) {
        return reply_error(req, BRAIN_ERR_BAD_FEATURE, out, cap, out_len);
    }
    const uint32_t t0 = srv->cycles();
    const rm_status_t s1 = rm_make_drive(features, drive);
    const rm_status_t s2 = rm_step(&srv->model, drive, MODEL_SUBSTEPS_PER_FRAME);
    const rm_status_t s3 = rm_outputs(&srv->model, &outputs);
    const rm_status_t s4 = rm_decode_turn(&outputs, &turn_left, &turn_right);
    const uint32_t cycles = srv->cycles() - t0;
    if ((s1 != RM_OK) || (s2 != RM_OK) || (s3 != RM_OK) || (s4 != RM_OK)) {
        return reply_error(req, BRAIN_ERR_BAD_FEATURE, out, cap, out_len);
    }
    const brain_step_resp_t resp = {.actions = action_bits(turn_left, turn_right),
                                    .step_cycles = cycles,
                                    .steer_left = outputs.steer_left,
                                    .steer_right = outputs.steer_right,
                                    .forward = outputs.forward,
                                    .backward = outputs.backward};
    if (brain_step_resp_encode(&resp, payload, sizeof(payload)) != BRAIN_OK) {
        return BRAIN_ERR_SIZE;
    }
    return reply(req, BRAIN_MSG_STEP_RESP, payload, BRAIN_STEP_RESP_PAYLOAD_BYTES, out, cap,
                 out_len);
}

brain_status_t brain_server_init(brain_server_t *srv, uint32_t core_clock_khz,
                                 brain_cycle_fn_t cycles)
{
    if ((srv == NULL) || (cycles == NULL)) {
        return BRAIN_ERR_NULL;
    }
    srv->core_clock_khz = core_clock_khz;
    srv->cycles = cycles;
    return (rm_reset(&srv->model) == RM_OK) ? BRAIN_OK : BRAIN_ERR_NULL;
}

brain_status_t brain_server_handle(brain_server_t *srv, const brain_frame_t *req, uint8_t *out,
                                   size_t cap, size_t *out_len)
{
    if ((srv == NULL) || (req == NULL) || (out == NULL) || (out_len == NULL)) {
        return BRAIN_ERR_NULL;
    }
    if (req->version != BRAIN_PROTOCOL_VERSION) {
        return reply_error(req, BRAIN_ERR_BAD_VERSION, out, cap, out_len);
    }
    switch (req->type) {
    case BRAIN_MSG_PING:
        if (req->len != 0U) {
            return reply_error(req, BRAIN_ERR_BAD_LENGTH, out, cap, out_len);
        }
        return handle_ping(srv, req, out, cap, out_len);
    case BRAIN_MSG_STEP:
        return handle_step(srv, req, out, cap, out_len);
    case BRAIN_MSG_RESET:
        if (req->len != 0U) {
            return reply_error(req, BRAIN_ERR_BAD_LENGTH, out, cap, out_len);
        }
        if (rm_reset(&srv->model) != RM_OK) {
            return BRAIN_ERR_NULL;
        }
        return reply(req, BRAIN_MSG_RESET_RESP, NULL, 0U, out, cap, out_len);
    default:
        return reply_error(req, BRAIN_ERR_UNKNOWN_TYPE, out, cap, out_len);
    }
}
