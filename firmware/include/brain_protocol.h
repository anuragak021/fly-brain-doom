/**
 * @file brain_protocol.h
 * @brief Brain UART protocol v1: wire format shared by the STM32 brain, the laptop
 *        (`--backend stm32`) and the ESP32-S3 Doom game (doom-esp32).
 *
 * OWNER: drosophila. Consumers must match this file exactly and never redefine it.
 *
 * What a consumer may rely on:
 *  - This header needs only <stdint.h>, <stdbool.h> and <stddef.h>. It has no HAL, CMSIS
 *    or model dependency, so ESP-IDF can include it unchanged.
 *  - All multi-byte integers on the wire are LITTLE-ENDIAN. Floats are IEEE-754 binary32,
 *    sent as the little-endian bytes of their 32-bit pattern.
 *  - Every field is addressed by a byte-offset macro below. Do NOT overlay structs on the
 *    wire bytes (padding and alignment differ between compilers and cores).
 *  - Link behaviour (master, timeouts, error handling) is specified in the README section
 *    "Brain protocol" and summarised at the end of this file.
 *
 * Frame layout (bytes):
 *
 *   off 0   SOF      0xA5
 *   off 1   VERSION  protocol version (BRAIN_PROTOCOL_VERSION)
 *   off 2   TYPE     message type (BRAIN_MSG_*)
 *   off 3   SEQ      sequence number, chosen by the master, echoed by the brain
 *   off 4   LEN      payload length in bytes (payload only; 0..BRAIN_MAX_PAYLOAD_BYTES)
 *   off 5   PAYLOAD  LEN bytes
 *   off 5+LEN CRC32  u32 little-endian, over bytes VERSION..PAYLOAD (off 1 .. 4+LEN)
 *
 * CRC-32 is the standard zlib / IEEE 802.3 CRC, identical to secure-boot's crc32 and to
 * zlib.crc32() in Python and esp_rom_crc32_le(0, buf, len) on the ESP32:
 * reflected polynomial 0xEDB88320 (0x04C11DB7 normal), init 0xFFFFFFFF, input and output
 * reflected, final XOR 0xFFFFFFFF. Check value: CRC-32("123456789") = 0xCBF43926.
 */
#ifndef BRAIN_PROTOCOL_H
#define BRAIN_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Frame constants -------------------------------------------------------------- */

#define BRAIN_PROTOCOL_VERSION 1U
#define BRAIN_SOF 0xA5U

#define BRAIN_OFF_SOF 0U
#define BRAIN_OFF_VERSION 1U
#define BRAIN_OFF_TYPE 2U
#define BRAIN_OFF_SEQ 3U
#define BRAIN_OFF_LEN 4U
#define BRAIN_OFF_PAYLOAD 5U

#define BRAIN_HEADER_BYTES 5U /**< SOF, VERSION, TYPE, SEQ, LEN */
#define BRAIN_CRC_BYTES 4U
#define BRAIN_FRAME_OVERHEAD_BYTES (BRAIN_HEADER_BYTES + BRAIN_CRC_BYTES)
#define BRAIN_MAX_PAYLOAD_BYTES 32U
#define BRAIN_MAX_FRAME_BYTES (BRAIN_FRAME_OVERHEAD_BYTES + BRAIN_MAX_PAYLOAD_BYTES)

/** CRC-32 of the ASCII string "123456789". */
#define BRAIN_CRC32_CHECK_VALUE 0xCBF43926U

/** A receiver drops a partial frame if no byte arrives for this long (milliseconds). */
#define BRAIN_INTERBYTE_TIMEOUT_MS 20U

/* ---- Message types ---------------------------------------------------------------- */

/** Requests are sent by the master; each response type is the request type | 0x80. */
#define BRAIN_MSG_PING 0x01U  /**< request, payload empty; answered with INFO */
#define BRAIN_MSG_STEP 0x02U  /**< request, payload = features (see below) */
#define BRAIN_MSG_RESET 0x03U /**< request, payload empty */
#define BRAIN_MSG_INFO 0x81U  /**< response to PING */
#define BRAIN_MSG_STEP_RESP 0x82U
#define BRAIN_MSG_RESET_RESP 0x83U
#define BRAIN_MSG_ERROR 0xFFU /**< response to any request that was well-formed but invalid */

