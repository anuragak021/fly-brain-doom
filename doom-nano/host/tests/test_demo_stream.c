/** @file test_demo_stream.c @brief Tests for the Phase 1 demo stream protocol parser. */
#include <stdint.h>
#include <string.h>

#include "demo_stream.h"
#include "test_runner.h"

#define FRAME_BYTES (DS_MAGIC_BYTES + DS_FRAME_HEADER_BYTES + DS_FRAME_PAYLOAD_BYTES + DS_CRC_BYTES)

static ds_parser_t g_parser;
static uint8_t g_frame[FRAME_BYTES];

static size_t build_frame(uint8_t *out, uint16_t seq, uint8_t flags, uint16_t len)
{
    memcpy(out, DS_FRAME_MAGIC, DS_MAGIC_BYTES);
    uint8_t *h = &out[DS_MAGIC_BYTES];
    h[0] = (uint8_t)(seq & 0xFFU);
    h[1] = (uint8_t)(seq >> 8U);
    h[2] = flags;
    h[3] = 0U;
    h[4] = (uint8_t)(len & 0xFFU);
    h[5] = (uint8_t)(len >> 8U);
    uint8_t *payload = &h[DS_FRAME_HEADER_BYTES];
    for (uint32_t i = 0U; i < DS_FRAME_PAYLOAD_BYTES; i++) {
        payload[i] = (uint8_t)((i * 7U) + seq);
    }
    const uint32_t crc = ds_crc32(0U, h, DS_FRAME_HEADER_BYTES + DS_FRAME_PAYLOAD_BYTES);
    uint8_t *c = &payload[DS_FRAME_PAYLOAD_BYTES];
    for (uint32_t i = 0U; i < 4U; i++) {
        c[i] = (uint8_t)(crc >> (8U * i));
    }
    return FRAME_BYTES;
}

/** Feed bytes, return the last non-NONE event and count frames. */
static ds_event_t feed(const uint8_t *data, size_t len, uint32_t *frames)
{
    ds_event_t last = DS_EV_NONE;
    for (size_t i = 0U; i < len; i++) {
        const ds_event_t ev = ds_parser_push(&g_parser, data[i]);
        if (ev != DS_EV_NONE) {
            last = ev;
        }
        if ((ev == DS_EV_FRAME) && (frames != NULL)) {
            (*frames)++;
        }
    }
    return last;
}

static void test_crc_standard_vector(void)
{
    const uint8_t v[] = "123456789";
    CHECK_EQ_INT(ds_crc32(0U, v, 9U), 0xCBF43926U);
    CHECK_EQ_INT(ds_crc32(0U, v, 0U), 0U);
    /* Incremental update equals one-shot, as zlib.crc32 chaining does. */
    CHECK_EQ_INT(ds_crc32(ds_crc32(0U, v, 4U), &v[4], 5U), 0xCBF43926U);
}

static void test_valid_frame(void)
{
    ds_parser_init(&g_parser);
    build_frame(g_frame, 0x1234U, DS_FLAG_FAST_I2C | 1U, DS_FRAME_PAYLOAD_BYTES);
    uint32_t frames = 0U;
    CHECK_EQ_INT(feed(g_frame, FRAME_BYTES, &frames), DS_EV_FRAME);
    CHECK_EQ_INT(frames, 1);
    CHECK_EQ_INT(g_parser.seq, 0x1234);
    CHECK_EQ_INT(g_parser.flags, DS_FLAG_FAST_I2C | 1U);
    CHECK(memcmp(g_parser.payload, &g_frame[DS_MAGIC_BYTES + DS_FRAME_HEADER_BYTES],
                 DS_FRAME_PAYLOAD_BYTES) == 0);
}

static void test_garbage_before_magic_and_partial_magic(void)
{
    ds_parser_init(&g_parser);
    /* Boot log text, then a false start "OLO", then "OOL..." prefix noise. */
    const uint8_t junk[] = "ESP-ROM:esp32s3 I (31) boot: OLOOLF";
    uint32_t frames = 0U;
    feed(junk, sizeof junk - 1U, &frames);
    build_frame(g_frame, 7U, 0U, DS_FRAME_PAYLOAD_BYTES);
    CHECK_EQ_INT(feed(g_frame, FRAME_BYTES, &frames), DS_EV_FRAME);
    CHECK_EQ_INT(frames, 1);
    CHECK_EQ_INT(g_parser.seq, 7);
}

