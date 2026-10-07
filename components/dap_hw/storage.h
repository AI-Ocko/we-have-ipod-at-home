#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define STORAGE_MOUNT_POINT "/sdcard"

// Mount the SD card's FAT filesystem at STORAGE_MOUNT_POINT over 1-bit SDMMC.
// Safe to call when already mounted. Returns the driver's error if there is
// no card or it cannot be read; never formats the card. A failed call leaves
// nothing behind and logs nothing of its own (the IDF logs its errors), so it
// can be called again and again until a card appears.
esp_err_t storage_mount(void);

// Unmount the card. Does nothing if it is not mounted.
void storage_unmount(void);

bool storage_is_mounted(void);

// The card's size and its free space, in bytes, from the FAT
// (esp_vfs_fat_info). ESP_ERR_INVALID_STATE if no card is mounted, or the
// driver's error. It reads the card: FatFs keeps the free count once it
// knows it (from the card's FSInfo sector, or from one pass over the FAT
// the first time if that sector cannot be trusted), so it is quick after
// the first call but may not be the first time. Not for the UI task.
esp_err_t storage_space(uint64_t *total_bytes, uint64_t *free_bytes);
