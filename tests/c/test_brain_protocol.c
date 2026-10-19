/**
 * @file test_brain_protocol.c
 * @brief Host test: brain protocol v1 against the known-answer vectors and hostile input.
 *
 * Usage: test_brain_protocol <protocol_vectors.txt>
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_protocol.h"

#define LINE_BYTES 512U
#define TOKEN_BYTES 128U
#define HEX_BUF_BYTES 128U
#define MAX_VECTOR_FRAMES 16U

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t payload[BRAIN_MAX_PAYLOAD_BYTES];
    size_t payload_len;
    uint8_t frame[BRAIN_MAX_FRAME_BYTES];
    size_t frame_len;
} vec_frame_t;

static vec_frame_t g_frames[MAX_VECTOR_FRAMES];
static size_t g_n_frames = 0U;

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0U;
    if (strcmp(hex, "-") == 0) {
        return 0U;
    }
    for (size_t i = 0U; (hex[i] != '\0') && (hex[i + 1U] != '\0') && (n < cap); i += 2U) {
        char pair[3] = {hex[i], hex[i + 1U], '\0'};
        out[n] = (uint8_t)strtoul(pair, NULL, 16);
        n++;
    }
    return n;
}

static bool same_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

/** Feed `n` bytes; return the number of frames produced and copy the last one. */
static size_t feed(brain_parser_t *p, const uint8_t *data, size_t n, brain_frame_t *last)
{
    size_t frames = 0U;
    for (size_t i = 0U; i < n; i++) {
        bool got = false;
        brain_frame_t f;
        CHECK(brain_parser_push(p, data[i], &f, &got) == BRAIN_OK);
        while (got) {
            frames++;
            *last = f;
            CHECK(brain_parser_poll(p, &f, &got) == BRAIN_OK);
        }
    }
    return frames;
}

static void line_crc(const char *line)
{
    char text[TOKEN_BYTES];
    unsigned int want;
    CHECK(sscanf(line, "CRC %127s %x", text, &want) == 2);
    CHECK(brain_crc32((const uint8_t *)text, strlen(text)) == (uint32_t)want);
    CHECK(brain_crc32((const uint8_t *)text, strlen(text)) == BRAIN_CRC32_CHECK_VALUE);
}

static void line_q15(const char *line)
{
    char text[TOKEN_BYTES];
    int want;
    CHECK(sscanf(line, "Q15 %127s %d", text, &want) == 2);
    CHECK((int)brain_feature_to_q15(strtof(text, NULL)) == want);
}

static void line_rq15(const char *line)
{
    char text[TOKEN_BYTES];
    int want;
    CHECK(sscanf(line, "RQ15 %127s %d", text, &want) == 2);
    CHECK((int)brain_reward_to_q15(strtof(text, NULL)) == want);
}

