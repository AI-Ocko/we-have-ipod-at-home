#pragma once

#include "esp_err.h"
#include <stdbool.h>

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
