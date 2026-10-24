#pragma once
// Small NVS wrapper: one blob per key in the "stand" namespace.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool kv_init(void);
// Returns the blob size, 0 if missing or unreadable.
size_t kv_get(const char *key, uint8_t *buf, size_t cap);
bool kv_set(const char *key, const uint8_t *buf, size_t len);
// Removes the key; a missing key counts as success.
bool kv_erase(const char *key);