static void line_stepreq(const char *line)
{
    char f[BRAIN_N_FEATURES + 1U][TOKEN_BYTES];
    unsigned int flags;
    char hex[HEX_BUF_BYTES];
    uint8_t want[HEX_BUF_BYTES];
    uint8_t got[BRAIN_STEP_PAYLOAD_BYTES];
    brain_step_req_t req;
    brain_step_req_t back;
    CHECK(sscanf(line, "STEPREQ %127s %127s %127s %127s %127s %127s %127s %u %127s", f[0], f[1],
                 f[2], f[3], f[4], f[5], f[6], &flags, hex) == 9);
    for (uint32_t i = 0U; i < BRAIN_N_FEATURES; i++) {
        req.features[i] = strtof(f[i], NULL);
    }
    req.reward = strtof(f[BRAIN_N_FEATURES], NULL);
    req.flags = (uint8_t)flags;
    const size_t n = hex_to_bytes(hex, want, sizeof(want));
    CHECK(n == BRAIN_STEP_PAYLOAD_BYTES);
    CHECK(brain_step_request_encode(&req, got, sizeof(got)) == BRAIN_OK);
    CHECK(same_bytes(got, want, n));
    CHECK(brain_step_request_decode(want, n, &back) == BRAIN_OK);
    for (uint32_t i = 0U; i < BRAIN_N_FEATURES; i++) {
        CHECK(fabsf(back.features[i] - req.features[i]) <= (1.0F / 32767.0F));
    }
    CHECK(fabsf(back.reward - req.reward) <= (1.0F / 32767.0F));
    CHECK(back.flags == req.flags);
    CHECK(brain_step_request_decode(want, n - 1U, &back) == BRAIN_ERR_LENGTH);

    uint8_t bad[BRAIN_STEP_PAYLOAD_BYTES];
    (void)memcpy(bad, want, sizeof(bad));
    bad[2] = 0x00U; /* feature 1 = i16 -32768 */
    bad[3] = 0x80U;
    CHECK(brain_step_request_decode(bad, sizeof(bad), &back) == BRAIN_ERR_FEATURE);
    (void)memcpy(bad, want, sizeof(bad));
    bad[BRAIN_STEP_OFF_REWARD] = 0x00U; /* reward -32768 is rejected */
    bad[BRAIN_STEP_OFF_REWARD + 1U] = 0x80U;
    CHECK(brain_step_request_decode(bad, sizeof(bad), &back) == BRAIN_ERR_FEATURE);
    bad[BRAIN_STEP_OFF_REWARD] = 0x01U; /* -32767 = -1.0 is fine */
    CHECK(brain_step_request_decode(bad, sizeof(bad), &back) == BRAIN_OK);
    CHECK(back.reward == -1.0F);
    (void)memcpy(bad, want, sizeof(bad));
    bad[BRAIN_STEP_OFF_FLAGS] = 0xFFU; /* reserved bits are ignored */
    CHECK(brain_step_request_decode(bad, sizeof(bad), &back) == BRAIN_OK);
    CHECK(back.flags == (BRAIN_STEP_FLAG_LEARN | BRAIN_STEP_FLAG_RESET_WEIGHTS));
}

static void line_stepresp(const char *line)
{
    unsigned int actions;
    unsigned long cycles;
    char v[5][TOKEN_BYTES];
    char hex[HEX_BUF_BYTES];
    uint8_t want[HEX_BUF_BYTES];
    uint8_t got[BRAIN_STEP_RESP_PAYLOAD_BYTES];
    brain_step_resp_t r;
    brain_step_resp_t d;
    CHECK(sscanf(line, "STEPRESP %u %lu %127s %127s %127s %127s %127s %127s", &actions, &cycles,
                 v[0], v[1], v[2], v[3], v[4], hex) == 8);
    r.actions = (uint8_t)actions;
    r.step_cycles = (uint32_t)cycles;
    r.steer_left = strtof(v[0], NULL);
    r.steer_right = strtof(v[1], NULL);
    r.forward = strtof(v[2], NULL);
    r.backward = strtof(v[3], NULL);
    r.weight_drift = strtof(v[4], NULL);
    const size_t n = hex_to_bytes(hex, want, sizeof(want));
    CHECK(n == BRAIN_STEP_RESP_PAYLOAD_BYTES);
    CHECK(brain_step_resp_encode(&r, got, sizeof(got)) == BRAIN_OK);
    CHECK(same_bytes(got, want, n));
    CHECK(brain_step_resp_decode(want, n, &d) == BRAIN_OK);
    CHECK((d.actions == r.actions) && (d.step_cycles == r.step_cycles));
    CHECK((d.steer_left == r.steer_left) && (d.backward == r.backward));
    CHECK(d.weight_drift == r.weight_drift);
    CHECK(brain_step_resp_decode(want, n + 1U, &d) == BRAIN_ERR_LENGTH);
}

