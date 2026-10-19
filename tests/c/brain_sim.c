/**
 * @file brain_sim.c
 * @brief Host simulator of the brain: the real parser, server and model behind stdin/stdout.
 *
 * Lets the Python client (game/stm32_backend.py) be tested end to end without hardware.
 * The cycle counter is fake (it just counts calls), so step_cycles from the simulator are
 * meaningless; only the real board gives cycle counts.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "brain_protocol.h"
#include "brain_server.h"

#define SIM_CORE_KHZ 64000U

static uint32_t g_fake_cycles;

static uint32_t fake_cycles(void)
{
    g_fake_cycles += 100U;
    return g_fake_cycles;
}

int main(void)
{
    static brain_server_t server;
    brain_parser_t parser;
    brain_frame_t frame;
    int ch;

    if (brain_server_init(&server, SIM_CORE_KHZ, fake_cycles) != BRAIN_OK) {
        return 1;
    }
    brain_parser_reset(&parser);
    while ((ch = getchar()) != EOF) {
        bool got = false;
        if (brain_parser_push(&parser, (uint8_t)ch, &frame, &got) != BRAIN_OK) {
            return 1;
        }
        while (got) {
            uint8_t out[BRAIN_MAX_FRAME_BYTES];
            size_t out_len = 0U;
            if (brain_server_handle(&server, &frame, out, sizeof(out), &out_len) != BRAIN_OK) {
                return 1;
            }
            if (fwrite(out, 1U, out_len, stdout) != out_len) {
                return 1;
            }
            (void)fflush(stdout);
            if (brain_parser_poll(&parser, &frame, &got) != BRAIN_OK) {
                return 1;
            }
        }
    }
    return 0;
}
