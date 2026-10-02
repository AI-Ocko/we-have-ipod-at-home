#include "storage.h"

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <stdio.h>

static sdmmc_card_t *s_card;

esp_err_t storage_mount(void) {
  if (s_card) {
    return ESP_OK;
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false, // never wipe a card full of music
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
  };

  // Default speed (20MHz). The pins are fixed by the chip, see pins.h.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.unaligned_multi_block_rw_max_chunk_size = 8;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  // 1-bit only: this board's camera connector shares the other data lines.
  slot_config.width = 1;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_err_t ret = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host,
                                          &slot_config, &mount_config, &s_card);
  if (ret != ESP_OK) {
    // A failed mount has already undone itself: the IDF frees the card
    // structure, removes the slot and deletes the host controller before it
    // returns, so there is nothing to release here and calling again is safe.
    // Nothing is logged: the caller may be retrying, and decides how much to
    // say.
    s_card = NULL;
    return ret;
  }

  sdmmc_card_print_info(stdout, s_card);
  return ESP_OK;
}

void storage_unmount(void) {
  if (!s_card) {
    return;
  }
  esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, s_card);
  s_card = NULL;
}

bool storage_is_mounted(void) { return s_card != NULL; }
