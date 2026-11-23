#include "board.h"

_Static_assert(sizeof(SMSF_BOARD_LAYOUT) <= 32, "Update board/layout ID must be NUL terminated");
_Static_assert(sizeof(SMSF_BOARD_VARIANT) <= 32, "Update board variant must be NUL terminated");

/* ESP-IDF places this descriptor immediately after esp_app_desc in the image. */
const smsf_board_desc_t smsf_board_descriptor
    __attribute__((section(".rodata_custom_desc"))) = {
        .magic = SMSF_BOARD_DESC_MAGIC,
        .layout = SMSF_BOARD_LAYOUT,
        .variant = SMSF_BOARD_VARIANT,
    };
