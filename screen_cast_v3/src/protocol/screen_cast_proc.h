/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file screen_cast_proc.h
 * @brief Screen cast frame demultiplexer and ring buffers.
 *
 * Incoming continuous stream packets are de-multiplexed into JPEG frames,
 * MP3 audio, navigation info and camera info. Each type is queued in its own
 * ring buffer; the application acquires the oldest ready frame with
 * @ref sc_frame_acquire and returns it with @ref sc_frame_release.
 *
 * The demultiplexer lives in the SDK middleware so any cast application
 * (map cast, camera preview, screen mirroring ...) can reuse it; only the
 * rendering stays application specific.
 */

#ifndef SCREEN_CAST_PROC_H
#define SCREEN_CAST_PROC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Wire level definitions (OTA-V3 continual file stream)               */
/* ------------------------------------------------------------------ */

/* OTA types (dfu_type_t subset) */
#define SC_TYPE_PHOTO_PREVIEW       7
#define SC_TYPE_CONTINUAL_FILE      17

/* File types (dfu_file_type_t subset) */
#define SC_FILE_TYPE_RES            0
#define SC_FILE_TYPE_RES_WITHOUT_PATH 1
#define SC_FILE_TYPE_OTA_CTRL       2
#define SC_FILE_TYPE_OTA_BIN        3
#define SC_FILE_TYPE_DIRECT_BIN     4

/* Phone types (dfu_phone_type_t subset) */
#define SC_PHONE_TYPE_IOS           1
#define SC_PHONE_TYPE_ANDROID       2
#define SC_PHONE_TYPE_IOS_OLD       3
#define SC_PHONE_TYPE_ANDROID_SPP   4
#define SC_PHONE_TYPE_BT_PAN        5
#define SC_PHONE_TYPE_HARMONY       6
#define SC_PHONE_TYPE_DEVICE        7

/* Packet segmentation type carried in continual packets. */
enum
{
    SC_CONTINUAL_WHOLE_PACKET = 0,
    SC_CONTINUAL_START_PACKET,
    SC_CONTINUAL_MIDDLE_PACKET,
    SC_CONTINUAL_ENDDING_PACKET,
};

/* Maximum payload of one continual packet. */
#define SC_PACKET_LEN_CONTINUAL_FILE    4096

/**
 * @brief Parsed input packet handed to the processing layer.
 *
 * The protocol engine fills this structure for every continual packet and
 * passes it to @ref sc_proc_on_packet. It is also used as the shared receive
 * context of a cast session.
 */
typedef struct
{
    uint8_t  *packet_data;      /**< Payload of the current continual packet.  */
    uint32_t  packet_len;       /**< Payload length of the current packet.     */
    uint32_t  index;            /**< Packet index inside the current frame.    */
    uint8_t   packet_type;      /**< SC_CONTINUAL_* packet type.               */
    uint32_t  file_size;        /**< Total size of the current frame.          */
    uint8_t   file_aligned;     /**< Frame is 4-byte aligned + trailing CRC.   */
    uint8_t   phone_type;       /**< SC_PHONE_TYPE_*.                          */
    uint32_t  remote_version;   /**< Remote (phone) OTA code version.          */
} sc_recv_t;

/* ------------------------------------------------------------------ */
/* Frame API                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Display mode currently requested by the phone.
 */
enum
{
    SC_MAP_TYPE_NULL = 0,
    SC_MAP_TYPE_FULL_MODE,      /**< Full screen image (map / preview).        */
    SC_MAP_TYPE_NAV_MODE,       /**< Navigation overlay mode.                  */
    SC_MAP_TYPE_TAKE_PICTURE,
    SC_MAP_TYPE_START_RECORD,
    SC_MAP_TYPE_STOP_RECORD,
    SC_MAP_TYPE_MODE_DELAY_RECORD,
    SC_MAP_TYPE_MODE_RECORD,
    SC_MAP_TYPE_MODE_PICTRUE,
    SC_MAP_TYPE_MODE_PORTRAIT,
    SC_MAP_TYPE_MODE_NIGHT_VIEW,
};
typedef uint8_t sc_map_type_t;

