/**
 * @file brain_protocol.c
 * @brief Brain UART protocol v1 helpers (pure logic, no HAL). See brain_protocol.h.
 */
#include "brain_protocol.h"

#include <string.h>

#define CRC32_POLY_REFLECTED 0xEDB88320U
#define CRC32_INIT 0xFFFFFFFFU
#define CRC32_XOR_OUT 0xFFFFFFFFU
#define BITS_PER_BYTE 8U
#define Q15_MAX 32767
#define Q15_MAX_F 32767.0F
#define ROUND_HALF 0.5F
#define BYTE_MASK 0xFFU
#define SHIFT_8 8U
#define SHIFT_16 16U
#define SHIFT_24 24U
#define FRAME_CRC_START 1U /* CRC covers VERSION .. PAYLOAD */

_Static_assert(BRAIN_STEP_PAYLOAD_BYTES == (2U * BRAIN_N_FEATURES),
               "STEP payload is 2 bytes per feature");
_Static_assert(BRAIN_STEP_RESP_PAYLOAD_BYTES == (BRAIN_RESP_OFF_BACKWARD + 4U),
               "STEP response layout");
_Static_assert(BRAIN_INFO_PAYLOAD_BYTES == (BRAIN_INFO_OFF_CORE_CLOCK_KHZ + 4U), "INFO layout");
_Static_assert(BRAIN_MAX_FRAME_BYTES <= 255U, "parser count is uint8_t");

typedef enum { CHECK_MORE, CHECK_GOOD, CHECK_BAD } check_t;

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & BYTE_MASK);
    p[1] = (uint8_t)((uint32_t)v >> SHIFT_8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & BYTE_MASK);
    p[1] = (uint8_t)((v >> SHIFT_8) & BYTE_MASK);
    p[2] = (uint8_t)((v >> SHIFT_16) & BYTE_MASK);
    p[3] = (uint8_t)((v >> SHIFT_24) & BYTE_MASK);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << SHIFT_8) | ((uint32_t)p[2] << SHIFT_16) |
           ((uint32_t)p[3] << SHIFT_24);
}

static int16_t get_i16(const uint8_t *p)
{
    const uint16_t u = (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << SHIFT_8));
    int16_t s;
    (void)memcpy(&s, &u, sizeof(s));
    return s;
}

static void put_f32(uint8_t *p, float v)
{
    uint32_t bits;
    (void)memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}

static float get_f32(const uint8_t *p)
{
    const uint32_t bits = get_u32(p);
    float v;
    (void)memcpy(&v, &bits, sizeof(v));
    return v;
}

uint32_t brain_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = CRC32_INIT;
    for (size_t i = 0U; (data != NULL) && (i < len); i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < BITS_PER_BYTE; bit++) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ CRC32_POLY_REFLECTED) : (crc >> 1U);
        }
    }
    return crc ^ CRC32_XOR_OUT;
}

brain_status_t brain_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len,
                                  uint8_t *out, size_t cap, size_t *out_len)
{
    if ((out == NULL) || (out_len == NULL) || ((payload == NULL) && (len != 0U))) {
        return BRAIN_ERR_NULL;
    }
    const size_t total = (size_t)BRAIN_FRAME_OVERHEAD_BYTES + len;
    if ((len > BRAIN_MAX_PAYLOAD_BYTES) || (cap < total)) {
        return BRAIN_ERR_SIZE;
    }
    out[BRAIN_OFF_SOF] = (uint8_t)BRAIN_SOF;
    out[BRAIN_OFF_VERSION] = (uint8_t)BRAIN_PROTOCOL_VERSION;
    out[BRAIN_OFF_TYPE] = type;
    out[BRAIN_OFF_SEQ] = seq;
    out[BRAIN_OFF_LEN] = len;
    if (len != 0U) {
        (void)memcpy(&out[BRAIN_OFF_PAYLOAD], payload, len);
    }
    const size_t crc_span = (size_t)BRAIN_HEADER_BYTES - FRAME_CRC_START + len;
    put_u32(&out[BRAIN_OFF_PAYLOAD + len], brain_crc32(&out[FRAME_CRC_START], crc_span));
    *out_len = total;
    return BRAIN_OK;
}

