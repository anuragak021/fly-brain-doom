/**
 * @file wad_check.h
 * @brief Validate a WAD image held in memory (pure C, no IDF calls, host tested).
 *
 * The WAD partition is written by a host script and may hold anything: an old WAD, a partial
 * write, or erased flash (0xFF). The engine trusts the header and directory completely, so
 * every offset and size is checked here, against the mapped size, before PrBoom sees it.
 *
 * WAD layout (little endian): "IWAD"/"PWAD" | numlumps i32 | infotableofs i32, then at
 * infotableofs a directory of numlumps entries { filepos i32, size i32, name[8] }.
 */
#ifndef WAD_CHECK_H
#define WAD_CHECK_H

#include <stddef.h>
#include <stdint.h>

#define WAD_HEADER_BYTES (12U)
#define WAD_DIR_ENTRY_BYTES (16U)
#define WAD_MAX_LUMPS (65536U) /**< Sanity bound; DOOM1.WAD has 1264, DOOM2.WAD 2919. */

/** Result of a WAD check. */
typedef enum {
    WAD_OK = 0,
    WAD_ERR_NULL = 1,      /**< NULL pointer argument. */
    WAD_ERR_SHORT = 2,     /**< Buffer smaller than the header. */
    WAD_ERR_MAGIC = 3,     /**< Not "IWAD" (or "PWAD" when allowed); erased flash lands here. */
    WAD_ERR_COUNT = 4,     /**< numlumps negative or above WAD_MAX_LUMPS. */
    WAD_ERR_DIRECTORY = 5, /**< Directory outside the buffer. */
    WAD_ERR_LUMP = 6,      /**< A lump's [filepos, filepos + size) is outside the buffer. */
} wad_status_t;

/** Facts about a valid WAD. */
typedef struct {
    uint32_t num_lumps;
    uint32_t dir_offset;
    size_t used_bytes; /**< End of the furthest header, directory or lump byte. */
    int is_iwad;       /**< 1 for "IWAD", 0 for "PWAD". */
} wad_info_t;

/**
 * @brief Check that @p data holds a complete, internally consistent WAD.
 * @param data   WAD bytes (for example a memory-mapped flash partition).
 * @param len    Number of readable bytes at @p data.
 * @param info   Output, filled only on WAD_OK.
 * @return WAD_OK or the first problem found.
 * @note Reads at most WAD_HEADER_BYTES + num_lumps * WAD_DIR_ENTRY_BYTES bytes; never reads
 *       beyond @p len.
 */
wad_status_t wad_check(const uint8_t *data, size_t len, wad_info_t *info);

#endif /* WAD_CHECK_H */
