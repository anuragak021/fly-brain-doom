/**
 * @file test_brain_server.c
 * @brief Host test: brain_server request handling, error replies and state semantics.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "brain_server.h"

#define FAKE_CYCLES_PER_CALL 1000U
#define N_STEPS 5U

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static uint32_t g_fake_cycles;

static uint32_t fake_cycles(void)
{
    g_fake_cycles += FAKE_CYCLES_PER_CALL;
    return g_fake_cycles;
}

/** Build a request frame struct directly (lets tests set a wrong version). */
static brain_frame_t make_req(uint8_t version, uint8_t type, uint8_t seq, const uint8_t *payload,
                              uint8_t len)
{
    brain_frame_t f;
    (void)memset(&f, 0, sizeof(f));
    f.version = version;
    f.type = type;
    f.seq = seq;
    f.len = len;
    if (len != 0U) {
        (void)memcpy(f.payload, payload, len);
    }
    return f;
}

/** Run the handler and parse the response back through the real parser. */
static brain_frame_t roundtrip(brain_server_t *srv, const brain_frame_t *req)
{
    uint8_t out[BRAIN_MAX_FRAME_BYTES];
    size_t out_len = 0U;
    brain_parser_t p;
    brain_frame_t resp;
    bool got = false;
    (void)memset(&resp, 0, sizeof(resp));
    CHECK(brain_server_handle(srv, req, out, sizeof(out), &out_len) == BRAIN_OK);
    brain_parser_reset(&p);
    for (size_t i = 0U; i < out_len; i++) {
        CHECK(brain_parser_push(&p, out[i], &resp, &got) == BRAIN_OK);
    }
    CHECK(got);
    return resp;
}

static void step_payload(const float *features, uint8_t *payload)
{
    CHECK(brain_step_request_encode(features, payload, BRAIN_STEP_PAYLOAD_BYTES) == BRAIN_OK);
}

static void test_ping_info(void)
{
    static brain_server_t srv;
    const uint32_t clock_khz = 64000U;
    CHECK(brain_server_init(&srv, clock_khz, fake_cycles) == BRAIN_OK);
    const brain_frame_t req = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_PING, 7U, NULL, 0U);
    const brain_frame_t r = roundtrip(&srv, &req);
    CHECK((r.type == BRAIN_MSG_INFO) && (r.seq == 7U) && (r.len == BRAIN_INFO_PAYLOAD_BYTES));
    CHECK(r.payload[BRAIN_INFO_OFF_PROTO_VERSION] == BRAIN_PROTOCOL_VERSION);
    CHECK(r.payload[BRAIN_INFO_OFF_N_FEATURES] == BRAIN_N_FEATURES);
    const uint32_t neurons = (uint32_t)r.payload[BRAIN_INFO_OFF_N_NEURONS] |
                             ((uint32_t)r.payload[BRAIN_INFO_OFF_N_NEURONS + 1U] << 8U);
    CHECK(neurons == MODEL_N_NEURONS);
    CHECK(r.payload[BRAIN_INFO_OFF_SUBSTEPS] == MODEL_SUBSTEPS_PER_FRAME);
    const uint8_t *c = &r.payload[BRAIN_INFO_OFF_CORE_CLOCK_KHZ];
    CHECK((uint32_t)c[0] + ((uint32_t)c[1] << 8U) + ((uint32_t)c[2] << 16U) +
              ((uint32_t)c[3] << 24U) ==
          clock_khz);
}

/** STEP through the server must equal calling the model directly, step for step. */
static void test_step_matches_direct_model(void)
{
    static brain_server_t srv;
    static rm_state_t direct;
    const float feats[BRAIN_N_FEATURES] = {0.2F, 0.4F, 0.6F, 0.0F};
    float quantised[BRAIN_N_FEATURES];
    float drive[MODEL_N_NEURONS];
    uint8_t payload[BRAIN_STEP_PAYLOAD_BYTES];
    CHECK(brain_server_init(&srv, 64000U, fake_cycles) == BRAIN_OK);
    CHECK(rm_reset(&direct) == RM_OK);
    step_payload(feats, payload);
    CHECK(brain_step_request_decode(payload, sizeof(payload), quantised) == BRAIN_OK);
    for (uint32_t i = 0U; i < N_STEPS; i++) {
        const brain_frame_t req =
            make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_STEP, (uint8_t)i, payload, sizeof(payload));
        const brain_frame_t r = roundtrip(&srv, &req);
        brain_step_resp_t resp;
        rm_outputs_t o;
        bool tl = false;
        bool tr = false;
        CHECK((r.type == BRAIN_MSG_STEP_RESP) && (r.seq == (uint8_t)i));
        CHECK(brain_step_resp_decode(r.payload, r.len, &resp) == BRAIN_OK);
        CHECK(rm_make_drive(quantised, drive) == RM_OK);
        CHECK(rm_step(&direct, drive, MODEL_SUBSTEPS_PER_FRAME) == RM_OK);
        CHECK(rm_outputs(&direct, &o) == RM_OK);
        CHECK(rm_decode_turn(&o, &tl, &tr) == RM_OK);
        CHECK(resp.steer_left == o.steer_left);
        CHECK(resp.steer_right == o.steer_right);
        CHECK(resp.forward == o.forward);
        CHECK(resp.backward == o.backward);
        CHECK(resp.actions ==
              (uint8_t)((tl ? BRAIN_ACTION_TURN_LEFT : 0U) | (tr ? BRAIN_ACTION_TURN_RIGHT : 0U)));
        CHECK((resp.actions & BRAIN_ACTION_RESERVED_MASK) == 0U);
        CHECK((resp.actions & (BRAIN_ACTION_TURN_LEFT | BRAIN_ACTION_TURN_RIGHT)) !=
              (BRAIN_ACTION_TURN_LEFT | BRAIN_ACTION_TURN_RIGHT));
        CHECK(resp.step_cycles == (2U * FAKE_CYCLES_PER_CALL) - FAKE_CYCLES_PER_CALL);
    }
}