/* ---- Payload layouts -------------------------------------------------------------- */

/** PING request and RESET request and RESET response: empty payload (LEN = 0). */

/** INFO response payload (BRAIN_INFO_PAYLOAD_BYTES). */
#define BRAIN_INFO_PAYLOAD_BYTES 9U
#define BRAIN_INFO_OFF_PROTO_VERSION 0U  /**< u8, equals BRAIN_PROTOCOL_VERSION */
#define BRAIN_INFO_OFF_N_FEATURES 1U     /**< u8, equals BRAIN_N_FEATURES */
#define BRAIN_INFO_OFF_N_NEURONS 2U      /**< u16 LE, neurons in the model */
#define BRAIN_INFO_OFF_SUBSTEPS 4U       /**< u8, Euler substeps per STEP */
#define BRAIN_INFO_OFF_CORE_CLOCK_KHZ 5U /**< u32 LE, core clock the cycle count is in */

/** STEP request payload: BRAIN_N_FEATURES x i16 LE, Q15 (BRAIN_STEP_PAYLOAD_BYTES). */
#define BRAIN_N_FEATURES 4U
#define BRAIN_STEP_PAYLOAD_BYTES 8U /**< 4 features x 2 bytes */
#define BRAIN_FEAT_LUM_LEFT 0U      /**< payload bytes 0..1 */
#define BRAIN_FEAT_LUM_RIGHT 1U     /**< payload bytes 2..3 */
#define BRAIN_FEAT_ENEMY_LEFT 2U    /**< payload bytes 4..5 */
#define BRAIN_FEAT_ENEMY_RIGHT 3U   /**< payload bytes 6..7 */

/** STEP response payload (BRAIN_STEP_RESP_PAYLOAD_BYTES). */
#define BRAIN_STEP_RESP_PAYLOAD_BYTES 22U
#define BRAIN_RESP_OFF_ACTIONS 0U      /**< u8 action bits (BRAIN_ACTION_*) */
#define BRAIN_RESP_OFF_RESERVED 1U     /**< u8, always 0 in v1; receivers ignore it */
#define BRAIN_RESP_OFF_STEP_CYCLES 2U  /**< u32 LE, core cycles spent in the model step */
#define BRAIN_RESP_OFF_STEER_LEFT 6U   /**< f32 LE, mean activity of left steering outputs */
#define BRAIN_RESP_OFF_STEER_RIGHT 10U /**< f32 LE */
#define BRAIN_RESP_OFF_FORWARD 14U     /**< f32 LE */
#define BRAIN_RESP_OFF_BACKWARD 18U    /**< f32 LE */

/** ERROR response payload (BRAIN_ERROR_PAYLOAD_BYTES). */
#define BRAIN_ERROR_PAYLOAD_BYTES 2U
#define BRAIN_ERROR_OFF_CODE 0U         /**< u8, BRAIN_ERR_* */
#define BRAIN_ERROR_OFF_REQUEST_TYPE 1U /**< u8, TYPE of the offending request */

#define BRAIN_ERR_UNKNOWN_TYPE 1U /**< TYPE is not a request this version knows */
#define BRAIN_ERR_BAD_LENGTH 2U   /**< LEN does not match the request's payload size */
#define BRAIN_ERR_BAD_VERSION 3U  /**< VERSION differs from the brain's version */
#define BRAIN_ERR_BAD_FEATURE 4U  /**< a feature is negative (Q15 value < 0) */