static void line_frame(const char *line)
{
    char name[TOKEN_BYTES];
    char payload_hex[HEX_BUF_BYTES];
    char frame_hex[HEX_BUF_BYTES];
    unsigned int type;
    unsigned int seq;
    uint8_t built[BRAIN_MAX_FRAME_BYTES];
    size_t built_len = 0U;
    CHECK(g_n_frames < MAX_VECTOR_FRAMES);
    vec_frame_t *vf = &g_frames[g_n_frames];
    CHECK(sscanf(line, "FRAME %127s %x %u %127s %127s", name, &type, &seq, payload_hex,
                 frame_hex) == 5);
    vf->type = (uint8_t)type;
    vf->seq = (uint8_t)seq;
    vf->payload_len = hex_to_bytes(payload_hex, vf->payload, sizeof(vf->payload));
    vf->frame_len = hex_to_bytes(frame_hex, vf->frame, sizeof(vf->frame));
    CHECK(brain_frame_encode(vf->type, vf->seq, vf->payload, (uint8_t)vf->payload_len, built,
                             sizeof(built), &built_len) == BRAIN_OK);
    CHECK(built_len == vf->frame_len);
    CHECK(same_bytes(built, vf->frame, vf->frame_len));
    brain_parser_t p;
    brain_frame_t f;
    brain_parser_reset(&p);
    CHECK(feed(&p, vf->frame, vf->frame_len, &f) == 1U);
    CHECK((f.type == vf->type) && (f.seq == vf->seq) && (f.len == vf->payload_len));
    CHECK((f.version == BRAIN_PROTOCOL_VERSION) && same_bytes(f.payload, vf->payload, f.len));
    g_n_frames++;
}

static void load_vectors(const char *path)
{
    char line[LINE_BYTES];
    FILE *fp = fopen(path, "r");
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        if (strncmp(line, "CRC ", 4U) == 0) {
            line_crc(line);
        } else if (strncmp(line, "Q15 ", 4U) == 0) {
            line_q15(line);
        } else if (strncmp(line, "RQ15 ", 5U) == 0) {
            line_rq15(line);
        } else if (strncmp(line, "STEPREQ ", 8U) == 0) {
            line_stepreq(line);
        } else if (strncmp(line, "STEPRESP ", 9U) == 0) {
            line_stepresp(line);
        } else if (strncmp(line, "FRAME ", 6U) == 0) {
            line_frame(line);
        }
    }
    (void)fclose(fp);
    CHECK(g_n_frames == 7U);
}

static void test_back_to_back_and_garbage(void)
{
    uint8_t stream[BRAIN_MAX_FRAME_BYTES * MAX_VECTOR_FRAMES + 8U];
    size_t n = 0U;
    brain_parser_t p;
    brain_frame_t f;
    const uint8_t junk[] = {0x00U, 0x13U, 0xFFU, 0x42U};
    brain_parser_reset(&p);
    (void)memcpy(stream, junk, sizeof(junk));
    n = sizeof(junk);
    for (size_t i = 0U; i < g_n_frames; i++) {
        (void)memcpy(&stream[n], g_frames[i].frame, g_frames[i].frame_len);
        n += g_frames[i].frame_len;
    }
    CHECK(feed(&p, stream, n, &f) == g_n_frames);
}

static void test_sof_inside_payload(void)
{
    const uint8_t payload[BRAIN_STEP_PAYLOAD_BYTES] = {0xA5U, 0x01U, 0xA5U, 0x02U, 0xA5U,
                                                       0x02U, 0xA5U, 0x00U, 0xA5U, 0x02U,
                                                       0xA5U, 0x02U, 0xA5U, 0x02U, 0xA5U};
    uint8_t frame[BRAIN_MAX_FRAME_BYTES];
    size_t len = 0U;
    brain_parser_t p;
    brain_frame_t f;
    CHECK(brain_frame_encode(BRAIN_MSG_STEP, 9U, payload, sizeof(payload), frame, sizeof(frame),
                             &len) == BRAIN_OK);
    brain_parser_reset(&p);
    CHECK(feed(&p, frame, len, &f) == 1U);
    CHECK((f.seq == 9U) && same_bytes(f.payload, payload, sizeof(payload)));
}

static void test_bad_frames_then_good(void)
{
    const uint8_t oversize[] = {0xA5U, 0x01U, 0x02U, 0x01U, 0xFFU}; /* LEN 255 > max */
    brain_parser_t p;
    brain_frame_t f;
    uint8_t bad[BRAIN_MAX_FRAME_BYTES];
    brain_parser_reset(&p);
    CHECK(feed(&p, oversize, sizeof(oversize), &f) == 0U);
    CHECK(feed(&p, g_frames[0].frame, g_frames[0].frame_len, &f) == 1U);

    (void)memcpy(bad, g_frames[2].frame, g_frames[2].frame_len);
    bad[BRAIN_OFF_PAYLOAD] ^= 0x01U; /* corrupt one payload bit */
    CHECK(feed(&p, bad, g_frames[2].frame_len, &f) == 0U);
    CHECK(feed(&p, g_frames[4].frame, g_frames[4].frame_len, &f) == 1U);
    CHECK(f.type == BRAIN_MSG_RESET);
}

