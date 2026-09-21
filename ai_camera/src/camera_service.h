/* SPDX-License-Identifier: Apache-2.0 */
#ifndef AI_CAMERA_SERVICE_H
#define AI_CAMERA_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include "photo_storage/photo_store.h"

#define CAMERA_LIVE_WIDTH 800U
#define CAMERA_LIVE_HEIGHT 600U
#define CAMERA_PHOTO_WIDTH 1600U
#define CAMERA_PHOTO_HEIGHT 1200U
#define CAMERA_THUMB_WIDTH 200U
#define CAMERA_THUMB_HEIGHT 150U
#define CAMERA_PAGE_SIZE 10U

typedef struct
{
    const uint8_t *pixels;
    uint32_t sequence;
    int slot;
} camera_live_frame_t;

typedef enum
{
    CAMERA_MEDIA_CAPTURE = 1,
    CAMERA_MEDIA_PAGE,
    CAMERA_MEDIA_PHOTO,
    CAMERA_MEDIA_ROTATE,
    CAMERA_MEDIA_PHOTO_INDEX,
    CAMERA_MEDIA_DELETE
} camera_media_kind_t;

typedef enum
{
    CAMERA_CAPTURE_IDLE,
    CAMERA_CAPTURE_CAPTURING,
    CAMERA_CAPTURE_SAVING,
    CAMERA_CAPTURE_RESTORING,
} camera_capture_stage_t;

typedef struct
{
    camera_media_kind_t kind;
    int status;
    uint32_t token;
    uint32_t offset;
    uint32_t total;
    size_t count;
    photo_store_info_t items[CAMERA_PAGE_SIZE];
    const uint8_t *thumbnails[CAMERA_PAGE_SIZE];
    const uint8_t *photo;
    uint16_t width;
    uint16_t height;
    uint16_t source_width;
    uint16_t source_height;
    uint32_t file_size;
    /* Capture status describes the photo; preview_status describes recovery. */
    int preview_status;
} camera_media_result_t;

int camera_service_start(void);
void camera_service_set_live(int enabled);
/* UI-thread hooks: count a distinct preview after LCD swap completion, then
 * report the received/published/displayed rates from the regular UI loop. */
void camera_service_record_displayed(void);
void camera_service_report_fps(void);
/* Successful take borrows a completed DMA frame and excludes media reuse.
 * Circular DMA keeps running: validate after LVGL/EPIC reads finish, then
 * release the reference. The caller must not write into a live frame. */
int camera_service_take_frame(camera_live_frame_t *frame);
int camera_service_frame_is_valid(const camera_live_frame_t *frame);
void camera_service_release_frame(camera_live_frame_t *frame);
/* Media calls are nonblocking; only one request/result may be outstanding.
 * Page/full-photo buffers may be reused by the next media request. Hide the
 * affected LVGL images and wait for EPIC before submitting another request. */
/* Detach all UI image references before capture. The capture result is posted
 * after the still image is saved (or fails) and preview recovery is attempted. */
int camera_service_capture(uint32_t token);
/* Nonblocking toggle between 0 and 180 degrees; requires live preview and
 * no pending media operation. Completion uses the normal media result API. */
int camera_service_rotate(uint32_t token);
camera_capture_stage_t camera_service_capture_stage(void);
int camera_service_request_page(uint32_t offset, uint32_t count, uint32_t token);
int camera_service_request_photo(const char *path, uint32_t token);
/* Load one photo by its zero-based position in the full album, newest first.
 * An unavailable position fails; this API does not wrap at album boundaries. */
int camera_service_request_photo_at(uint32_t offset, uint32_t token);
/* Removes one album photo by its zero-based position in the full album, newest
 * first. Filesystem work only, so it also runs while the camera is live. The
 * result carries the removed record in items[0] with count == 1 and the album
 * size afterwards in total. Every later position shifts down, so the caller has
 * to reload the page it is showing. */
int camera_service_request_delete(uint32_t offset, uint32_t token);
int camera_service_poll_result(camera_media_result_t *result);
int camera_service_camera_status(void);

#endif