/* ---- Features (STEP request) ------------------------------------------------------ */
/*
 * Four features, each a real number in 0..1, sent as Q15: q = floor(f * 32767 + 0.5),
 * clamped by the SENDER to 0..32767; real value = q / 32767. Receiver rejects q < 0 with
 * BRAIN_ERR_BAD_FEATURE. Two implementations may differ by 1 LSB; that is acceptable.
 *
 *  LUM_LEFT, LUM_RIGHT : mean brightness of the left / right half of the rendered frame,
 *      computed as the mean of an 8-bit grey image divided by 255, BEFORE any dithering
 *      to 1 bit. (The reference is ViZDoom GRAY8; palette-to-grey on other platforms is an
 *      approximation.) Left half = columns [0, W/2), right half = columns [W - W/2, W).
 *
 *  ENEMY_LEFT, ENEMY_RIGHT : one visible enemy chosen as the LARGEST on-screen bounding
 *      box (approximately the nearest). Let x be its horizontal screen position of the box
 *      centre, normalised to -1 (left edge) .. +1 (right edge) of the projected image:
 *          ENEMY_LEFT  = max(-x, 0)      ENEMY_RIGHT = max(x, 0)
 *      A sender that works from angles must use the PROJECTED position
 *          x = tan(theta) / tan(HFOV / 2),  theta = angle right of view centre,
 *      not the linear theta / (HFOV / 2). Only enemies the player can actually see count
 *      (line of sight; not behind walls). With no visible enemy both are 0.
 *      Note an enemy exactly at x = 0 also gives 0, 0.
 */

/* ---- Actions (STEP response) ------------------------------------------------------ */
/*
 * Action bits decided BY THE BRAIN (the connectome-driven part). TURN_LEFT and TURN_RIGHT
 * are mutually exclusive. Bits 2..7 are reserved and are 0 in v1 (forward and backward
 * activities are returned as numbers only). FIRE is NOT a brain bit: the game side decides
 * it with its own heuristic (README: "Simplifications").
 */
#define BRAIN_ACTION_TURN_LEFT (1U << 0)
#define BRAIN_ACTION_TURN_RIGHT (1U << 1)
#define BRAIN_ACTION_RESERVED_MASK 0xFCU

/* ---- Status codes for the C helpers ----------------------------------------------- */

typedef enum {
    BRAIN_OK = 0,
    BRAIN_ERR_NULL = 1,   /**< NULL pointer argument */
    BRAIN_ERR_SIZE = 2,   /**< buffer too small or payload too large */
    BRAIN_ERR_LENGTH = 3, /**< payload length does not match the message */
    BRAIN_ERR_FEATURE = 4 /**< a Q15 feature is negative */
} brain_status_t;

/* ---- Helpers (implemented in firmware/src/brain_protocol.c) ----------------------- */

/** A decoded, CRC-checked frame. `payload` holds `len` valid bytes. */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[BRAIN_MAX_PAYLOAD_BYTES];
} brain_frame_t;

/** Byte-at-a-time receive parser. Zero it with brain_parser_reset() before use. */
typedef struct {
    uint8_t buf[BRAIN_MAX_FRAME_BYTES];
    uint8_t count;
} brain_parser_t;

/**
 * @brief CRC-32 (zlib / IEEE) of a byte range.
 * @param[in] data  Bytes (may be NULL only if len is 0).
 * @param[in] len   Number of bytes.
 * @return The CRC-32. For "123456789" this is BRAIN_CRC32_CHECK_VALUE.
 */
uint32_t brain_crc32(const uint8_t *data, size_t len);

/**
 * @brief Build a complete frame (SOF .. CRC) into `out`.
 * @param[in]  type     Message type.
 * @param[in]  seq      Sequence number.
 * @param[in]  payload  Payload bytes (may be NULL only if len is 0).
 * @param[in]  len      Payload length, at most BRAIN_MAX_PAYLOAD_BYTES.
 * @param[out] out      Destination buffer.
 * @param[in]  cap      Capacity of `out`.
 * @param[out] out_len  Number of bytes written (BRAIN_FRAME_OVERHEAD_BYTES + len).
 * @return BRAIN_OK, BRAIN_ERR_NULL, or BRAIN_ERR_SIZE.
 */
brain_status_t brain_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len,
                                  uint8_t *out, size_t cap, size_t *out_len);

/** Reset the parser to "hunting for SOF". Call this after an inter-byte timeout too. */
void brain_parser_reset(brain_parser_t *parser);