/**
 * @brief Frame data type.
 */
enum
{
    SC_DATA_TYPE_JPEG = 0,      /**< JPEG image frame.                         */
    SC_DATA_TYPE_MP3,           /**< Audio frame (ID3 / RIFF).                 */
    SC_DATA_TYPE_EZIP,          /**< EZIP image frame.                         */
    SC_DATA_TYPE_NAV_INFO,      /**< Navigation JSON + turn icon (ezip).       */
    SC_DATA_TYPE_CAMERA_INFO,   /**< Camera status/JSON frame.                 */
    SC_DATA_TYPE_MAX,
};
typedef uint8_t sc_data_type_t;

enum
{
    SC_DATA_STATUS_IDLE = 0,    /**< Free.                                     */
    SC_DATA_STATUS_UPDATING,    /**< Being filled.                             */
    SC_DATA_STATUS_UPDATED,     /**< Ready to be consumed.                     */
    SC_DATA_STATUS_USING,       /**< Acquired by the application.              */
};
typedef uint8_t sc_data_status_t;

/**
 * @brief One frame slot of a ring buffer.
 */
typedef struct
{
    sc_data_status_t status;        /**< Buffer state.                         */
    uint32_t generate_timestamp;    /**< Timestamp provided by the phone.      */
    uint32_t tick;                  /**< Local update tick.                    */
    uint32_t malloc_size;           /**< Allocated size.                       */
    uint32_t size;                  /**< Valid data size.                      */
    uint32_t offset;                /**< Current write offset.                 */
    uint8_t *data;                  /**< Frame buffer.                         */
} sc_frame_t;

/* ------------------------------------------------------------------ */
/* Processing layer API                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the processing layer (mutex and buffer bookkeeping).
 */
void sc_proc_init(void);

/**
 * @brief Notify the processing layer of the remote OTA code version.
 */
void sc_proc_on_version(uint32_t remote_version);

/**
 * @brief Notify the processing layer of a file-init request.
 * @param[in] phone_type remote phone type (SC_PHONE_TYPE_*).
 * @return requested response frequency.
 */
uint16_t sc_proc_on_init_req(uint8_t phone_type);

/**
 * @brief File (frame) start notification, resets the receiving buffer.
 * @return 0 on success.
 */
int sc_proc_file_start(void);

/**
 * @brief File (frame) end notification, releases the receiving buffer.
 * @return 0 on success.
 */
int sc_proc_file_end(void);

/**
 * @brief Session end notification, releases all buffers.
 * @return 0 on success.
 */
int sc_proc_session_end(void);

/**
 * @brief Error notification, releases the receiving buffer.
 * @return 0 on success.
 */
int sc_proc_err(void);

/**
 * @brief Feed one continuous packet.
 * @param[in] recv parsed receive context.
 * @return 0 on success, negative when the frame was dropped.
 */
int sc_proc_on_packet(sc_recv_t *recv);

/**
 * @brief Notify the processing layer that the current packet is invalid.
 * @return 0 on success.
 */
int sc_proc_on_packet_error(void);

/**
 * @brief Acquire the oldest ready frame of the given type (marks it in-use).
 * @param[in] type frame type.
 * @return frame pointer or NULL when no frame is ready.
 */
sc_frame_t *sc_frame_acquire(sc_data_type_t type);

/**
 * @brief Release a frame previously acquired with @ref sc_frame_acquire.
 * @param[in] type frame type.
 * @return true on success.
 */
bool sc_frame_release(sc_data_type_t type);

/**
 * @brief Whether the remote side is currently pushing a cast stream.
 *
 * Set when a frame transfer starts and cleared when the session ends, an
 * error is reported or the link is lost. Applications usually use it to
 * decide whether the "scan to connect" page should stay visible.
 */
bool sc_is_casting(void);

/**
 * @brief Get the current display mode.
 */
sc_map_type_t sc_get_map_type(void);

/**
 * @brief Request the phone to switch mode (take picture / start record ...).
 */
void sc_set_switch_mode(uint8_t mode);

/**
 * @brief Get the pending mode requested by the application.
 */
uint8_t sc_get_switch_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_CAST_PROC_H */