void brain_parser_reset(brain_parser_t *parser)
{
    if (parser != NULL) {
        parser->count = 0U;
    }
}

static check_t check_buffer(const brain_parser_t *p)
{
    if (p->count < BRAIN_HEADER_BYTES) {
        return CHECK_MORE;
    }
    const uint8_t len = p->buf[BRAIN_OFF_LEN];
    if (len > BRAIN_MAX_PAYLOAD_BYTES) {
        return CHECK_BAD;
    }
    const size_t total = (size_t)BRAIN_FRAME_OVERHEAD_BYTES + len;
    if (p->count < total) {
        return CHECK_MORE;
    }
    const size_t crc_span = (size_t)BRAIN_HEADER_BYTES - FRAME_CRC_START + len;
    const uint32_t want = get_u32(&p->buf[BRAIN_OFF_PAYLOAD + len]);
    return (brain_crc32(&p->buf[FRAME_CRC_START], crc_span) == want) ? CHECK_GOOD : CHECK_BAD;
}

/** Drop `n` bytes from the front of the buffer. */
static void drop_front(brain_parser_t *p, size_t n)
{
    p->count = (uint8_t)(p->count - n);
    (void)memmove(p->buf, &p->buf[n], p->count);
}

/** Discard the false SOF at index 0 and everything up to the next SOF byte. */
static void resync(brain_parser_t *p)
{
    size_t next = 1U;
    while ((next < p->count) && (p->buf[next] != (uint8_t)BRAIN_SOF)) {
        next++;
    }
    drop_front(p, next);
}

static void take_frame(brain_parser_t *p, brain_frame_t *frame)
{
    const uint8_t len = p->buf[BRAIN_OFF_LEN];
    frame->version = p->buf[BRAIN_OFF_VERSION];
    frame->type = p->buf[BRAIN_OFF_TYPE];
    frame->seq = p->buf[BRAIN_OFF_SEQ];
    frame->len = len;
    (void)memset(frame->payload, 0, sizeof(frame->payload));
    (void)memcpy(frame->payload, &p->buf[BRAIN_OFF_PAYLOAD], len);
    drop_front(p, (size_t)BRAIN_FRAME_OVERHEAD_BYTES + len);
}

brain_status_t brain_parser_poll(brain_parser_t *parser, brain_frame_t *frame, bool *got_frame)
{
    if ((parser == NULL) || (frame == NULL) || (got_frame == NULL)) {
        return BRAIN_ERR_NULL;
    }
    *got_frame = false;
    /* Each CHECK_BAD removes at least one byte, so this loop runs at most
     * BRAIN_MAX_FRAME_BYTES times. */
    for (uint32_t guard = 0U; (guard <= BRAIN_MAX_FRAME_BYTES) && (parser->count > 0U); guard++) {
        if (parser->buf[0] != (uint8_t)BRAIN_SOF) {
            resync(parser);
            continue;
        }
        const check_t state = check_buffer(parser);
        if (state == CHECK_MORE) {
            break;
        }
        if (state == CHECK_GOOD) {
            take_frame(parser, frame);
            *got_frame = true;
            break;
        }
        resync(parser);
    }
    return BRAIN_OK;
}

brain_status_t brain_parser_push(brain_parser_t *parser, uint8_t byte, brain_frame_t *frame,
                                 bool *got_frame)
{
    if ((parser == NULL) || (frame == NULL) || (got_frame == NULL)) {
        return BRAIN_ERR_NULL;
    }
    *got_frame = false;
    if ((parser->count == 0U) && (byte != (uint8_t)BRAIN_SOF)) {
        return BRAIN_OK;
    }
    if (parser->count >= BRAIN_MAX_FRAME_BYTES) {
        /* Unreachable in normal operation (a full buffer is always resolved), kept as a guard. */
        parser->count = 0U;
        return BRAIN_OK;
    }
    parser->buf[parser->count] = byte;
    parser->count++;
    return brain_parser_poll(parser, frame, got_frame);
}

