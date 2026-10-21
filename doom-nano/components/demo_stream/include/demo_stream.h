/**
 * @file demo_stream.h
 * @brief Phase 1 demo stream protocol: laptop -> ESP32 gray frames, ESP32 -> laptop timings.
 *
 * NOT the brain protocol. The brain UART protocol is owned by the drosophila project
 * (drosophila/firmware/include/brain_protocol.h). This is only for the ViZDoom -> OLED demo.
 * Pure C, no IDF calls, host tested.
 *
 * Frame (laptop -> ESP32), little endian:
 *   "OLF1" | seq u16 | flags u8 | reserved u8 | len u16 | payload[len] | crc32 u32
 *   len must equal DS_FRAME_PAYLOAD_BYTES (128x64 gray). crc32 (IEEE, as zlib.crc32) covers
 *   everything after the magic up to the end of the payload.
 * Reply (ESP32 -> laptop), little endian, fixed DS_REPLY_BYTES:
 *   "OLS1" | seq u16 | status u8 | flags u8 | dither_us u32 | pack_us u32 | i2c_us u32 |
 *   crc_errors u32 | len_errors u32 | crc32 u32 (over everything after the magic)
 */
#ifndef DEMO_STREAM_H
#define DEMO_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define DS_MAGIC_BYTES (4U)
#define DS_FRAME_PAYLOAD_BYTES (128U * 64U) /**< One 128x64 8 bit gray frame. */
#define DS_FRAME_HEADER_BYTES (6U)          /**< seq, flags, reserved, len (after the magic). */
#define DS_CRC_BYTES (4U)
#define DS_REPLY_BYTES (32U)

#define DS_FLAG_STYLE_MASK (0x03U) /**< 0 = Bayer 4x4, 1 = Floyd-Steinberg, 2 = outline. */
#define DS_STYLE_BAYER4 (0U)
#define DS_STYLE_FLOYD_STEINBERG (1U)
#define DS_STYLE_OUTLINE (2U)
#define DS_FLAG_FAST_I2C (0x80U) /**< Request the 1 MHz (out of spec) I2C clock. */

/** Fixed magic values. */
extern const uint8_t DS_FRAME_MAGIC[DS_MAGIC_BYTES];
extern const uint8_t DS_REPLY_MAGIC[DS_MAGIC_BYTES];

/** Result of feeding one byte to the parser. */
typedef enum {
    DS_EV_NONE = 0,    /**< Byte consumed, nothing complete yet. */
    DS_EV_FRAME = 1,   /**< A valid frame is ready in the parser (see ds_parser_t). */
    DS_EV_ERR_CRC = 2, /**< Frame complete but CRC mismatch; dropped, parser resynced. */
    DS_EV_ERR_LEN = 3, /**< Header announced a wrong length; dropped, parser resynced. */
} ds_event_t;

/** Reply status codes. */
typedef enum {
    DS_STATUS_OK = 0,       /**< Frame shown. */
    DS_STATUS_HELLO = 1,    /**< Sent once at boot: device ready, all timings zero. */
    DS_STATUS_ERR_PIPE = 2, /**< Dither or pack failed. */
    DS_STATUS_ERR_I2C = 3,  /**< I2C transfer to the OLED failed. */
} ds_status_t;

/** Parser state. Large (holds one payload); place it in static storage. */
typedef struct {
    uint32_t state; /**< Internal state machine position. */
    uint32_t index; /**< Bytes received within the current state. */
    uint8_t header[DS_FRAME_HEADER_BYTES];
    uint8_t crc_bytes[DS_CRC_BYTES];
    uint32_t crc;  /**< Running CRC (pre-inversion form). */
    uint16_t seq;  /**< Valid after DS_EV_FRAME. */
    uint8_t flags; /**< Valid after DS_EV_FRAME. */
    uint32_t crc_errors;
    uint32_t len_errors;
    uint8_t payload[DS_FRAME_PAYLOAD_BYTES]; /**< Valid after DS_EV_FRAME until next push. */
} ds_parser_t;

/** Reply contents. */
typedef struct {
    uint16_t seq;
    uint8_t status; /**< ds_status_t */
    uint8_t flags;
    uint32_t dither_us;
    uint32_t pack_us;
    uint32_t i2c_us;
    uint32_t crc_errors;
    uint32_t len_errors;
} ds_reply_t;

/**
 * @brief CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), identical to zlib.crc32.
 * @param crc  Previous return value, or 0 to start.
 * @param data Bytes (may be NULL only when len is 0).
 * @param len  Number of bytes.
 * @return Updated CRC.
 */
uint32_t ds_crc32(uint32_t crc, const uint8_t *data, size_t len);

/**
 * @brief Reset the parser to look for a frame magic. Error counters are cleared.
 * @param p Parser; ignored if NULL.
 */
void ds_parser_init(ds_parser_t *p);

/**
 * @brief Feed one received byte. Resyncs on the magic after any error or garbage.
 * @param p    Parser (must not be NULL; checked, returns DS_EV_NONE if it is).
 * @param byte Received byte.
 * @return Event; on DS_EV_FRAME, p->seq, p->flags and p->payload hold the frame.
 */
ds_event_t ds_parser_push(ds_parser_t *p, uint8_t byte);

/**
 * @brief Encode a reply into exactly DS_REPLY_BYTES bytes.
 * @param r       Reply.
 * @param out     Output buffer.
 * @param out_len Size of @p out.
 * @return Number of bytes written (DS_REPLY_BYTES), or 0 on NULL or short buffer.
 */
size_t ds_encode_reply(const ds_reply_t *r, uint8_t *out, size_t out_len);

#endif /* DEMO_STREAM_H */
