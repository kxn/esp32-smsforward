#include "firmware_update.h"

#include "board_identity.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

enum {
    FIRMWARE_IDLE,
    FIRMWARE_UPLOADING,
    FIRMWARE_RESTARTING,
    FIRMWARE_TRIAL_BOOT,
};

#define FIRMWARE_CHUNK_SIZE 4096U
#define FIRMWARE_APP_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define FIRMWARE_PREFIX_SIZE (FIRMWARE_APP_OFFSET + sizeof(esp_app_desc_t) + sizeof(smsf_board_desc_t))
_Static_assert(FIRMWARE_PREFIX_SIZE <= FIRMWARE_CHUNK_SIZE, "Image prefix exceeds upload buffer");
_Static_assert(sizeof(((esp_app_desc_t *)0)->version) == FIRMWARE_VERSION_SIZE, "Unexpected ESP app version length");

static const char *TAG = "firmware";
static const char PROJECT[] = "newsmsforward";
static atomic_int s_operation;
static atomic_bool s_trial_ready;
static bool (*s_lan_healthy)(void);

static bool claim(int operation)
{
    int idle = FIRMWARE_IDLE;
    return atomic_compare_exchange_strong(&s_operation, &idle, operation);
}

bool firmware_restart_reserve(void)
{
    return claim(FIRMWARE_RESTARTING);
}

void firmware_restart_release(void)
{
    atomic_store(&s_operation, FIRMWARE_IDLE);
}

const char *firmware_version(void)
{
    return esp_app_get_description()->version;
}

static const esp_partition_t *upgrade_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!running || !boot || boot->address != running->address || !slot ||
        running->address == slot->address || running->size != FIRMWARE_IMAGE_MAX ||
        slot->size != FIRMWARE_IMAGE_MAX || esp_ota_get_app_partition_count() != 2)
        return NULL;
    bool running_ota0 = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
                        running->address == 0x20000 &&
                        slot->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
                        slot->address == 0x600000;
    bool running_ota1 = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
                        running->address == 0x600000 &&
                        slot->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
                        slot->address == 0x20000;
    if (!running_ota0 && !running_ota1) return NULL;
    esp_ota_img_states_t state;
    /* The installed slot must already be a bootable rollback target.
     * ESP-IDF seeds VALID when starting from empty otadata with no factory app. */
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_VALID)
        return NULL;
    return slot;
}

bool firmware_can_upload(void)
{
    return atomic_load(&s_operation) == FIRMWARE_IDLE && upgrade_slot() != NULL;
}

static bool read_exact(firmware_read_fn read, void *ctx, uint8_t *out, size_t len)
{
    while (len) {
        int n = read(ctx, out, len);
        if (n <= 0 || (size_t)n > len) return false;
        out += n;
        len -= (size_t)n;
    }
    return true;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool image_prefix_valid(const uint8_t *prefix, char version[FIRMWARE_VERSION_SIZE])
{
    esp_image_header_t image;
    esp_image_segment_header_t segment;
    esp_app_desc_t desc;
    memcpy(&image, prefix, sizeof(image));
    memcpy(&segment, prefix + sizeof(image), sizeof(segment));
    memcpy(&desc, prefix + FIRMWARE_APP_OFFSET, sizeof(desc));
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != ESP_CHIP_ID_ESP32S3 ||
        !image.segment_count || image.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
        segment.data_len < sizeof(desc) + sizeof(smsf_board_desc_t) || desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(desc.project_name, PROJECT, sizeof(PROJECT)))
        return false;

    if (memcmp(prefix + FIRMWARE_APP_OFFSET + sizeof(desc), &smsf_board_descriptor,
               sizeof(smsf_board_descriptor)))
        return false;

    size_t version_len = strnlen(desc.version, sizeof(desc.version));
    if (!version_len || version_len == sizeof(desc.version)) return false;
    for (size_t i = 0; i < version_len; ++i)
        if ((unsigned char)desc.version[i] < 0x20 || (unsigned char)desc.version[i] > 0x7e) return false;
    memcpy(version, desc.version, version_len + 1);
    return true;
}

