#include "keys.h"

static const char KEY_CHARS[KEYS_COUNT + 1] = "123A456B789C*0#D";

void keys_init(keys_t *k) {
  k->candidate = 0;
  k->count = 0;
  k->stable = 0;
  k->active = -1;
  k->blocked = false;
  k->long_sent = false;
  k->pressed_at = 0;
  k->last_fire = 0;
}

char keys_char(int index) {
  if (index < 0 || index >= KEYS_COUNT) {
    return 0;
  }
  return KEY_CHARS[index];
}

static bool emit(key_event_t *event, key_event_type_t type, int index) {
  event->type = type;
  event->key = KEY_CHARS[index];
  return true;
}

bool keys_update(keys_t *k, uint16_t raw, uint32_t now_ms, key_event_t *event) {
  // Debounce: a snapshot is believed once it has been seen unchanged for
  // KEYS_DEBOUNCE_SCANS scans in a row.
  if (raw == k->candidate) {
    if (k->count < KEYS_DEBOUNCE_SCANS) {
      k->count++;
    }
  } else {
    k->candidate = raw;
    k->count = 1;
  }
  if (k->count >= KEYS_DEBOUNCE_SCANS) {
    k->stable = k->candidate;
  }

  if (k->active < 0) {
    uint16_t down = k->stable;
    if (down == 0) {
      k->blocked = false;
      return false;
    }
    if (k->blocked) {
      return false;
    }
    if (down & (down - 1)) { // more than one bit set
      k->blocked = true;
      return false;
    }
    int index = 0;
    while (!(down & 1u)) {
      down >>= 1;
      index++;
    }
    k->active = (int8_t)index;
    k->pressed_at = now_ms;
    k->long_sent = false;
    return emit(event, KEY_PRESS, index);
  }

  if (!(k->stable & (1u << k->active))) {
    int index = k->active;
    k->active = -1;
    return emit(event, KEY_RELEASE, index);
  }

  // The latest scan no longer shows the key, so it may be lifting; debounce
  // has not decided yet. Fire nothing meanwhile, or letting go would produce
  // one more repeat (or a late long press) after the finger has left the key.
  // The timers keep running: if this was only a bounce they carry on as before.
  if (!(raw & (1u << k->active))) {
    return false;
  }

  // Unsigned subtraction gives the right answer across a clock wrap-around.
  uint32_t held = now_ms - k->pressed_at;
  if (!k->long_sent) {
    if (held < KEYS_LONG_MS) {
      return false;
    }
    k->long_sent = true;
    k->last_fire = now_ms;
    return emit(event, KEY_LONG, k->active);
  }

  uint32_t interval =
      (held >= KEYS_FAST_AFTER_MS) ? KEYS_FAST_REPEAT_MS : KEYS_REPEAT_MS;
  if (now_ms - k->last_fire < interval) {
    return false;
  }
  k->last_fire = now_ms;
  return emit(event, KEY_REPEAT, k->active);
}
