#include "storage.h"

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "storage";
static bool s_ready;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

static esp_err_t partition_is_erased(const esp_partition_t *p, bool *erased)
{
    uint8_t buf[4096];
    *erased = true;
    for (size_t off = 0; off < p->size; off += sizeof(buf)) {
        size_t n = p->size - off;
        if (n > sizeof(buf)) n = sizeof(buf);
        esp_err_t e = esp_partition_read(p, off, buf, n);
        if (e != ESP_OK) return e;
        for (size_t i = 0; i < n; i++) if (buf[i] != 0xFF) { *erased = false; return ESP_OK; }
    }
    return ESP_OK;
}

esp_err_t storage_mount(void)
{
    if (s_ready) return ESP_OK;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!p) return ESP_ERR_NOT_FOUND;
    bool erased = false;
    esp_err_t e = partition_is_erased(p, &erased);
    if (e != ESP_OK) return e;
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = erased,
        .max_files = 16,
        .allocation_unit_size = 4096,
    };
    e = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_MOUNT_POINT, STORAGE_PARTITION_LABEL, &cfg, &s_wl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "FAT mount failed (%s); filesystem left untouched", esp_err_to_name(e));
        return e;
    }
    s_ready = true;
    ESP_LOGI(TAG, "FAT mounted at /data (initial format=%s)", erased ? "yes" : "no");
    return ESP_OK;
}

bool storage_ready(void) { return s_ready; }