static firmware_result_t write_image(const esp_partition_t *slot, size_t image_len,
                                     const uint8_t expected_digest[32], uint8_t *buf,
                                     firmware_read_fn read, void *ctx)
{
    esp_ota_handle_t ota;
    if (esp_ota_begin(slot, image_len, &ota) != ESP_OK) return FIRMWARE_WRITE;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    firmware_result_t result = FIRMWARE_WRITE;
    size_t n = FIRMWARE_PREFIX_SIZE;
    size_t remaining = image_len;
    uint8_t digest[32];
    if (mbedtls_sha256_starts(&sha, 0) != 0) goto fail;

    while (remaining) {
        if (mbedtls_sha256_update(&sha, buf, n) != 0 || esp_ota_write(ota, buf, n) != ESP_OK)
            goto fail;
        remaining -= n;
        n = remaining < FIRMWARE_CHUNK_SIZE ? remaining : FIRMWARE_CHUNK_SIZE;
        if (n && !read_exact(read, ctx, buf, n)) {
            result = FIRMWARE_INCOMPLETE;
            goto fail;
        }
    }

    if (mbedtls_sha256_finish(&sha, digest) != 0) goto fail;
    if (memcmp(expected_digest, digest, sizeof(digest))) {
        result = FIRMWARE_CHECKSUM;
        goto fail;
    }

    mbedtls_sha256_free(&sha);
    /* ESP-IDF consumes the handle even on validation failure. */
    esp_err_t err = esp_ota_end(ota);
    if (err != ESP_OK) return err == ESP_ERR_OTA_VALIDATE_FAILED ? FIRMWARE_IMAGE : FIRMWARE_WRITE;
    /* Bound the verified image to the exact package payload and reject trailers.
     * esp_ota_end() alone verifies against the whole slot size. */
    const esp_partition_pos_t pos = { .offset = slot->address, .size = image_len };
    esp_image_metadata_t meta;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK ||
        meta.image_len != image_len)
        return FIRMWARE_IMAGE;

    return esp_ota_set_boot_partition(slot) == ESP_OK ? FIRMWARE_OK : FIRMWARE_SELECT;

fail:
    mbedtls_sha256_free(&sha);
    esp_ota_abort(ota);
    return result;
}

static firmware_result_t receive_package(const esp_partition_t *slot, size_t content_len,
                                          firmware_read_fn read, void *ctx,
                                          char version[FIRMWARE_VERSION_SIZE])
{
    uint8_t header[FIRMWARE_HEADER_SIZE];
    static const char board[32] = SMSF_BOARD_LAYOUT;
    if (!read_exact(read, ctx, header, sizeof(header))) return FIRMWARE_INCOMPLETE;
    if (memcmp(header, "SMSFUPD1", 8) || read_le32(header + 76) != 0) return FIRMWARE_PACKAGE;
    if (memcmp(header + 8, board, sizeof(board))) return FIRMWARE_BOARD_MISMATCH;
    size_t image_len = read_le32(header + 40);
    if (image_len < FIRMWARE_PREFIX_SIZE || image_len > slot->size ||
        image_len != content_len - sizeof(header))
        return FIRMWARE_SIZE;

    uint8_t *buf = malloc(FIRMWARE_CHUNK_SIZE);
    if (!buf) return FIRMWARE_MEMORY;
    firmware_result_t result;
    if (!read_exact(read, ctx, buf, FIRMWARE_PREFIX_SIZE)) result = FIRMWARE_INCOMPLETE;
    else if (!image_prefix_valid(buf, version)) result = FIRMWARE_IMAGE;
    else result = write_image(slot, image_len, header + 44, buf, read, ctx);
    free(buf);
    return result;
}

firmware_result_t firmware_upload(size_t content_len, firmware_read_fn read,
                                  void *ctx, char version[FIRMWARE_VERSION_SIZE])
{
    if (content_len < FIRMWARE_HEADER_SIZE + FIRMWARE_PREFIX_SIZE ||
        content_len > FIRMWARE_HEADER_SIZE + FIRMWARE_IMAGE_MAX)
        return FIRMWARE_SIZE;
    if (!claim(FIRMWARE_UPLOADING)) return FIRMWARE_BUSY;
    const esp_partition_t *slot = upgrade_slot();
    firmware_result_t result = slot ? receive_package(slot, content_len, read, ctx, version)
                                    : FIRMWARE_UNAVAILABLE;
    atomic_store(&s_operation, result == FIRMWARE_OK ? FIRMWARE_RESTARTING : FIRMWARE_IDLE);
    return result;
}

static void rollback_trial(void)
{
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* A restart while PENDING_VERIFY also lets the bootloader try the old slot. */
    ESP_LOGE(TAG, "trial firmware failed startup (%s)", esp_err_to_name(err));
    esp_restart();
}

static void check_trial(void *arg)
{
    (void)arg;
    TickType_t started = xTaskGetTickCount();
    unsigned stable_ticks = 0;
    while (xTaskGetTickCount() - started < pdMS_TO_TICKS(120000)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!atomic_load(&s_trial_ready) || !s_lan_healthy()) {
            stable_ticks = 0;
            continue;
        }
        if (++stable_ticks < 5) continue;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) break;
        ESP_LOGI(TAG, "trial firmware confirmed");
        atomic_store(&s_operation, FIRMWARE_IDLE);
        vTaskDelete(NULL);
        return;
    }
    rollback_trial();
}

void firmware_boot_check_start(bool (*lan_healthy)(void))
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY)
        return;
    s_lan_healthy = lan_healthy;
    atomic_store(&s_operation, FIRMWARE_TRIAL_BOOT);
    if (xTaskCreate(check_trial, "fw_trial", 4096, NULL, 5, NULL) != pdPASS)
        rollback_trial();
}

void firmware_boot_check_ready(void)
{
    atomic_store(&s_trial_ready, true);
}
