#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_OTA_VALIDATE_FAILED 1

#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 0x11
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_IMAGE_MAX_SEGMENTS 16
#define ESP_CHIP_ID_ESP32S3 9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432

typedef struct {
    uint8_t magic;
    uint8_t segment_count;
    uint8_t reserved1[10];
    uint16_t chip_id;
    uint8_t reserved2[10];
} esp_image_header_t;

typedef struct {
    uint32_t load_addr;
    uint32_t data_len;
} esp_image_segment_header_t;

typedef struct {
    uint32_t magic_word;
    uint8_t reserved1[12];
    char version[32];
    char project_name[32];
    uint8_t reserved2[176];
} esp_app_desc_t;

typedef struct {
    uint8_t subtype;
    uint32_t address;
    uint32_t size;
} esp_partition_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

typedef struct { uint32_t image_len; } esp_image_metadata_t;

typedef int esp_ota_handle_t;
typedef enum { ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY } esp_ota_img_states_t;
typedef enum { ESP_IMAGE_VERIFY_SILENT } esp_image_load_mode_t;

const esp_app_desc_t *esp_app_get_description(void);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start);
int esp_ota_get_app_partition_count(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *part, esp_ota_img_states_t *state);
esp_err_t esp_ota_begin(const esp_partition_t *part, size_t size, esp_ota_handle_t *handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part);
esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *pos,
                           esp_image_metadata_t *meta);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
void esp_restart(void);
const char *esp_err_to_name(esp_err_t err);
void test_log(const char *tag, const char *message, ...);
#define ESP_LOGE test_log
#define ESP_LOGI test_log

typedef unsigned TickType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
int xTaskCreate(void (*run)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle);

typedef struct { uint32_t hash; } mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *buf, size_t len);
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *out);
