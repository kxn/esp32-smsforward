#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board_identity.h"
#include "firmware_update.h"
#include "idf_fake.h"

enum { IMAGE_LEN = 10000, HEADER_LEN = FIRMWARE_HEADER_SIZE };

const smsf_board_desc_t smsf_board_descriptor = {
    { 'S', 'M', 'S', 'F', 'B', 'R', 'D', '1' }, SMSF_BOARD_LAYOUT, SMSF_BOARD_VARIANT,
};
static const esp_app_desc_t app = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD, .version = "1.2.3", .project_name = "newsmsforward",
};
static const esp_partition_t ota0 = { ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x20000, FIRMWARE_IMAGE_MAX };
static const esp_partition_t ota1 = { ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x600000, FIRMWARE_IMAGE_MAX };
static const esp_partition_t *boot = &ota0;
static esp_ota_img_states_t state = ESP_OTA_IMG_VALID;
static bool no_ota_state;
static uint8_t package[HEADER_LEN + IMAGE_LEN];
static uint8_t flash[IMAGE_LEN];
static size_t flashed;
static bool ota_open;
static bool fail_write, fail_end, fail_verify, fail_select, image_has_tail;
static unsigned began, aborted, validated, verified, selected;
static unsigned rollback_called, confirmed;
static TickType_t ticks;
static void (*trial_task)(void *);
static jmp_buf restart_marker;

const esp_app_desc_t *esp_app_get_description(void) { return &app; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &ota0; }
const esp_partition_t *esp_ota_get_boot_partition(void) { return boot; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start)
{
    assert(start == NULL);
    return &ota1;
}
int esp_ota_get_app_partition_count(void) { return 2; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *part, esp_ota_img_states_t *out)
{
    assert(part == &ota0);
    if (no_ota_state) return ESP_FAIL;
    *out = state;
    return ESP_OK;
}

esp_err_t esp_ota_begin(const esp_partition_t *part, size_t size, esp_ota_handle_t *handle)
{
    assert(part == &ota1 && size == IMAGE_LEN && !ota_open);
    memset(flash, 0xff, sizeof(flash));
    flashed = 0;
    ota_open = true;
    *handle = 10;
    ++began;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    assert(ota_open && handle == 10 && flashed + size <= sizeof(flash));
    if (fail_write) return ESP_FAIL;
    memcpy(flash + flashed, data, size);
    flashed += size;
    return ESP_OK;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    assert(ota_open && handle == 10);
    ota_open = false;
    ++aborted;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(ota_open && handle == 10 && flashed == sizeof(flash));
    ota_open = false;
    ++validated;
    return fail_end ? ESP_ERR_OTA_VALIDATE_FAILED : ESP_OK;
}

esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *pos,
                           esp_image_metadata_t *meta)
{
    assert(mode == ESP_IMAGE_VERIFY_SILENT && pos->offset == ota1.address && pos->size == IMAGE_LEN);
    ++verified;
    meta->image_len = image_has_tail ? IMAGE_LEN - 1 : IMAGE_LEN;
    return fail_verify ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part)
{
    assert(!ota_open && part == &ota1);
    assert(memcmp(flash, package + HEADER_LEN, IMAGE_LEN) == 0);
    if (fail_select) return ESP_FAIL;
    ++selected;
    boot = part;
    return ESP_OK;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    assert(state == ESP_OTA_IMG_PENDING_VERIFY);
    ++confirmed;
    state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    assert(state == ESP_OTA_IMG_PENDING_VERIFY);
    ++rollback_called;
    return ESP_FAIL;
}

void esp_restart(void) { longjmp(restart_marker, 1); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "test"; }
void test_log(const char *tag, const char *message, ...) { (void)tag; (void)message; }

TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delta) { ticks += delta; }
void vTaskDelete(void *task) { assert(task == NULL); }
int xTaskCreate(void (*run)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle)
{
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    (void)handle;
    trial_task = run;
    return pdPASS;
}

/* Host stand-in for mbedTLS: captures the exact byte stream and detects change.
 * Device builds link mbedTLS SHA-256 and ESP-IDF's image validation. */
void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { ctx->hash = 0; }
void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { (void)ctx; }
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{
    assert(!is224);
    ctx->hash = 2166136261U;
    return 0;
}
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; ++i) ctx->hash = (ctx->hash ^ buf[i]) * 16777619U;
    return 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char *out)
{
    memset(out, 0, 32);
    memcpy(out, &ctx->hash, sizeof(ctx->hash));
    return 0;
}

static void make_package(void)
{
    static const esp_image_header_t image = {
        .magic = ESP_IMAGE_HEADER_MAGIC, .segment_count = 1, .chip_id = ESP_CHIP_ID_ESP32S3,
    };
    static const esp_image_segment_header_t segment = { .data_len = 9999 };
    memset(package, 0, sizeof(package));
    memcpy(package, "SMSFUPD1", 8);
    memcpy(package + 8, SMSF_BOARD_LAYOUT, sizeof(SMSF_BOARD_LAYOUT));
    package[40] = IMAGE_LEN & 0xff;
    package[41] = IMAGE_LEN >> 8;
    size_t off = HEADER_LEN;
    memcpy(package + off, &image, sizeof(image));
    off += sizeof(image);
    memcpy(package + off, &segment, sizeof(segment));
    off += sizeof(segment);
    memcpy(package + off, &app, sizeof(app));
    off += sizeof(app);
    memcpy(package + off, &smsf_board_descriptor, sizeof(smsf_board_descriptor));
    off += sizeof(smsf_board_descriptor);
    for (; off < sizeof(package); ++off) package[off] = (uint8_t)off;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, package + HEADER_LEN, IMAGE_LEN);
    mbedtls_sha256_finish(&sha, package + 44);
    mbedtls_sha256_free(&sha);
}

