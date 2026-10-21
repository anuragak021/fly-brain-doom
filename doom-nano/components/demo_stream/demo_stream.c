/**
 * @file demo_stream.c
 * @brief Phase 1 demo stream protocol (not the brain protocol). See demo_stream.h.
 */
#include "demo_stream.h"

const uint8_t DS_FRAME_MAGIC[DS_MAGIC_BYTES] = {'O', 'L', 'F', '1'};
const uint8_t DS_REPLY_MAGIC[DS_MAGIC_BYTES] = {'O', 'L', 'S', '1'};

#define CRC_INIT (0xFFFFFFFFU)
#define CRC_NIBBLE_MASK (0x0FU)
#define NIBBLE_BITS (4U)

/* Nibble table for the reflected IEEE polynomial: 64 bytes of flash, two lookups per byte.
 * Fast enough for 8 KB frames (about 16 k lookups) without a 1 KB table. */
static const uint32_t CRC_NIBBLE[16] = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU, 0x76DC4190U, 0x6B6B51F4U,
    0x4DB26158U, 0x5005713CU, 0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
    0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
};

/* Parser states. */
enum {
    ST_MAGIC = 0,
    ST_HEADER = 1,
    ST_PAYLOAD = 2,
    ST_CRC = 3,
};

/* Header field offsets (after the magic). */
enum {
    HDR_SEQ = 0,
    HDR_FLAGS = 2,
    HDR_LEN = 4,
};

static uint32_t crc_update_byte(uint32_t crc, uint8_t byte)
{
    uint32_t c = crc ^ byte;
    c = (c >> NIBBLE_BITS) ^ CRC_NIBBLE[c & CRC_NIBBLE_MASK];
    c = (c >> NIBBLE_BITS) ^ CRC_NIBBLE[c & CRC_NIBBLE_MASK];
    return c;
}

uint32_t ds_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return crc;
    }
    uint32_t c = crc ^ CRC_INIT;
    for (size_t i = 0U; i < len; i++) {
        c = crc_update_byte(c, data[i]);
    }
    return c ^ CRC_INIT;
}

static uint16_t get_u16(const uint8_t *b)
{
    return (uint16_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8U));
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8U) | ((uint32_t)b[2] << 16U) |
           ((uint32_t)b[3] << 24U);
}

static void put_u16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v & 0xFFU);
    b[1] = (uint8_t)(v >> 8U);
}

static void put_u32(uint8_t *b, uint32_t v)
{
    for (uint32_t i = 0U; i < 4U; i++) {
        b[i] = (uint8_t)((v >> (8U * i)) & 0xFFU);
    }
}

static void resync(ds_parser_t *p)
{
    p->state = ST_MAGIC;
    p->index = 0U;
}

void ds_parser_init(ds_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    resync(p);
    p->crc = CRC_INIT;
    p->seq = 0U;
    p->flags = 0U;
    p->crc_errors = 0U;
    p->len_errors = 0U;
}

static void push_magic(ds_parser_t *p, uint8_t byte)
{
    if (byte == DS_FRAME_MAGIC[p->index]) {
        p->index++;
    } else {
        /* The magic has no repeated prefix, so restart, counting this byte if it starts one. */
        p->index = (byte == DS_FRAME_MAGIC[0]) ? 1U : 0U;
    }
    if (p->index == DS_MAGIC_BYTES) {
        p->state = ST_HEADER;
        p->index = 0U;
        p->crc = CRC_INIT;
    }
}

static ds_event_t push_header(ds_parser_t *p, uint8_t byte)
{
    p->header[p->index] = byte;
    p->index++;
    p->crc = crc_update_byte(p->crc, byte);
    if (p->index < DS_FRAME_HEADER_BYTES) {
        return DS_EV_NONE;
    }
    if (get_u16(&p->header[HDR_LEN]) != DS_FRAME_PAYLOAD_BYTES) {
        p->len_errors++;
        resync(p);
        return DS_EV_ERR_LEN;
    }
    p->state = ST_PAYLOAD;
    p->index = 0U;
    return DS_EV_NONE;
}

static ds_event_t push_crc(ds_parser_t *p, uint8_t byte)
{
    p->crc_bytes[p->index] = byte;
    p->index++;
    if (p->index < DS_CRC_BYTES) {
        return DS_EV_NONE;
    }
    const uint32_t expected = p->crc ^ CRC_INIT;
    resync(p);
    if (get_u32(p->crc_bytes) != expected) {
        p->crc_errors++;
        return DS_EV_ERR_CRC;
    }
    p->seq = get_u16(&p->header[HDR_SEQ]);
    p->flags = p->header[HDR_FLAGS];
    return DS_EV_FRAME;
}

ds_event_t ds_parser_push(ds_parser_t *p, uint8_t byte)
{
    if (p == NULL) {
        return DS_EV_NONE;
    }
    switch (p->state) {
    case ST_MAGIC:
        push_magic(p, byte);
        return DS_EV_NONE;
    case ST_HEADER:
        return push_header(p, byte);
    case ST_PAYLOAD:
        /* index < DS_FRAME_PAYLOAD_BYTES holds: the state changes as soon as it is reached. */
        p->payload[p->index] = byte;
        p->index++;
        p->crc = crc_update_byte(p->crc, byte);
        if (p->index == DS_FRAME_PAYLOAD_BYTES) {
            p->state = ST_CRC;
            p->index = 0U;
        }
        return DS_EV_NONE;
    case ST_CRC:
        return push_crc(p, byte);
    default:
        resync(p); /* corrupted state: start over */
        return DS_EV_NONE;
    }
}

size_t ds_encode_reply(const ds_reply_t *r, uint8_t *out, size_t out_len)
{
    if ((r == NULL) || (out == NULL) || (out_len < DS_REPLY_BYTES)) {
        return 0U;
    }
    for (uint32_t i = 0U; i < DS_MAGIC_BYTES; i++) {
        out[i] = DS_REPLY_MAGIC[i];
    }
    uint8_t *b = &out[DS_MAGIC_BYTES];
    put_u16(&b[0], r->seq);
    b[2] = r->status;
    b[3] = r->flags;
    put_u32(&b[4], r->dither_us);
    put_u32(&b[8], r->pack_us);
    put_u32(&b[12], r->i2c_us);
    put_u32(&b[16], r->crc_errors);
    put_u32(&b[20], r->len_errors);
    const size_t body = DS_REPLY_BYTES - DS_MAGIC_BYTES - DS_CRC_BYTES;
    put_u32(&b[body], ds_crc32(0U, b, body));
    return DS_REPLY_BYTES;
}

_Static_assert(DS_REPLY_BYTES == DS_MAGIC_BYTES + 2U + 1U + 1U + (5U * 4U) + DS_CRC_BYTES,
               "reply layout");
_Static_assert(DS_FRAME_PAYLOAD_BYTES <= UINT16_MAX, "length field is u16");
