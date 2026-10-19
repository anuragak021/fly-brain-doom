/**
 * @file parser_cli.c
 * @brief Test harness: read hex bytes on stdin, print one line per parsed frame.
 *
 * Output line: "<type hex> <seq> <len> <payload hex or ->". Used by the Python fuzz test to
 * compare the C parser with the Python parser byte for byte.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "brain_protocol.h"

#define HEX_PAIR_CHARS 2U

static void print_frame(const brain_frame_t *f)
{
    (void)printf("%02x %u %u ", f->type, (unsigned)f->seq, (unsigned)f->len);
    if (f->len == 0U) {
        (void)printf("-");
    }
    for (uint32_t i = 0U; i < f->len; i++) {
        (void)printf("%02x", f->payload[i]);
    }
    (void)printf("\n");
}

int main(void)
{
    brain_parser_t parser;
    brain_frame_t frame;
    char pair[HEX_PAIR_CHARS + 1U] = {0};
    int ch;
    uint32_t have = 0U;

    brain_parser_reset(&parser);
    while ((ch = getchar()) != EOF) {
        if ((ch == '\n') || (ch == ' ')) {
            continue;
        }
        pair[have] = (char)ch;
        have++;
        if (have < HEX_PAIR_CHARS) {
            continue;
        }
        have = 0U;
        bool got = false;
        if (brain_parser_push(&parser, (uint8_t)strtoul(pair, NULL, 16), &frame, &got) !=
            BRAIN_OK) {
            return 1;
        }
        while (got) {
            print_frame(&frame);
            if (brain_parser_poll(&parser, &frame, &got) != BRAIN_OK) {
                return 1;
            }
        }
    }
    return 0;
}
