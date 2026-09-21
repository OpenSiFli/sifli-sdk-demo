/* SPDX-License-Identifier: Apache-2.0 */
#ifndef PHOTO_STORE_H
#define PHOTO_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHOTO_STORE_PATH_MAX 64U
#define PHOTO_STORE_DIRECTORY "/photo"

typedef struct
{
    uint32_t id;
    char path[PHOTO_STORE_PATH_MAX];
    uint32_t size;
} photo_store_info_t;

typedef struct
{
    int ready;
    int last_error;
    uint32_t photo_count;
    /* FAT data-area bytes; free_bytes includes space needed for metadata. */
    uint32_t total_bytes;
    uint32_t free_bytes;
} photo_store_status_t;

/* Calls are synchronous, serialized, and return zero or a negative errno. */
/* Mount and inspect storage; an absent photo directory is an empty album. */
int photo_store_init(void);
int photo_store_get_status(photo_store_status_t *status);

/* Newest IDs first. Offset is a zero-based item index; capacity may be zero. */
int photo_store_list(uint32_t offset, photo_store_info_t *items, size_t capacity,
                     size_t *written, uint32_t *total);
int photo_store_save_jpeg(const void *data, size_t size, photo_store_info_t *info);

int photo_store_delete_at(uint32_t offset, photo_store_info_t *deleted);

int photo_store_load_rotation(uint16_t *degrees);
int photo_store_save_rotation(uint16_t degrees);

/* A NULL buffer with zero capacity queries the file size without reading it. */
int photo_store_read_jpeg(const char *path, void *buffer, size_t capacity,
                          size_t *size);

#ifdef __cplusplus
}
#endif

#endif