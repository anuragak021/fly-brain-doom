/**
 * @file brain_link.h
 * @brief One serial link to a protocol master: byte source, frame parser, reply sink. Pure
 *        logic, no HAL (the byte source and sink are injected).
 *
 * The firmware serves two links (laptop on the ST-Link VCP, ESP32 on USART6) with one shared
 * neuron state. Each link has its own parser, so bytes of one never mix with the other, and each
 * reply goes back on the link its request came from. Only one master should be stepping at a
 * time: STEP requests from both would interleave on the same neuron state.
 */
#ifndef BRAIN_LINK_H
#define BRAIN_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brain_protocol.h"
#include "brain_server.h"

/** Pops one received byte if available; returns false when none. */
typedef bool (*brain_read_fn_t)(void *ctx, uint8_t *byte);
/** Sends a complete frame; returns false on a transmit failure. */
typedef bool (*brain_write_fn_t)(void *ctx, const uint8_t *data, size_t len);

/** Link state. Initialise with brain_link_init(). */
typedef struct {
    brain_parser_t parser;
    brain_read_fn_t read;
    brain_write_fn_t write;
    void *ctx;
    uint32_t last_rx_ms;
    uint32_t frames_served;
    uint32_t steps_served;
    uint32_t write_failures;
} brain_link_t;

/** What one brain_link_service() call did. */
typedef struct {
    uint32_t frames; /**< Requests answered. */
    uint32_t steps;  /**< Of which STEP requests. */
    bool failed;     /**< A reply could not be built or sent. */
} brain_link_result_t;

/**
 * @brief Initialise a link.
 * @return BRAIN_OK or BRAIN_ERR_NULL (any of link, read, write NULL).
 */
brain_status_t brain_link_init(brain_link_t *link, brain_read_fn_t read, brain_write_fn_t write,
                               void *ctx);

/**
 * @brief Drain the link's received bytes, answer every complete request, and drop a partial
 *        frame that has been idle longer than BRAIN_INTERBYTE_TIMEOUT_MS.
 *
 * The number of bytes handled per call is bounded by BRAIN_LINK_MAX_BYTES_PER_CALL so one noisy
 * link cannot starve the other.
 *
 * @param[in,out] link    Link.
 * @param[in,out] server  Shared server (neuron state).
 * @param[in]     now_ms  Millisecond clock.
 * @param[out]    result  What happened; ignored if NULL.
 * @return BRAIN_OK or BRAIN_ERR_NULL.
 */
brain_status_t brain_link_service(brain_link_t *link, brain_server_t *server, uint32_t now_ms,
                                  brain_link_result_t *result);

/** Upper bound of bytes consumed per brain_link_service() call. */
#define BRAIN_LINK_MAX_BYTES_PER_CALL 128U

#endif /* BRAIN_LINK_H */
