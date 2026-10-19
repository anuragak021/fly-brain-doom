/**
 * @file brain_link.c
 * @brief Link service: parse requests from one port and answer them. See brain_link.h.
 */
#include "brain_link.h"

#include <stddef.h>

brain_status_t brain_link_init(brain_link_t *link, brain_read_fn_t read, brain_write_fn_t write,
                               void *ctx)
{
    if ((link == NULL) || (read == NULL) || (write == NULL)) {
        return BRAIN_ERR_NULL;
    }
    brain_parser_reset(&link->parser);
    link->read = read;
    link->write = write;
    link->ctx = ctx;
    link->last_rx_ms = 0U;
    link->frames_served = 0U;
    link->steps_served = 0U;
    link->write_failures = 0U;
    return BRAIN_OK;
}

/** Answer one request; false if the reply could not be built or sent. */
static bool answer(brain_link_t *link, brain_server_t *server, const brain_frame_t *req)
{
    uint8_t out[BRAIN_MAX_FRAME_BYTES];
    size_t out_len = 0U;
    if (brain_server_handle(server, req, out, sizeof(out), &out_len) != BRAIN_OK) {
        return false;
    }
    if (!link->write(link->ctx, out, out_len)) {
        link->write_failures++;
        return false;
    }
    link->frames_served++;
    if (req->type == BRAIN_MSG_STEP) {
        link->steps_served++;
    }
    return true;
}

brain_status_t brain_link_service(brain_link_t *link, brain_server_t *server, uint32_t now_ms,
                                  brain_link_result_t *result)
{
    if ((link == NULL) || (server == NULL)) {
        return BRAIN_ERR_NULL;
    }
    brain_link_result_t res = {0U, 0U, false};
    uint8_t byte;
    for (uint32_t n = 0U; (n < BRAIN_LINK_MAX_BYTES_PER_CALL) && link->read(link->ctx, &byte);
         n++) {
        link->last_rx_ms = now_ms;
        brain_frame_t frame;
        bool got = false;
        if (brain_parser_push(&link->parser, byte, &frame, &got) != BRAIN_OK) {
            continue;
        }
        while (got) {
            if (answer(link, server, &frame)) {
                res.frames++;
                res.steps += (frame.type == BRAIN_MSG_STEP) ? 1U : 0U;
            } else {
                res.failed = true;
            }
            if (brain_parser_poll(&link->parser, &frame, &got) != BRAIN_OK) {
                break;
            }
        }
    }
    if ((link->parser.count != 0U) && ((now_ms - link->last_rx_ms) > BRAIN_INTERBYTE_TIMEOUT_MS)) {
        brain_parser_reset(&link->parser); /* partial frame went stale */
    }
    if (result != NULL) {
        *result = res;
    }
    return BRAIN_OK;
}
