/**
 * @file test_brain_link.c
 * @brief Host test: two links share one server; replies return on the right link, bytes never
 *        cross, a noisy link does not disturb the other, a stale partial frame is dropped.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "brain_link.h"

#define QUEUE_BYTES 512U
#define FAKE_CLOCK_KHZ 64000U

static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/** A fake port: bytes to receive and bytes that were sent. */
typedef struct {
    uint8_t rx[QUEUE_BYTES];
    size_t rx_len;
    size_t rx_pos;
    uint8_t tx[QUEUE_BYTES];
    size_t tx_len;
    bool tx_fail;
} fake_port_t;

static bool fake_read(void *ctx, uint8_t *byte)
{
    fake_port_t *p = (fake_port_t *)ctx;
    if (p->rx_pos >= p->rx_len) {
        return false;
    }
    *byte = p->rx[p->rx_pos++];
    return true;
}

static bool fake_write(void *ctx, const uint8_t *data, size_t len)
{
    fake_port_t *p = (fake_port_t *)ctx;
    if (p->tx_fail || ((p->tx_len + len) > sizeof(p->tx))) {
        return false;
    }
    (void)memcpy(&p->tx[p->tx_len], data, len);
    p->tx_len += len;
    return true;
}

static uint32_t fake_cycles(void)
{
    static uint32_t c;
    c += 1000U;
    return c;
}

static void feed(fake_port_t *p, uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
    size_t n = 0U;
    CHECK(brain_frame_encode(type, seq, payload, len, &p->rx[p->rx_len], sizeof(p->rx) - p->rx_len,
                             &n) == BRAIN_OK);
    p->rx_len += n;
}

/** Parse the first frame of a port's output. */
static brain_frame_t first_reply(const fake_port_t *p)
{
    brain_parser_t parser;
    brain_frame_t f;
    bool got = false;
    (void)memset(&f, 0, sizeof(f));
    brain_parser_reset(&parser);
    for (size_t i = 0U; (i < p->tx_len) && !got; i++) {
        CHECK(brain_parser_push(&parser, p->tx[i], &f, &got) == BRAIN_OK);
    }
    CHECK(got);
    return f;
}

static void test_routing_and_isolation(void)
{
    static brain_server_t srv;
    static fake_port_t a, b;
    brain_link_t la, lb;
    brain_link_result_t res;
    uint8_t step[BRAIN_STEP_PAYLOAD_BYTES];
    const brain_step_req_t req = {
        .features = {0.5F, 0.5F, 0.0F, 0.9F, 0.0F, 0.0F}, .reward = 0.0F, .flags = 0U};
    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    CHECK(brain_server_init(&srv, FAKE_CLOCK_KHZ, fake_cycles) == BRAIN_OK);
    CHECK(brain_link_init(&la, fake_read, fake_write, &a) == BRAIN_OK);
    CHECK(brain_link_init(&lb, fake_read, fake_write, &b) == BRAIN_OK);
    CHECK(brain_step_request_encode(&req, step, sizeof(step)) == BRAIN_OK);

    feed(&a, BRAIN_MSG_PING, 11U, NULL, 0U);
    feed(&b, BRAIN_MSG_STEP, 22U, step, BRAIN_STEP_PAYLOAD_BYTES);
    CHECK(brain_link_service(&la, &srv, 0U, &res) == BRAIN_OK);
    CHECK((res.frames == 1U) && (res.steps == 0U) && !res.failed);
    CHECK(b.tx_len == 0U); /* b has not been serviced yet */
    CHECK(brain_link_service(&lb, &srv, 0U, &res) == BRAIN_OK);
    CHECK((res.frames == 1U) && (res.steps == 1U));

    const brain_frame_t ra = first_reply(&a);
    const brain_frame_t rb = first_reply(&b);
    CHECK((ra.type == BRAIN_MSG_INFO) && (ra.seq == 11U));
    CHECK((rb.type == BRAIN_MSG_STEP_RESP) && (rb.seq == 22U));
    CHECK((la.steps_served == 0U) && (lb.steps_served == 1U));
}