int16_t brain_feature_to_q15(float feature)
{
    if (!(feature > 0.0F)) {
        return 0; /* also catches NaN */
    }
    if (feature >= 1.0F) {
        return (int16_t)Q15_MAX;
    }
    return (int16_t)((feature * Q15_MAX_F) + ROUND_HALF);
}

float brain_feature_from_q15(int16_t q15)
{
    return (float)q15 / Q15_MAX_F;
}

brain_status_t brain_step_request_decode(const uint8_t *payload, size_t len, float *features)
{
    if ((payload == NULL) || (features == NULL)) {
        return BRAIN_ERR_NULL;
    }
    if (len != BRAIN_STEP_PAYLOAD_BYTES) {
        return BRAIN_ERR_LENGTH;
    }
    for (uint32_t i = 0U; i < BRAIN_N_FEATURES; i++) {
        const int16_t q = get_i16(&payload[(size_t)2U * i]);
        if (q < 0) {
            return BRAIN_ERR_FEATURE;
        }
        features[i] = brain_feature_from_q15(q);
    }
    return BRAIN_OK;
}

brain_status_t brain_step_request_encode(const float *features, uint8_t *payload, size_t cap)
{
    if ((features == NULL) || (payload == NULL)) {
        return BRAIN_ERR_NULL;
    }
    if (cap < BRAIN_STEP_PAYLOAD_BYTES) {
        return BRAIN_ERR_SIZE;
    }
    for (uint32_t i = 0U; i < BRAIN_N_FEATURES; i++) {
        put_u16(&payload[(size_t)2U * i], (uint16_t)brain_feature_to_q15(features[i]));
    }
    return BRAIN_OK;
}

brain_status_t brain_step_resp_encode(const brain_step_resp_t *resp, uint8_t *payload, size_t cap)
{
    if ((resp == NULL) || (payload == NULL)) {
        return BRAIN_ERR_NULL;
    }
    if (cap < BRAIN_STEP_RESP_PAYLOAD_BYTES) {
        return BRAIN_ERR_SIZE;
    }
    payload[BRAIN_RESP_OFF_ACTIONS] = resp->actions;
    payload[BRAIN_RESP_OFF_RESERVED] = 0U;
    put_u32(&payload[BRAIN_RESP_OFF_STEP_CYCLES], resp->step_cycles);
    put_f32(&payload[BRAIN_RESP_OFF_STEER_LEFT], resp->steer_left);
    put_f32(&payload[BRAIN_RESP_OFF_STEER_RIGHT], resp->steer_right);
    put_f32(&payload[BRAIN_RESP_OFF_FORWARD], resp->forward);
    put_f32(&payload[BRAIN_RESP_OFF_BACKWARD], resp->backward);
    return BRAIN_OK;
}

brain_status_t brain_step_resp_decode(const uint8_t *payload, size_t len, brain_step_resp_t *resp)
{
    if ((payload == NULL) || (resp == NULL)) {
        return BRAIN_ERR_NULL;
    }
    if (len != BRAIN_STEP_RESP_PAYLOAD_BYTES) {
        return BRAIN_ERR_LENGTH;
    }
    resp->actions = payload[BRAIN_RESP_OFF_ACTIONS];
    resp->step_cycles = get_u32(&payload[BRAIN_RESP_OFF_STEP_CYCLES]);
    resp->steer_left = get_f32(&payload[BRAIN_RESP_OFF_STEER_LEFT]);
    resp->steer_right = get_f32(&payload[BRAIN_RESP_OFF_STEER_RIGHT]);
    resp->forward = get_f32(&payload[BRAIN_RESP_OFF_FORWARD]);
    resp->backward = get_f32(&payload[BRAIN_RESP_OFF_BACKWARD]);
    return BRAIN_OK;
}
