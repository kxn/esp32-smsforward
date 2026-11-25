#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Mount point and partition label of the FAT volume.  esp_vfs_fat_info() wants
 * the mount point; the mount call wants the partition label. */
#define STORAGE_MOUNT_POINT "/data"
#define STORAGE_PARTITION_LABEL "storage"

esp_err_t storage_mount(void);
bool storage_ready(void);