/**
 * @brief Feed one received byte. Never blocks and never allocates.
 *
 * Bytes before an SOF are discarded. A frame with LEN > BRAIN_MAX_PAYLOAD_BYTES or a bad
 * CRC is dropped silently and the parser resynchronises by moving one byte past the false
 * SOF (not by skipping LEN bytes). After a frame is returned, call brain_parser_poll()
 * until it reports no frame, because buffered bytes may hold another complete frame.
 *
 * @param[in,out] parser     Parser state.
 * @param[in]     byte       Received byte.
 * @param[out]    frame      Filled when *got_frame is true.
 * @param[out]    got_frame  True if a complete, CRC-valid frame was produced.
 * @return BRAIN_OK or BRAIN_ERR_NULL.
 */
brain_status_t brain_parser_push(brain_parser_t *parser, uint8_t byte, brain_frame_t *frame,
                                 bool *got_frame);

/** Same as brain_parser_push() but without a new byte: drain already buffered bytes. */
brain_status_t brain_parser_poll(brain_parser_t *parser, brain_frame_t *frame, bool *got_frame);

/**
 * @brief Convert a feature in 0..1 to Q15 (sender side). Values <= 0 and NaN give 0, values
 *        >= 1 give 32767.
 */
int16_t brain_feature_to_q15(float feature);

/** @brief Convert Q15 back to a feature (q / 32767). Caller must have rejected q < 0. */
float brain_feature_from_q15(int16_t q15);

/**
 * @brief Decode a STEP request payload into features.
 * @param[in]  payload  Payload bytes.
 * @param[in]  len      Payload length.
 * @param[out] features BRAIN_N_FEATURES floats in 0..1.
 * @return BRAIN_OK, BRAIN_ERR_NULL, BRAIN_ERR_LENGTH (len != BRAIN_STEP_PAYLOAD_BYTES) or
 *         BRAIN_ERR_FEATURE (a Q15 value is negative; features is then unspecified).
 */
brain_status_t brain_step_request_decode(const uint8_t *payload, size_t len, float *features);

/** @brief Encode a STEP request payload (BRAIN_STEP_PAYLOAD_BYTES) from features in 0..1. */
brain_status_t brain_step_request_encode(const float *features, uint8_t *payload, size_t cap);

/** Decoded STEP response. */
typedef struct {
    uint8_t actions;
    uint32_t step_cycles;
    float steer_left;
    float steer_right;
    float forward;
    float backward;
} brain_step_resp_t;

/** @brief Encode a STEP response payload (BRAIN_STEP_RESP_PAYLOAD_BYTES). */
brain_status_t brain_step_resp_encode(const brain_step_resp_t *resp, uint8_t *payload, size_t cap);

/** @brief Decode a STEP response payload; BRAIN_ERR_LENGTH if len is not 22. */
brain_status_t brain_step_resp_decode(const uint8_t *payload, size_t len, brain_step_resp_t *resp);

/*
 * Link behaviour (summary; the README section "Brain protocol" is normative together with
 * this file):
 *  - The master (ESP32 or laptop) sends one request and waits for its response before
 *    sending the next. The brain never sends unsolicited frames.
 *  - The response carries the request's SEQ. The master should time out (suggested
 *    100 ms or more for STEP: the model step takes 27 ms at 64 MHz) and then count a miss; a late response with a stale SEQ is ignored.
 *  - A frame with a bad CRC or LEN > BRAIN_MAX_PAYLOAD_BYTES is dropped with no reply
 *    (its SEQ cannot be trusted). A frame with a valid CRC that is invalid in content gets
 *    a BRAIN_MSG_ERROR response echoing SEQ: unknown type, wrong LEN for the type, wrong
 *    VERSION, or a negative feature.
 *  - RESET zeroes the neuron state. The resting reference used to decode actions is a
 *    constant of the model and is unchanged by RESET.
 *  - Only ONE master may drive the model at a time; the brain does not arbitrate.
 */

#endif /* BRAIN_PROTOCOL_H */