static void test_reset_returns_to_first_step(void)
{
    static brain_server_t srv;
    const float feats[BRAIN_N_FEATURES] = {0.1F, 0.9F, 0.0F, 0.7F};
    uint8_t payload[BRAIN_STEP_PAYLOAD_BYTES];
    brain_step_resp_t first;
    brain_step_resp_t after;
    CHECK(brain_server_init(&srv, 64000U, fake_cycles) == BRAIN_OK);
    step_payload(feats, payload);
    const brain_frame_t step =
        make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_STEP, 1U, payload, sizeof(payload));
    brain_frame_t r = roundtrip(&srv, &step);
    CHECK(brain_step_resp_decode(r.payload, r.len, &first) == BRAIN_OK);
    for (uint32_t i = 0U; i < N_STEPS; i++) {
        r = roundtrip(&srv, &step);
    }
    const brain_frame_t reset = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_RESET, 9U, NULL, 0U);
    r = roundtrip(&srv, &reset);
    CHECK((r.type == BRAIN_MSG_RESET_RESP) && (r.seq == 9U) && (r.len == 0U));
    r = roundtrip(&srv, &step);
    CHECK(brain_step_resp_decode(r.payload, r.len, &after) == BRAIN_OK);
    CHECK((after.steer_left == first.steer_left) && (after.steer_right == first.steer_right));
}

static void expect_error(brain_server_t *srv, const brain_frame_t *req, uint8_t code)
{
    const brain_frame_t r = roundtrip(srv, req);
    CHECK(r.type == BRAIN_MSG_ERROR);
    CHECK(r.seq == req->seq);
    CHECK(r.len == BRAIN_ERROR_PAYLOAD_BYTES);
    CHECK(r.payload[BRAIN_ERROR_OFF_CODE] == code);
    CHECK(r.payload[BRAIN_ERROR_OFF_REQUEST_TYPE] == req->type);
}

static void test_error_replies(void)
{
    static brain_server_t srv;
    uint8_t good[BRAIN_STEP_PAYLOAD_BYTES] = {0};
    uint8_t negative[BRAIN_STEP_PAYLOAD_BYTES] = {0, 0, 0, 0, 0x00, 0x80, 0, 0};
    CHECK(brain_server_init(&srv, 64000U, fake_cycles) == BRAIN_OK);

    brain_frame_t req = make_req(2U, BRAIN_MSG_PING, 3U, NULL, 0U);
    expect_error(&srv, &req, BRAIN_ERR_BAD_VERSION);
    req = make_req(BRAIN_PROTOCOL_VERSION, 0x55U, 4U, NULL, 0U);
    expect_error(&srv, &req, BRAIN_ERR_UNKNOWN_TYPE);
    req = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_INFO, 5U, NULL, 0U); /* a response type */
    expect_error(&srv, &req, BRAIN_ERR_UNKNOWN_TYPE);
    req = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_PING, 6U, good, 1U);
    expect_error(&srv, &req, BRAIN_ERR_BAD_LENGTH);
    req = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_RESET, 7U, good, 2U);
    expect_error(&srv, &req, BRAIN_ERR_BAD_LENGTH);
    req = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_STEP, 8U, good, 7U);
    expect_error(&srv, &req, BRAIN_ERR_BAD_LENGTH);
    req = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_STEP, 9U, negative, sizeof(negative));
    expect_error(&srv, &req, BRAIN_ERR_BAD_FEATURE);
}

static void test_null_and_size_errors(void)
{
    static brain_server_t srv;
    uint8_t out[BRAIN_MAX_FRAME_BYTES];
    uint8_t tiny[4];
    size_t len = 0U;
    const brain_frame_t req = make_req(BRAIN_PROTOCOL_VERSION, BRAIN_MSG_PING, 1U, NULL, 0U);
    CHECK(brain_server_init(NULL, 1U, fake_cycles) == BRAIN_ERR_NULL);
    CHECK(brain_server_init(&srv, 1U, NULL) == BRAIN_ERR_NULL);
    CHECK(brain_server_init(&srv, 1U, fake_cycles) == BRAIN_OK);
    CHECK(brain_server_handle(NULL, &req, out, sizeof(out), &len) == BRAIN_ERR_NULL);
    CHECK(brain_server_handle(&srv, NULL, out, sizeof(out), &len) == BRAIN_ERR_NULL);
    CHECK(brain_server_handle(&srv, &req, NULL, sizeof(out), &len) == BRAIN_ERR_NULL);
    CHECK(brain_server_handle(&srv, &req, out, sizeof(out), NULL) == BRAIN_ERR_NULL);
    CHECK(brain_server_handle(&srv, &req, tiny, sizeof(tiny), &len) == BRAIN_ERR_SIZE);
}

int main(void)
{
    test_ping_info();
    test_step_matches_direct_model();
    test_reset_returns_to_first_step();
    test_error_replies();
    test_null_and_size_errors();
    if (g_failures != 0) {
        (void)printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    (void)printf("all checks passed\n");
    return 0;
}