static void test_noise_on_one_link_does_not_hurt_the_other(void)
{
    static brain_server_t srv;
    static fake_port_t a, b;
    brain_link_t la, lb;
    brain_link_result_t res;
    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    CHECK(brain_server_init(&srv, FAKE_CLOCK_KHZ, fake_cycles) == BRAIN_OK);
    CHECK(brain_link_init(&la, fake_read, fake_write, &a) == BRAIN_OK);
    CHECK(brain_link_init(&lb, fake_read, fake_write, &b) == BRAIN_OK);
    for (size_t i = 0U; i < 100U; i++) {
        a.rx[i] = (uint8_t)((i * 37U) + 5U); /* garbage that may contain SOF bytes */
    }
    a.rx_len = 100U;
    feed(&b, BRAIN_MSG_PING, 5U, NULL, 0U);
    CHECK(brain_link_service(&la, &srv, 0U, &res) == BRAIN_OK);
    CHECK(brain_link_service(&lb, &srv, 0U, &res) == BRAIN_OK);
    CHECK(res.frames == 1U);
    CHECK(first_reply(&b).seq == 5U);
    CHECK(a.tx_len == 0U); /* no CRC-valid frame on a, so no reply (silent drop) */
}

static void test_stale_partial_frame_is_dropped(void)
{
    static brain_server_t srv;
    static fake_port_t a;
    brain_link_t la;
    brain_link_result_t res;
    (void)memset(&a, 0, sizeof(a));
    CHECK(brain_server_init(&srv, FAKE_CLOCK_KHZ, fake_cycles) == BRAIN_OK);
    CHECK(brain_link_init(&la, fake_read, fake_write, &a) == BRAIN_OK);
    feed(&a, BRAIN_MSG_PING, 1U, NULL, 0U);
    const size_t full = a.rx_len;
    a.rx_len = full - 3U; /* last three CRC bytes never arrive */
    CHECK(brain_link_service(&la, &srv, 0U, &res) == BRAIN_OK);
    CHECK((res.frames == 0U) && (la.parser.count != 0U));
    CHECK(brain_link_service(&la, &srv, BRAIN_INTERBYTE_TIMEOUT_MS + 1U, &res) == BRAIN_OK);
    CHECK(la.parser.count == 0U);
    /* a fresh frame after the timeout is served normally */
    a.rx_len = 0U;
    a.rx_pos = 0U;
    feed(&a, BRAIN_MSG_PING, 2U, NULL, 0U);
    CHECK(brain_link_service(&la, &srv, BRAIN_INTERBYTE_TIMEOUT_MS + 2U, &res) == BRAIN_OK);
    CHECK(res.frames == 1U);
}

static void test_write_failure_is_reported(void)
{
    static brain_server_t srv;
    static fake_port_t a;
    brain_link_t la;
    brain_link_result_t res;
    (void)memset(&a, 0, sizeof(a));
    a.tx_fail = true;
    CHECK(brain_server_init(&srv, FAKE_CLOCK_KHZ, fake_cycles) == BRAIN_OK);
    CHECK(brain_link_init(&la, fake_read, fake_write, &a) == BRAIN_OK);
    feed(&a, BRAIN_MSG_PING, 1U, NULL, 0U);
    CHECK(brain_link_service(&la, &srv, 0U, &res) == BRAIN_OK);
    CHECK(res.failed && (res.frames == 0U) && (la.write_failures == 1U));
}

static void test_null_arguments(void)
{
    static brain_server_t srv;
    brain_link_t la;
    CHECK(brain_link_init(NULL, fake_read, fake_write, NULL) == BRAIN_ERR_NULL);
    CHECK(brain_link_init(&la, NULL, fake_write, NULL) == BRAIN_ERR_NULL);
    CHECK(brain_link_service(NULL, &srv, 0U, NULL) == BRAIN_ERR_NULL);
}

int main(void)
{
    test_routing_and_isolation();
    test_noise_on_one_link_does_not_hurt_the_other();
    test_stale_partial_frame_is_dropped();
    test_write_failure_is_reported();
    test_null_arguments();
    if (g_failures == 0) {
        (void)printf("brain_link: all tests passed\n");
        return 0;
    }
    (void)printf("brain_link: %d failure(s)\n", g_failures);
    return 1;
}