static void test_bad_crc_then_recovers(void)
{
    ds_parser_init(&g_parser);
    build_frame(g_frame, 1U, 0U, DS_FRAME_PAYLOAD_BYTES);
    g_frame[100] ^= 0x01U; /* corrupt one payload bit */
    CHECK_EQ_INT(feed(g_frame, FRAME_BYTES, NULL), DS_EV_ERR_CRC);
    CHECK_EQ_INT(g_parser.crc_errors, 1);
    build_frame(g_frame, 2U, 0U, DS_FRAME_PAYLOAD_BYTES);
    uint32_t frames = 0U;
    CHECK_EQ_INT(feed(g_frame, FRAME_BYTES, &frames), DS_EV_FRAME);
    CHECK_EQ_INT(frames, 1);
    CHECK_EQ_INT(g_parser.seq, 2);
}

static void test_bad_length_rejected_without_overflow(void)
{
    ds_parser_init(&g_parser);
    build_frame(g_frame, 3U, 0U, 0xFFFFU); /* announces 65535 bytes */
    const size_t header_end = DS_MAGIC_BYTES + DS_FRAME_HEADER_BYTES;
    CHECK_EQ_INT(feed(g_frame, header_end, NULL), DS_EV_ERR_LEN);
    CHECK_EQ_INT(g_parser.len_errors, 1);
    /* The rest of that bogus frame is treated as garbage; a following good frame still parses. */
    feed(&g_frame[header_end], FRAME_BYTES - header_end, NULL);
    build_frame(g_frame, 4U, 0U, DS_FRAME_PAYLOAD_BYTES);
    uint32_t frames = 0U;
    feed(g_frame, FRAME_BYTES, &frames);
    CHECK_EQ_INT(frames, 1);
    CHECK_EQ_INT(g_parser.seq, 4);
}

static void test_truncated_frame_then_new_frame(void)
{
    ds_parser_init(&g_parser);
    build_frame(g_frame, 5U, 0U, DS_FRAME_PAYLOAD_BYTES);
    feed(g_frame, 1000U, NULL); /* host died mid-frame */
    build_frame(g_frame, 6U, 0U, DS_FRAME_PAYLOAD_BYTES);
    uint32_t frames = 0U;
    /* The new frame is swallowed as payload of the old one, which then fails CRC; the stream
     * recovers by the frame after. This is the documented cost of a length-prefixed format. */
    feed(g_frame, FRAME_BYTES, &frames);
    CHECK_EQ_INT(g_parser.crc_errors, 1);
    build_frame(g_frame, 8U, 0U, DS_FRAME_PAYLOAD_BYTES);
    feed(g_frame, FRAME_BYTES, &frames);
    CHECK(frames >= 1U);
    CHECK_EQ_INT(g_parser.seq, 8);
}

static void test_reply_encoding(void)
{
    const ds_reply_t r = {0xBEEFU, DS_STATUS_OK, 1U, 111U, 222U, 23000U, 4U, 5U};
    uint8_t out[DS_REPLY_BYTES];
    CHECK_EQ_INT(ds_encode_reply(&r, out, sizeof out), DS_REPLY_BYTES);
    CHECK(memcmp(out, DS_REPLY_MAGIC, DS_MAGIC_BYTES) == 0);
    CHECK_EQ_INT(out[4], 0xEF);
    CHECK_EQ_INT(out[5], 0xBE);
    CHECK_EQ_INT(out[16], 23000U & 0xFFU); /* i2c_us low byte at offset 4 + 12 */
    const size_t body = DS_REPLY_BYTES - DS_MAGIC_BYTES - DS_CRC_BYTES;
    const uint32_t crc = ds_crc32(0U, &out[DS_MAGIC_BYTES], body);
    const uint8_t *c = &out[DS_MAGIC_BYTES + body];
    CHECK_EQ_INT((uint32_t)c[0] | ((uint32_t)c[1] << 8U) | ((uint32_t)c[2] << 16U) |
                     ((uint32_t)c[3] << 24U),
                 crc);
    CHECK_EQ_INT(ds_encode_reply(&r, out, sizeof out - 1U), 0);
    CHECK_EQ_INT(ds_encode_reply(NULL, out, sizeof out), 0);
}

static void test_null_safety(void)
{
    ds_parser_init(NULL);
    CHECK_EQ_INT(ds_parser_push(NULL, 0x4FU), DS_EV_NONE);
    CHECK_EQ_INT(ds_crc32(0x1234U, NULL, 10U), 0x1234U);
}

int main(void)
{
    RUN(test_crc_standard_vector);
    RUN(test_valid_frame);
    RUN(test_garbage_before_magic_and_partial_magic);
    RUN(test_bad_crc_then_recovers);
    RUN(test_bad_length_rejected_without_overflow);
    RUN(test_truncated_frame_then_new_frame);
    RUN(test_reply_encoding);
    RUN(test_null_safety);
    return TEST_REPORT();
}
