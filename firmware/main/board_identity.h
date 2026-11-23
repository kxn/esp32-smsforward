#pragma once

#define SMSF_BOARD_DESC_MAGIC "SMSFBRD1"
#define SMSF_BOARD_LAYOUT "dual-ml307a-s3-ab1"
#define SMSF_BOARD_VARIANT "8mb-octal-psram"

typedef struct {
    char magic[8];
    char layout[32];
    char variant[32];
} smsf_board_desc_t;

extern const smsf_board_desc_t smsf_board_descriptor;
