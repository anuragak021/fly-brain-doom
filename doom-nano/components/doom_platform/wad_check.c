/**
 * @file wad_check.c
 * @brief Validate a WAD image held in memory. See wad_check.h.
 */
#include "wad_check.h"

#define MAGIC_BYTES (4U)
#define HDR_OFF_NUMLUMPS (4U)
#define HDR_OFF_DIROFS (8U)
#define DIR_OFF_FILEPOS (0U)
#define DIR_OFF_SIZE (4U)

static int32_t get_i32(const uint8_t *b)
{
    const uint32_t u =
        (uint32_t)b[0] | ((uint32_t)b[1] << 8U) | ((uint32_t)b[2] << 16U) | ((uint32_t)b[3] << 24U);
    return (int32_t)u; /* two's complement on every supported target */
}

static int magic_is(const uint8_t *b, const char *m)
{
    for (uint32_t i = 0U; i < MAGIC_BYTES; i++) {
        if (b[i] != (uint8_t)m[i]) {
            return 0;
        }
    }
    return 1;
}

/** True if [off, off + n) lies inside [0, len) without overflow. */
static int range_ok(uint64_t off, uint64_t n, uint64_t len)
{
    return (off <= len) && (n <= (len - off));
}

wad_status_t wad_check(const uint8_t *data, size_t len, wad_info_t *info)
{
    if ((data == NULL) || (info == NULL)) {
        return WAD_ERR_NULL;
    }
    if (len < WAD_HEADER_BYTES) {
        return WAD_ERR_SHORT;
    }
    const int is_iwad = magic_is(data, "IWAD");
    if ((is_iwad == 0) && (magic_is(data, "PWAD") == 0)) {
        return WAD_ERR_MAGIC;
    }
    const int32_t num = get_i32(&data[HDR_OFF_NUMLUMPS]);
    const int32_t dir = get_i32(&data[HDR_OFF_DIROFS]);
    if ((num < 0) || ((uint32_t)num > WAD_MAX_LUMPS)) {
        return WAD_ERR_COUNT;
    }
    const uint64_t dir_bytes = (uint64_t)(uint32_t)num * WAD_DIR_ENTRY_BYTES;
    if ((dir < 0) || !range_ok((uint64_t)(uint32_t)dir, dir_bytes, len)) {
        return WAD_ERR_DIRECTORY;
    }
    uint64_t used = (uint64_t)(uint32_t)dir + dir_bytes;
    for (uint32_t i = 0U; i < (uint32_t)num; i++) {
        const uint8_t *e = &data[(size_t)(uint32_t)dir + ((size_t)i * WAD_DIR_ENTRY_BYTES)];
        const int32_t pos = get_i32(&e[DIR_OFF_FILEPOS]);
        const int32_t size = get_i32(&e[DIR_OFF_SIZE]);
        if ((pos < 0) || (size < 0) ||
            !range_ok((uint64_t)(uint32_t)pos, (uint64_t)(uint32_t)size, len)) {
            return WAD_ERR_LUMP;
        }
        const uint64_t end = (uint64_t)(uint32_t)pos + (uint32_t)size;
        if (end > used) {
            used = end;
        }
    }
    info->num_lumps = (uint32_t)num;
    info->dir_offset = (uint32_t)dir;
    info->used_bytes = (size_t)used; /* used <= len, so it fits in size_t */
    info->is_iwad = is_iwad;
    return WAD_OK;
}
