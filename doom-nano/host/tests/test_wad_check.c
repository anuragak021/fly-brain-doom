/** @file test_wad_check.c @brief Tests for the WAD image validator. */
#include <stdint.h>
#include <string.h>

#include "test_runner.h"
#include "wad_check.h"

#define BUF_BYTES (256U)

static uint8_t g_buf[BUF_BYTES];

static void put_i32(uint8_t *b, int32_t v)
{
    const uint32_t u = (uint32_t)v;
    for (uint32_t i = 0U; i < 4U; i++) {
        b[i] = (uint8_t)(u >> (8U * i));
    }
}

/* A 3 lump IWAD: lumps at 12..19, 20..23, an empty marker lump; directory at 24. */
static size_t build_wad(const char *magic)
{
    memset(g_buf, 0xEE, sizeof g_buf);
    memcpy(g_buf, magic, 4);
    put_i32(&g_buf[4], 3);
    put_i32(&g_buf[8], 24);
    const int32_t lumps[3][2] = {{12, 8}, {20, 4}, {0, 0}};
    for (uint32_t i = 0U; i < 3U; i++) {
        uint8_t *e = &g_buf[24U + (16U * i)];
        put_i32(&e[0], lumps[i][0]);
        put_i32(&e[4], lumps[i][1]);
        memcpy(&e[8], "LUMPNAME", 8);
    }
    return 24U + (3U * 16U); /* 72 */
}

static void test_valid_iwad_and_used_bytes(void)
{
    const size_t n = build_wad("IWAD");
    wad_info_t info;
    /* Partition is bigger than the WAD: trailing bytes are ignored, used_bytes is the WAD. */
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_OK);
    CHECK_EQ_INT(info.num_lumps, 3);
    CHECK_EQ_INT(info.dir_offset, 24);
    CHECK_EQ_INT(info.used_bytes, n);
    CHECK_EQ_INT(info.is_iwad, 1);
    CHECK_EQ_INT(wad_check(g_buf, n, &info), WAD_OK); /* exact fit */
}

static void test_pwad_accepted_and_flagged(void)
{
    (void)build_wad("PWAD");
    wad_info_t info;
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_OK);
    CHECK_EQ_INT(info.is_iwad, 0);
}

static void test_erased_flash_rejected(void)
{
    memset(g_buf, 0xFF, sizeof g_buf);
    wad_info_t info;
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_MAGIC);
}

static void test_truncated_directory_rejected(void)
{
    const size_t n = build_wad("IWAD");
    wad_info_t info;
    CHECK_EQ_INT(wad_check(g_buf, n - 1U, &info), WAD_ERR_DIRECTORY);
    CHECK_EQ_INT(wad_check(g_buf, 11U, &info), WAD_ERR_SHORT);
}

static void test_bad_counts_and_offsets(void)
{
    wad_info_t info;
    (void)build_wad("IWAD");
    put_i32(&g_buf[4], -1);
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_COUNT);
    put_i32(&g_buf[4], (int32_t)WAD_MAX_LUMPS + 1);
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_COUNT);
    (void)build_wad("IWAD");
    put_i32(&g_buf[8], -16);
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_DIRECTORY);
    /* Huge directory offset must not wrap around. */
    put_i32(&g_buf[8], INT32_MAX);
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_DIRECTORY);
}

static void test_lump_outside_buffer_rejected(void)
{
    wad_info_t info;
    (void)build_wad("IWAD");
    put_i32(&g_buf[24U + 16U + 4U], 1000); /* lump 1 size runs past the end */
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_LUMP);
    (void)build_wad("IWAD");
    put_i32(&g_buf[24U + 0U], -5); /* negative filepos */
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_LUMP);
    (void)build_wad("IWAD");
    put_i32(&g_buf[24U + 0U], INT32_MAX); /* pos + size must not wrap */
    put_i32(&g_buf[24U + 4U], INT32_MAX);
    CHECK_EQ_INT(wad_check(g_buf, sizeof g_buf, &info), WAD_ERR_LUMP);
}

static void test_null_args(void)
{
    wad_info_t info;
    CHECK_EQ_INT(wad_check(NULL, BUF_BYTES, &info), WAD_ERR_NULL);
    CHECK_EQ_INT(wad_check(g_buf, BUF_BYTES, NULL), WAD_ERR_NULL);
}

int main(void)
{
    RUN(test_valid_iwad_and_used_bytes);
    RUN(test_pwad_accepted_and_flagged);
    RUN(test_erased_flash_rejected);
    RUN(test_truncated_directory_rejected);
    RUN(test_bad_counts_and_offsets);
    RUN(test_lump_outside_buffer_rejected);
    RUN(test_null_args);
    return TEST_REPORT();
}