typedef struct { size_t pos, limit; } input_t;

static int read_package(void *ctx, void *buf, size_t len)
{
    assert(!firmware_restart_reserve());
    input_t *input = ctx;
    size_t remain = input->limit - input->pos;
    if (remain == 0) return -1;
    if (len > remain) len = remain;
    if (len > 137) len = 137;
    memcpy(buf, package + input->pos, len);
    input->pos += len;
    return (int)len;
}

static firmware_result_t upload(size_t declared, size_t available)
{
    input_t input = { .limit = available };
    char version[FIRMWARE_VERSION_SIZE];
    firmware_result_t result = firmware_upload(declared, read_package, &input, version);
    assert(!ota_open);
    if (result == FIRMWARE_OK) assert(strcmp(version, app.version) == 0);
    return result;
}

static bool lan_healthy(void)
{
    /* Two early healthy samples followed by a dropped LAN. */
    return ticks == 121000 || ticks == 122000 || ticks >= 124000;
}

int main(void)
{
    make_package();
    assert(strcmp(firmware_version(), app.version) == 0);
    assert(firmware_can_upload());
    assert(firmware_restart_reserve());
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_BUSY);
    firmware_restart_release();

    assert(upload(sizeof(package) - 1, sizeof(package) - 1) == FIRMWARE_SIZE);
    assert(upload(FIRMWARE_IMAGE_MAX + HEADER_LEN + 1, sizeof(package)) == FIRMWARE_SIZE);
    assert(upload(sizeof(package), 15) == FIRMWARE_INCOMPLETE);
    package[0] ^= 1;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_PACKAGE);
    package[0] ^= 1;
    package[76] = 1;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_PACKAGE);
    package[76] = 0;
    package[8] ^= 1;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_BOARD_MISMATCH);
    package[8] ^= 1;

    const size_t app_off = HEADER_LEN + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    const size_t board_off = app_off + sizeof(esp_app_desc_t);
    const size_t invalid_fields[] = {
        HEADER_LEN + offsetof(esp_image_header_t, chip_id),
        app_off + offsetof(esp_app_desc_t, magic_word),
        app_off + offsetof(esp_app_desc_t, project_name),
        board_off + offsetof(smsf_board_desc_t, variant),
    };
    for (size_t i = 0; i < sizeof(invalid_fields) / sizeof(invalid_fields[0]); ++i) {
        package[invalid_fields[i]] ^= 1;
        assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_IMAGE);
        package[invalid_fields[i]] ^= 1;
    }
    assert(began == 0 && selected == 0);

    assert(upload(sizeof(package), HEADER_LEN + 4500) == FIRMWARE_INCOMPLETE);
    assert(flashed > 4000 && began == 1 && aborted == 1 && validated == 0 && selected == 0);
    package[sizeof(package) - 1] ^= 1;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_CHECKSUM);
    package[sizeof(package) - 1] ^= 1;
    assert(aborted == 2 && validated == 0 && selected == 0);
    fail_write = true;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_WRITE);
    fail_write = false;
    assert(aborted == 3 && validated == 0 && selected == 0);
    fail_end = true;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_IMAGE);
    fail_end = false;
    assert(aborted == 3 && validated == 1 && verified == 0 && selected == 0);
    fail_verify = true;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_IMAGE);
    fail_verify = false;
    image_has_tail = true;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_IMAGE);
    image_has_tail = false;
    fail_select = true;
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_SELECT);
    fail_select = false;
    assert(boot == &ota0 && selected == 0 && firmware_can_upload());
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_OK);
    assert(boot == &ota1 && selected == 1);
    assert(!firmware_restart_reserve() && !firmware_can_upload());
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_BUSY);

    firmware_restart_release();
    assert(!firmware_can_upload());
    assert(upload(sizeof(package), sizeof(package)) == FIRMWARE_UNAVAILABLE);
    boot = &ota0;
    no_ota_state = true;
    firmware_boot_check_start(lan_healthy);
    assert(!firmware_can_upload() && trial_task == NULL);
    no_ota_state = false;
    firmware_boot_check_start(lan_healthy);
    assert(firmware_can_upload() && trial_task == NULL);

    state = ESP_OTA_IMG_PENDING_VERIFY;
    firmware_boot_check_start(lan_healthy);
    assert(!firmware_restart_reserve() && !firmware_can_upload());
    if (setjmp(restart_marker) == 0) {
        trial_task(NULL);
        assert(false);
    }
    assert(ticks == 120000 && confirmed == 0 && rollback_called == 1);

    firmware_boot_check_start(lan_healthy);
    firmware_boot_check_ready();
    trial_task(NULL);
    assert(ticks == 128000 && confirmed == 1 && firmware_can_upload());
    return 0;
}
