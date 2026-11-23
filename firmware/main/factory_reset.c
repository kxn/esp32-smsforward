#include "factory_reset.h"

esp_err_t factory_reset_apply(bool pending, const factory_reset_steps_t *steps)
{
    if (!pending) return ESP_OK;
    esp_err_t err = steps->erase_config();
    if (err == ESP_OK) err = steps->restore_wifi();
    if (err == ESP_OK) err = steps->erase_storage();
    if (err == ESP_OK) err = steps->clear_pending();
    return err;
}

#ifdef ESP_PLATFORM
#include "cfg.h"
#include "storage.h"
#include "wifi.h"

#include <stdatomic.h>
#include <stdint.h>
#include "esp_partition.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char RESET_NS[] = "smsfwd_reset";
static const char RESET_KEY[] = "pending";
static atomic_bool s_pending;

bool factory_reset_pending(void)
{
    return atomic_load(&s_pending);
}

esp_err_t factory_reset_request(void)
{
    if (factory_reset_pending()) return ESP_OK;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(RESET_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, RESET_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) atomic_store(&s_pending, true);
    return err;
}

static esp_err_t read_pending(bool *pending)
{
    *pending = false;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(RESET_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t value = 0;
    err = nvs_get_u8(nvs, RESET_KEY, &value);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err == ESP_OK) *pending = value != 0;
    return err;
}

static esp_err_t clear_pending(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(RESET_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(nvs, RESET_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) atomic_store(&s_pending, false);
    return err;
}

static esp_err_t restore_wifi(void)
{
    esp_err_t err = wifi_preinit();
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_restore();
    return err;
}

static esp_err_t erase_storage(void)
{
    static bool formatted;
    if (formatted) return ESP_OK;
    if (storage_ready()) return ESP_ERR_INVALID_STATE;
    /* This firmware's FAT, separate from the vendor FAT and from the SIMs. */
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!part) return ESP_ERR_NOT_FOUND;
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    /* Keep the journal until the freshly erased FAT mounts. An interruption
     * during formatting must be retried even if the FAT is partially written. */
    if (err == ESP_OK) err = storage_mount();
    if (err == ESP_OK) formatted = true;
    return err;
}

esp_err_t factory_reset_recover(void)
{
    bool pending;
    esp_err_t err = read_pending(&pending);
    if (err != ESP_OK) return err;
    atomic_store(&s_pending, pending);
    static const factory_reset_steps_t steps = {
        .erase_config = cfg_erase_persisted,
        .restore_wifi = restore_wifi,
        .erase_storage = erase_storage,
        .clear_pending = clear_pending,
    };
    return factory_reset_apply(pending, &steps);
}
#endif
