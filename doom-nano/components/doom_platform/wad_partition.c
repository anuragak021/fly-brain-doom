/**
 * @file wad_partition.c
 * @brief Memory-map the WAD data partition and hand it to the engine (I_MapWadFile).
 *
 * The mapped region is read through the flash cache, so lumps are used in place: no copy of
 * the 4 MB WAD in RAM. The mapping is never released (the engine keeps lump pointers for the
 * whole run).
 */
#include <stdbool.h>
#include <stddef.h>
#include <strings.h>

#include "doom_platform.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "wad_check.h"

static const char *TAG = "wad";

static const uint8_t *s_wad;
static size_t s_wad_len;

static const char *basename_of(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if ((*p == '/') || (*p == '\\')) {
            base = p + 1;
        }
    }
    return base;
}

dp_status_t dp_wad_map(const uint8_t **data, size_t *len)
{
    if ((data == NULL) || (len == NULL)) {
        return DP_ERR_ARG;
    }
    if (s_wad == NULL) {
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, DP_WAD_PARTITION_LABEL);
        if (part == NULL) {
            ESP_LOGE(TAG, "no '%s' partition in the partition table", DP_WAD_PARTITION_LABEL);
            return DP_ERR_NO_WAD;
        }
        const void *ptr = NULL;
        esp_partition_mmap_handle_t handle;
        const esp_err_t err =
            esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mmap of %lu bytes failed: %s", (unsigned long)part->size,
                     esp_err_to_name(err));
            return DP_ERR_MMAP;
        }
        wad_info_t info;
        const wad_status_t ws = wad_check((const uint8_t *)ptr, part->size, &info);
        if ((ws != WAD_OK) || (info.is_iwad == 0)) {
            ESP_LOGE(TAG,
                     "partition does not hold a valid IWAD (wad_check %d, iwad %d). "
                     "Flash one with tools/flash_wad.py",
                     (int)ws, (ws == WAD_OK) ? info.is_iwad : -1);
            esp_partition_munmap(handle);
            return DP_ERR_BAD_WAD;
        }
        s_wad = (const uint8_t *)ptr;
        s_wad_len = info.used_bytes;
        ESP_LOGI(TAG, "IWAD mapped: %lu lumps, %u bytes used of %lu byte partition",
                 (unsigned long)info.num_lumps, (unsigned)s_wad_len, (unsigned long)part->size);
    }
    *data = s_wad;
    *len = s_wad_len;
    return DP_OK;
}

/* Called from the doom-esp32 patch in D_AddFile (components/prboom/d_main.c). Only the IWAD
 * name is served from flash; anything else falls through to the engine's own lookup, which
 * fails cleanly because there is no filesystem. */
bool I_MapWadFile(const char *name, const void **data, size_t *size)
{
    if ((name == NULL) || (data == NULL) || (size == NULL)) {
        return false;
    }
    if (strcasecmp(basename_of(name), DP_IWAD_NAME) != 0) {
        return false;
    }
    const uint8_t *wad = NULL;
    size_t len = 0U;
    if (dp_wad_map(&wad, &len) != DP_OK) {
        return false;
    }
    *data = wad;
    *size = len;
    return true;
}
