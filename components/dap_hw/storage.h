#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define STORAGE_MOUNT_POINT "/sdcard"

// The SD card: mounting it, and keeping it safe to unmount when it is pulled
// out while the player runs.
//
// ONE TASK MOUNTS AND UNMOUNTS
//
// storage_mount, storage_unmount, storage_card_answers and storage_begin_loss
// are for one task only: the one that mounts the card (app_main.c). It is
// the only task that changes whether the card is mounted, so it can use the
// card structure without a lock.
//
// THE RULE FOR TASKS THAT USE THE CARD
//
//   if (storage_hold()) {
//     ... open, read, write, close files on the card ...
//     storage_release();
//   } else {
//     ... no card: give up at once ...
//   }
//
// - A hold is taken around one piece of work (a request, a save, a scan) and
//   given back when the work is over, in the same task. Every file opened
//   under it is closed before the release. Taking and giving back never
//   block: a few instructions in a critical section.
// - storage_hold refuses when there is no card, and from the moment the
//   mounting task has found that the card no longer answers
//   (storage_begin_loss). The card is unmounted only when no hold is left,
//   so no task is ever in the middle of a card operation when it goes.
// - A task that meets a card I/O failure calls storage_report_failure, and
//   carries on handling the failure as it always did. The mounting task then
//   asks the card whether it still answers (one status command); a card that
//   answers changes nothing. Nobody asks the card anything while nothing has
//   failed.
//
// The player holds the card from opening a track's file to closing it, and
// refuses to open one when storage_hold refuses (a PLAYER_ERROR_IO); the UI
// reports its read errors (PLAYER_ERROR_IO).

// Set up what follows. Call once, first thing at start-up, before any other
// task exists. Cannot fail (static memory).
void storage_init(void);

// Mount the card's FAT filesystem at STORAGE_MOUNT_POINT over 1-bit SDMMC.
// Safe to call when already mounted. Returns the driver's error if there is
// no card or it cannot be read; never formats the card. A failed call leaves
// nothing behind and logs nothing of its own (the IDF logs its errors), so it
// can be called again and again until a card appears.
esp_err_t storage_mount(void);

// Unmount the card. Does nothing if it is not mounted. Only once no hold is
// left (storage_holds), so no file is open.
void storage_unmount(void);

// Mounted, and not found to have gone (storage_begin_loss). Never blocks.
bool storage_is_mounted(void);

// See THE RULE FOR TASKS THAT USE THE CARD.
bool storage_hold(void);
void storage_release(void);

// Holds not yet given back. For the mounting task, which waits for 0.
uint32_t storage_holds(void);

// A task met a card I/O failure: wake the mounting task to check the card.
// Never blocks; reports made while one is already waiting are one report.
// Returns the number of checks the mounting task had finished at this
// moment (see storage_checks).
uint32_t storage_report_failure(void);

// The number of checks the mounting task has finished since start-up. A task
// that must know the verdict on its own report waits (a short, bounded
// while) for this to pass what storage_report_failure returned, and then
// looks at storage_is_mounted.
uint32_t storage_checks(void);

// Test aid (the console's `card lost`): report a failure, and make the check
// it causes fail as if the card had stopped answering.
void storage_simulate_loss(void);

// For the mounting task: sleep until a failure is reported.
void storage_wait_failure(void);

// For the mounting task: forget reports made before now (they were about a
// card that has since been unmounted).
void storage_forget_failures(void);

// For the mounting task, after a report: whether the mounted card still
// answers (one SEND_STATUS command; false if the console simulated a loss).
// Counts a finished check (storage_checks) only after it has called
// storage_begin_loss if the answer is no, so a task that waits for the
// verdict then sees the card as gone.
bool storage_card_answers(void);

// For the mounting task: the card has gone. From now on storage_hold refuses
// and storage_is_mounted is false; the card stays mounted until
// storage_unmount.
void storage_begin_loss(void);

// The card's size and its free space, in bytes, from the FAT
// (esp_vfs_fat_info). ESP_ERR_INVALID_STATE if no card is mounted, or the
// driver's error. It reads the card: FatFs keeps the free count once it
// knows it (from the card's FSInfo sector, or from one pass over the FAT
// the first time if that sector cannot be trusted), so it is quick after
// the first call but may not be the first time. Not for the UI task; the
// caller holds the card.
esp_err_t storage_space(uint64_t *total_bytes, uint64_t *free_bytes);
