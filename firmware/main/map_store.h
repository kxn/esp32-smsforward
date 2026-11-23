#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MAP_STORE_CAPACITY 512

bool map_store_init(const char *path);
bool map_store_find_message(const char *record_id, char *message_id, size_t capacity);
bool map_store_identity_path(const char *path, char *out, size_t out_len);
bool map_store_note(const char *msg_id, int slot, const char *peer, const char *record_id);
bool map_store_note_sim(const char *msg_id, int slot, const char *peer,
                        const char *record_id, const char *iccid);
bool map_store_lookup(const char *msg_id, int *slot, char *peer, size_t peer_len,
                      char *record_id, size_t record_id_len);
bool map_store_lookup_sim(const char *msg_id, int *slot, char *peer, size_t peer_len,
                          char *record_id, size_t record_id_len,
                          char *iccid, size_t iccid_len);
bool map_store_clear(void);