static void test_false_sof_swallowing_real_frame(void)
{
    /* A false header claims LEN 32 (41 bytes). A real PING follows, then filler. The parser
     * must resynchronise one byte past the false SOF and still deliver the real frame. */
    uint8_t stream[BRAIN_MAX_FRAME_BYTES] = {0xA5U, 0x01U, 0x02U, 0x01U, BRAIN_MAX_PAYLOAD_BYTES};
    brain_parser_t p;
    brain_frame_t f;
    (void)memcpy(&stream[BRAIN_HEADER_BYTES], g_frames[0].frame, g_frames[0].frame_len);
    brain_parser_reset(&p);
    CHECK(feed(&p, stream, sizeof(stream), &f) == 1U);
    CHECK((f.type == BRAIN_MSG_PING) && (f.seq == g_frames[0].seq));
}

static void test_timeout_reset_and_split(void)
{
    brain_parser_t p;
    brain_frame_t f;
    brain_parser_reset(&p);
    CHECK(feed(&p, g_frames[2].frame, 6U, &f) == 0U); /* partial */
    brain_parser_reset(&p);                           /* inter-byte timeout */
    CHECK(feed(&p, g_frames[2].frame, g_frames[2].frame_len, &f) == 1U);
    /* Without the reset, the stale partial frame would have eaten the new one's start. */
    brain_parser_reset(&p);
    CHECK(feed(&p, g_frames[2].frame, 3U, &f) == 0U);
    CHECK(feed(&p, &g_frames[2].frame[3], g_frames[2].frame_len - 3U, &f) == 1U);
}

static void test_encode_and_null_errors(void)
{
    uint8_t out[BRAIN_MAX_FRAME_BYTES];
    uint8_t big[BRAIN_MAX_PAYLOAD_BYTES + 1U] = {0};
    size_t len = 0U;
    brain_parser_t p;
    brain_frame_t f;
    bool got = false;
    CHECK(brain_frame_encode(1U, 1U, NULL, 0U, out, sizeof(out), &len) == BRAIN_OK);
    CHECK(brain_frame_encode(1U, 1U, NULL, 1U, out, sizeof(out), &len) == BRAIN_ERR_NULL);
    CHECK(brain_frame_encode(1U, 1U, big, (uint8_t)sizeof(big), out, sizeof(out), &len) ==
          BRAIN_ERR_SIZE);
    CHECK(brain_frame_encode(1U, 1U, big, 8U, out, 8U, &len) == BRAIN_ERR_SIZE);
    CHECK(brain_frame_encode(1U, 1U, big, 0U, NULL, sizeof(out), &len) == BRAIN_ERR_NULL);
    CHECK(brain_parser_push(NULL, 0U, &f, &got) == BRAIN_ERR_NULL);
    brain_parser_reset(&p);
    CHECK(brain_parser_push(&p, 0U, NULL, &got) == BRAIN_ERR_NULL);
    CHECK(brain_feature_to_q15(NAN) == 0);
    CHECK(brain_feature_to_q15(-1.0F) == 0);
    CHECK(brain_feature_to_q15(2.0F) == 32767);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)printf("usage: %s <protocol_vectors.txt>\n", argv[0]);
        return 2;
    }
    load_vectors(argv[1]);
    test_back_to_back_and_garbage();
    test_sof_inside_payload();
    test_bad_frames_then_good();
    test_false_sof_swallowing_real_frame();
    test_timeout_reset_and_split();
    test_encode_and_null_errors();
    if (g_failures != 0) {
        (void)printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    (void)printf("all checks passed (%u vector frames)\n", (unsigned)g_n_frames);
    return 0;
}
