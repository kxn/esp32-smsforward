#pragma once

#include <stdbool.h>
#include <stddef.h>

#define FIRMWARE_HEADER_SIZE 80U
#define FIRMWARE_IMAGE_MAX 0x1e0000U
#define FIRMWARE_VERSION_SIZE 32U

typedef enum {
    FIRMWARE_OK,
    FIRMWARE_BUSY,
    FIRMWARE_UNAVAILABLE,
    FIRMWARE_SIZE,
    FIRMWARE_PACKAGE,
    FIRMWARE_BOARD_MISMATCH,
    FIRMWARE_IMAGE,
    FIRMWARE_CHECKSUM,
    FIRMWARE_INCOMPLETE,
    FIRMWARE_WRITE,
    FIRMWARE_SELECT,
    FIRMWARE_MEMORY,
} firmware_result_t;

typedef int (*firmware_read_fn)(void *ctx, void *buf, size_t len);

const char *firmware_version(void);
bool firmware_can_upload(void);
/* Success reserves the restart until the HTTP handler has sent its reply. */
firmware_result_t firmware_upload(size_t content_len, firmware_read_fn read,
                                  void *ctx, char version[FIRMWARE_VERSION_SIZE]);

bool firmware_restart_reserve(void);
void firmware_restart_release(void);
void firmware_boot_check_start(bool (*lan_healthy)(void));
void firmware_boot_check_ready(void);
