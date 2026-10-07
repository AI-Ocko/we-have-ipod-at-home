#include "storage.h"

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include <stdatomic.h>
#include <stdio.h>

// Changed only by the mounting task (storage.h); read by anyone under s_lock.
static sdmmc_card_t *s_card;

// s_lock guards these two and s_card's publication. Held for a few
// instructions at a time.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_lost;      // the card stopped answering: no new holds
static uint32_t s_holds; // holds not yet given back

// Reports of failures, for the mounting task. Static memory, so creating it
// cannot fail (storage_init).
static StaticSemaphore_t s_failed_block;
static SemaphoreHandle_t s_failed;
static atomic_bool s_simulated; // the console's `card lost`
static atomic_uint s_checks;    // checks finished

void storage_init(void) {
  if (s_failed == NULL) {
    s_failed = xSemaphoreCreateBinaryStatic(&s_failed_block);
  }
}

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

  sdmmc_card_t *card = NULL;
  esp_err_t ret = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host,
                                          &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    // A failed mount has already undone itself: the IDF frees the card
    // structure, removes the slot and deletes the host controller before it
    // returns, so there is nothing to release here and calling again is safe.
    // Nothing is logged: the caller may be retrying, and decides how much to
    // say.
    return ret;
  }

  sdmmc_card_print_info(stdout, card);
  portENTER_CRITICAL(&s_lock);
  s_card = card;
  s_lost = false;
  portEXIT_CRITICAL(&s_lock);
  return ESP_OK;
}

void storage_unmount(void) {
  if (!s_card) {
    return;
  }
  sdmmc_card_t *card = s_card;
  portENTER_CRITICAL(&s_lock);
  s_card = NULL; // no hold can be taken from here on
  s_lost = false;
  portEXIT_CRITICAL(&s_lock);
  esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, card);
}

bool storage_is_mounted(void) {
  portENTER_CRITICAL(&s_lock);
  bool mounted = s_card != NULL && !s_lost;
  portEXIT_CRITICAL(&s_lock);
  return mounted;
}

bool storage_hold(void) {
  portENTER_CRITICAL(&s_lock);
  bool held = s_card != NULL && !s_lost;
  if (held) {
    s_holds++;
  }
  portEXIT_CRITICAL(&s_lock);
  return held;
}

void storage_release(void) {
  portENTER_CRITICAL(&s_lock);
  if (s_holds > 0) {
    s_holds--;
  }
  portEXIT_CRITICAL(&s_lock);
}

uint32_t storage_holds(void) {
  portENTER_CRITICAL(&s_lock);
  uint32_t holds = s_holds;
  portEXIT_CRITICAL(&s_lock);
  return holds;
}

uint32_t storage_report_failure(void) {
  uint32_t checks = atomic_load(&s_checks);
  if (s_failed != NULL) {
    xSemaphoreGive(s_failed); // a binary semaphore: never blocks
  }
  return checks;
}

uint32_t storage_checks(void) { return atomic_load(&s_checks); }

void storage_simulate_loss(void) {
  atomic_store(&s_simulated, true);
  storage_report_failure();
}

void storage_wait_failure(void) {
  xSemaphoreTake(s_failed, portMAX_DELAY);
}

void storage_forget_failures(void) { xSemaphoreTake(s_failed, 0); }

bool storage_card_answers(void) {
  bool answers = false;
  if (s_card != NULL && !atomic_exchange(&s_simulated, false)) {
    // The host driver serialises commands, so this may go while another
    // task is reading: it waits its turn for the bus.
    answers = (sdmmc_get_status(s_card) == ESP_OK);
  }
  if (!answers) {
    storage_begin_loss();
  }
  atomic_fetch_add(&s_checks, 1);
  return answers;
}

void storage_begin_loss(void) {
  portENTER_CRITICAL(&s_lock);
  s_lost = true;
  portEXIT_CRITICAL(&s_lock);
}

esp_err_t storage_space(uint64_t *total_bytes, uint64_t *free_bytes) {
  if (!storage_is_mounted()) {
    return ESP_ERR_INVALID_STATE;
  }
  return esp_vfs_fat_info(STORAGE_MOUNT_POINT, total_bytes, free_bytes);
}
