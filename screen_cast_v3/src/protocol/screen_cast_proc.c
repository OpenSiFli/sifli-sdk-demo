/*
 * SPDX-FileCopyrightText: 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file screen_cast_proc.c
 * @brief Screen cast frame demultiplexer and ring buffers.
 *
 * Ported from solution2.0 `components/ota/ota_app/proc/photo_view/ota_photoview_proc.c`.
 */

#include "screen_cast_proc.h"
#include "app_mem.h"

#include <rtthread.h>
#include <string.h>
#include <board.h>

#define LOG_TAG     "SC_CAST"
#include "log.h"

#define JPEG_RSP_FREQ               3

#if defined(SOC_SF32LB58X) || defined(SOC_SF32LB57X)
    #define MAX_CACHE_NUMBER            (4)
    #define RECV_THREAD_YILED           (1)
#else
    #define MAX_CACHE_NUMBER            (2)
    #define RECV_THREAD_YILED           (1)
#endif

enum
{
    MAP_DATA_NO_JUDGE = 0,
    MAP_DATA_WITHOUT_PROTOCOL,
    MAP_DATA_PROTOCOL,
};

typedef void *(*malloc_t)(size_t size);
typedef void (*free_t)(void *ptr);
typedef void *(*realloc_t)(void *ptr, size_t size);

typedef struct
{
    sc_frame_t frames[MAX_CACHE_NUMBER];        // frame array
    int head;                                   // write position
    int tail;                                   // read position
    int count;                                  // current frame count
    malloc_t malloc;
    free_t free;
    realloc_t realloc;
} frame_cache_list_t;

typedef struct
{
    uint8_t inited;
    frame_cache_list_t cahe[SC_DATA_TYPE_MAX];
    sc_data_type_t cur_type;
    sc_map_type_t  map_type;
    uint8_t is_judge_protocol;
    uint8_t frame_droped;   // whether frame is dropped
    uint32_t fps_start_tick;
    uint32_t fps_recv_frames;
    uint32_t fps_drop_frames;
    uint32_t fps_recv_size;

    uint32_t total_recv_frames;     // total received frames
    uint32_t total_drop_frames;     // total dropped frames
} CircularBuffer;

static uint8_t jpeg_rsp_req = JPEG_RSP_FREQ;
static uint8_t log_on = 0;
static uint8_t dump_on = 0;     // dump frame size + CRC before decoding, for link verification
static uint8_t raw_dump_type = SC_DATA_TYPE_JPEG;   // sc_cast raw: frame type to export
static uint16_t raw_dump_count = 0;                 // sc_cast raw: frames left to export (0 = off)
static sc_map_type_t map_update = SC_MAP_TYPE_NULL;
static struct rt_mutex g_photoview_mutex;
static sc_frame_t *p_rcving_frame = NULL;
static CircularBuffer g_map_context = {0};
static bool support_camera_info = false;
static bool g_casting = false;

extern void *sifli_memcpy(void *dst, const void *src, rt_ubase_t count);

/* CRC32/MPEG-2 using the hardware CRC unit (init 0xFFFFFFFF, no final
 * inversion). Same algorithm as the serial-transmission framing, used to
 * verify the received frame data. */
static uint32_t sc_crc32_mpeg2(const uint8_t *data, uint32_t len)
{
    CRC_HandleTypeDef crc_handle = {0};

    crc_handle.Instance = hwp_crc;
    if (HAL_CRC_Init(&crc_handle) != HAL_OK)
    {
        return 0;
    }
    HAL_CRC_Setmode(&crc_handle, CRC_32_MPEG_2);
    return HAL_CRC_Calculate(&crc_handle, (uint8_t *)data, len);
}

/* Link verification without a working LCD: dump frame size and CRC before decoding */
static void sc_dump_frame(sc_data_type_t type, sc_frame_t *frame)
{
    uint32_t crc = sc_crc32_mpeg2(frame->data, frame->offset);
    uint32_t head = 0;
    uint32_t head_len = (frame->offset >= 4) ? 4 : frame->offset;

    if (head_len)
    {
        rt_memcpy(&head, frame->data, head_len);
    }
    LOG_I("frame dump: type %d, size %d, head 0x%08x, crc32mpeg2 0x%08x",
          type, frame->offset, head, crc);
}

static uint8_t judge_protocol(sc_recv_t *recv, uint8_t *packet_data, uint32_t packet_len, uint32_t data_len)
{
    if (packet_len < 10)
    {
        return MAP_DATA_WITHOUT_PROTOCOL;
    }
    uint16_t type;
    uint32_t timestamp;
    uint32_t total_len;
    rt_memcpy(&type, packet_data, 2);
    rt_memcpy(&timestamp, packet_data + 2, 4);
    rt_memcpy(&total_len, packet_data + 6, 4);

    LOG_I("data_type:%d, timestamp:%u, total_len:%d, data_len:%d align %d", type, timestamp, total_len, data_len, recv->file_aligned);
    if (type < SC_DATA_TYPE_MAX && ((!recv->file_aligned && (total_len + 10 == data_len)) ||
                                    (recv->file_aligned && ((total_len + 14 + 3) / 4 * 4 == data_len))))
    {
        LOG_I("is protocol data");
        return MAP_DATA_PROTOCOL;
    }
    LOG_I("not protocol data");
    return MAP_DATA_WITHOUT_PROTOCOL;
}

static void *jpeg_aligned_malloc(size_t size)
{
    const size_t alignment = 64;
    const size_t offset = alignment - 1 + sizeof(void *);
    size = size + offset;
    void *raw_ptr = app_cache_alloc(size, CACHE_PSRAM);
    if (raw_ptr == NULL)
    {
        return NULL;
    }
    void *aligned_ptr = (void *)(((uintptr_t)raw_ptr + sizeof(void *) + alignment - 1) & ~(alignment - 1));
    *(void **)((uintptr_t)aligned_ptr - sizeof(void *)) = raw_ptr;

    return aligned_ptr;
}

static void jpeg_aligned_free(void *aligned_ptr)
{
    if (aligned_ptr == NULL)
    {
        return;
    }
    void *raw_ptr = *(void **)((uintptr_t)aligned_ptr - sizeof(void *));
    app_cache_free(raw_ptr);
}

static void *jpeg_aligned_realloc(void *ptr, size_t size)
{
    return app_cache_realloc_ex(ptr, size, CACHE_PSRAM);
}

static int is_audio(const uint8_t *raw_data, size_t len)
{
    if (len < 10) return 0;
    if (memcmp((const char *)raw_data, "ID3", 3) != 0)
    {
        if (!memcmp((const char *)raw_data, "RIFF", 4))
        {
            LOG_I("wav");
            return 1;
        }
    }
    else
    {
        return 1;
    }
    return 0;
}

static int is_jpg(const uint8_t *raw_data, size_t len)
{
    const uint8_t jpg_signature_standard[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46};
    const uint8_t jpg_signature_exif[] = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x18, 0x45, 0x78, 0x69, 0x66};

    if (len < sizeof(jpg_signature_standard) || len < sizeof(jpg_signature_exif)) return 0;

    if (memcmp(jpg_signature_standard, raw_data, sizeof(jpg_signature_standard)) == 0)
    {
        return 1;
    }

    if (memcmp(jpg_signature_exif, raw_data, sizeof(jpg_signature_exif)) == 0)
    {
        return 1;
    }

    if (len >= 3 && raw_data[0] == 0xFF && raw_data[1] == 0xD8 && raw_data[2] == 0xFF)
    {
        return 1;
    }
    return 0;
}

// init buffers
static void buffer_init(void)
{
    CircularBuffer *buffer = &g_map_context;
    if (buffer->inited) return;
    LOG_I("%s:%p", __func__, buffer);
    rt_memset(buffer, 0, sizeof(CircularBuffer));
    buffer->inited = 1;

    for (uint8_t i = 0; i < SC_DATA_TYPE_MAX; i++)
    {

        buffer->cahe[i].malloc = (malloc_t)jpeg_aligned_malloc;
        buffer->cahe[i].free = (free_t)jpeg_aligned_free;
        buffer->cahe[i].realloc = (realloc_t)jpeg_aligned_realloc;
    }
    buffer->fps_start_tick = rt_tick_get_millisecond();
}

static void buffer_deinit(void)
{
    CircularBuffer *buffer = &g_map_context;
    if (buffer->inited == 0) return;
    rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);
    LOG_I("%s:%p", __func__, buffer);
    for (uint8_t i = 0; i < SC_DATA_TYPE_MAX; i++)
    {
        for (uint8_t j = 0; j < MAX_CACHE_NUMBER; j++)
        {
            if (buffer->cahe[i].frames[j].data)
            {
                buffer->cahe[i].free(buffer->cahe[i].frames[j].data);
                buffer->cahe[i].frames[j].data = NULL;
                buffer->cahe[i].frames[j].status = SC_DATA_STATUS_IDLE;
                buffer->cahe[i].frames[j].size = 0;
            }
        }
    }
    buffer->inited = 0;
    buffer->map_type = SC_MAP_TYPE_NULL;
    rt_mutex_release(&g_photoview_mutex);
}

static uint32_t buffer_get_updated_frames_cnt(CircularBuffer *buffer, sc_data_type_t type)
{
    uint32_t updated_cnt = 0;
    for (uint8_t i = 0; i < MAX_CACHE_NUMBER; i++)
    {
        if (buffer->cahe[type].frames[i].status == SC_DATA_STATUS_UPDATED)
        {
            updated_cnt++;
        }
    }
    return updated_cnt;
}

static sc_frame_t *buffer_get_old_updated_frame(CircularBuffer *buffer, sc_data_type_t type)
{
    sc_frame_t *oldest_frame = NULL;
    uint32_t tick = 0;
    for (uint8_t i = 0; i < MAX_CACHE_NUMBER; i++)
    {
        if (buffer->cahe[type].frames[i].status == SC_DATA_STATUS_UPDATED)
        {
            if (oldest_frame == NULL || buffer->cahe[type].frames[i].tick < tick)
            {
                oldest_frame = &buffer->cahe[type].frames[i];
                tick = oldest_frame->tick;
            }
        }
    }
    return oldest_frame;
}

static sc_frame_t *buffer_get_no_using_frame(CircularBuffer *buffer, sc_data_type_t type)
{
    sc_frame_t *frame = NULL;
    uint32_t tick = 0;
    for (uint8_t i = 0; i < MAX_CACHE_NUMBER; i++)
    {
        if (buffer->cahe[type].frames[i].status == SC_DATA_STATUS_IDLE)
        {
            if (!frame || buffer->cahe[type].frames[i].tick < tick)
            {
                frame = &buffer->cahe[type].frames[i];
                tick = frame->tick;
            }
        }
    }
    if (frame == NULL)
    {
        frame = buffer_get_old_updated_frame(buffer, type);
    }
    return frame;
}

static sc_frame_t *buffer_get_using_frame(CircularBuffer *buffer, sc_data_type_t type)
{
    sc_frame_t *frame = NULL;
    for (uint8_t i = 0; i < MAX_CACHE_NUMBER; i++)
    {
        if (buffer->cahe[type].frames[i].status == SC_DATA_STATUS_USING)
        {
            frame = &buffer->cahe[type].frames[i];
            break;
        }
    }
    return frame;
}

static sc_frame_t *buffer_get_writing_frame(CircularBuffer *buffer, sc_data_type_t type)
{
    sc_frame_t *frame = NULL;
    for (uint8_t i = 0; i < MAX_CACHE_NUMBER; i++)
    {
        if (buffer->cahe[type].frames[i].status == SC_DATA_STATUS_UPDATING)
        {
            frame = &buffer->cahe[type].frames[i];
            break;
        }
    }
    return frame;
}

static void buffer_dump_frame_status(CircularBuffer *buffer, sc_data_type_t type)
{
    for (uint8_t i = 0; i < MAX_CACHE_NUMBER; i++)
    {
        LOG_I("type %d,frame[%d] %p status:%d", type, i, &buffer->cahe[type].frames[i], buffer->cahe[type].frames[i].status);
    }
}

static sc_frame_t *buffer_get_frame_to_write(CircularBuffer *buffer, sc_data_type_t type, uint32_t w_size)
{
    sc_frame_t *frame = NULL;

    rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);

#if RECV_THREAD_YILED
    uint8_t retry = 1;
    if (type == SC_DATA_TYPE_MP3)
    {
        retry = 255; // can't drop mp3
    }
    do
    {
        frame = buffer_get_no_using_frame(buffer, type);
        if (!frame)
        {
            buffer_dump_frame_status(buffer, type);
            LOG_I("map view assemble thread yield");
            rt_thread_yield();
            rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);
        }
        else if (frame->status == SC_DATA_STATUS_IDLE)
        {
            break;
        }
        else if (frame->status == SC_DATA_STATUS_USING || frame->status == SC_DATA_STATUS_UPDATED)
        {
            rt_mutex_release(&g_photoview_mutex);

            LOG_I("map view assemble thread yield");
            rt_thread_yield();
            rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);
        }
    }
    while (retry--);
#endif
    buffer->frame_droped = 0;
    frame = buffer_get_no_using_frame(buffer, type);
    if (!frame || frame->status == SC_DATA_STATUS_USING)
    {
        LOG_I("no frame or frame all is using");
        buffer_dump_frame_status(buffer, type);
        RT_ASSERT(0); // two frames are using at the same time
    }
    if (frame->status == SC_DATA_STATUS_UPDATED)
    {
        buffer->total_drop_frames++;
        buffer->fps_drop_frames++;
        buffer->frame_droped = 1;
    }
    if (log_on)
    {
        LOG_I("get frame %p as updating, last status %d", frame, frame->status);
    }
    frame->status = SC_DATA_STATUS_UPDATING;
    frame->offset = 0;

    if (!frame->data || frame->malloc_size < w_size)
    {
        if (frame->data)
        {
            buffer->cahe[type].free(frame->data);
            frame->data = NULL;
            frame->malloc_size = 0;
        }
        frame->data = buffer->cahe[type].malloc(w_size);
        if (frame->data == NULL)
        {

            frame->status = SC_DATA_STATUS_IDLE;
            frame->size = 0;
            frame->offset = 0;
            rt_mutex_release(&g_photoview_mutex);
            LOG_W("frame malloc fail! type %d size %d, drop this frame", type, w_size);
            return NULL;
        }
        frame->malloc_size = w_size;
    }
    frame->size = w_size;
    rt_mutex_release(&g_photoview_mutex);
    return frame;
}

static bool buffer_set_frame_done(CircularBuffer *buffer, sc_frame_t *frame)
{
    rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);
    RT_ASSERT(frame->status == SC_DATA_STATUS_UPDATING);
    frame->tick = rt_tick_get_millisecond();
    frame->status = SC_DATA_STATUS_UPDATED;
    buffer->total_recv_frames++;
    buffer->fps_recv_frames++;
    buffer->fps_recv_size += frame->size;
    uint32_t tick = rt_tick_get_millisecond();
    if (log_on || tick - buffer->fps_start_tick > 1000)
    {
        uint32_t delayed = buffer_get_updated_frames_cnt(buffer, buffer->cur_type);
        rt_kprintf("%s %p len %d elpas %d ms rate %.2f KB/s pileup %d drop 0x%x total 0x%x drop %.2f recv_fps %.2f disp_fps %.2f\n",
                   buffer->cur_type == SC_DATA_TYPE_JPEG ? "JPEG" : "MP3", frame,
                   buffer->fps_recv_size, tick - buffer->fps_start_tick,
                   (buffer->fps_recv_size * 1.0 / 1024) / ((tick - buffer->fps_start_tick) * 1.0 / RT_TICK_PER_SECOND), delayed,
                   buffer->total_drop_frames, buffer->total_recv_frames,
                   buffer->total_drop_frames * 1.0 / buffer->total_recv_frames,
                   (buffer->fps_recv_frames) * 1.0 / ((tick - buffer->fps_start_tick * 1.0) / RT_TICK_PER_SECOND),
                   (buffer->fps_recv_frames - buffer->fps_drop_frames) * 1.0 / ((tick - buffer->fps_start_tick * 1.0) / RT_TICK_PER_SECOND));

        buffer->fps_start_tick = tick;
        buffer->fps_drop_frames = 0;
        buffer->fps_recv_frames = 0;
        buffer->fps_recv_size = 0;
    }
    rt_mutex_release(&g_photoview_mutex);
    return true;
}

static void buffer_update_writing_frame_err(CircularBuffer *buffer, sc_data_type_t type)
{
    rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);

    sc_frame_t *frame = buffer_get_writing_frame(buffer, SC_DATA_TYPE_JPEG);
    if (frame)
    {
        frame->offset = 0;
        frame->status = SC_DATA_STATUS_IDLE;
        LOG_I("update writing frame addr %p  type %d err", frame, type);
    }
    frame = buffer_get_writing_frame(buffer, SC_DATA_TYPE_MP3);
    if (frame)
    {
        frame->offset = 0;
        frame->status = SC_DATA_STATUS_IDLE;
        LOG_I("update writing frame addr %p  type %d err", frame, type);
    }
    rt_mutex_release(&g_photoview_mutex);
}

/* Dump a consumed frame to the serial port as hex (offline verification),
 * enabled by sc_cast raw. Runs in the consumer (main) thread without holding
 * the lock, so it neither blocks the transport thread nor trips the link
 * watchdog. */
static void sc_raw_dump_frame(sc_data_type_t type, sc_frame_t *frame)
{
    uint32_t crc = sc_crc32_mpeg2(frame->data, frame->size);
    uint32_t i;

    rt_kprintf("RAWDUMP begin type=%d size=%d crc32mpeg2=0x%08x\n", type, frame->size, crc);
    for (i = 0; i < frame->size; i += 32)
    {
        uint32_t j;
        uint32_t end = (i + 32 < frame->size) ? (i + 32) : frame->size;
        for (j = i; j < end; j++)
        {
            rt_kprintf("%02x", frame->data[j]);
        }
        rt_kprintf("\n");
    }
    rt_kprintf("RAWDUMP end\n");
}

sc_frame_t *sc_frame_acquire(sc_data_type_t type)
{
    CircularBuffer *buffer = &g_map_context;
    sc_frame_t *frame;
    if (!buffer->inited) return NULL;
    rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);
    frame = buffer_get_old_updated_frame(buffer, type);
    if (frame && frame->status == SC_DATA_STATUS_UPDATED)
    {
        frame->status = SC_DATA_STATUS_USING;
        if (log_on)
            LOG_I("get frame addr %p update tick %d as using,  type:%d, size:%d", frame, frame->tick, type, frame->size);
    }
    rt_mutex_release(&g_photoview_mutex);

    if (raw_dump_count > 0 && frame != NULL && type == raw_dump_type &&
            frame->data != NULL && frame->size > 0)
    {
        raw_dump_count--;
        sc_raw_dump_frame(type, frame);
        rt_kprintf("RAWDUMP left %d\n", raw_dump_count);
    }
    return frame;
}

bool sc_frame_release(sc_data_type_t type)
{
    CircularBuffer *buffer = &g_map_context;
    sc_frame_t *frame;
    if (!buffer->inited) return false;
    rt_mutex_take(&g_photoview_mutex, RT_WAITING_FOREVER);
    frame = buffer_get_using_frame(buffer, type);

    if (frame && frame->status == SC_DATA_STATUS_USING)
    {
        frame->status = SC_DATA_STATUS_IDLE;
        rt_mutex_release(&g_photoview_mutex);
        if (log_on)
            LOG_I("frame addr %p,tick %d status update to idle", frame, frame->tick);
        return true;
    }
    LOG_I("no frame in using status, type:%d", type);
    buffer_dump_frame_status(buffer, type);
    rt_mutex_release(&g_photoview_mutex);
    return false;
}

bool sc_is_casting(void)
{
    return g_casting;
}

sc_map_type_t sc_get_map_type(void)
{
    return g_map_context.map_type;
}

uint8_t sc_get_switch_mode(void)
{
    return map_update;
}

void sc_set_switch_mode(uint8_t type)
{
    map_update = type;
}

static void sc_free_data(void)
{
    p_rcving_frame = NULL;
    buffer_deinit();
}

void sc_proc_on_version(uint32_t remote_version)
{
    support_camera_info = (remote_version & 0xff) >= 4 ? true : false;
}

uint16_t sc_proc_on_init_req(uint8_t phone_type)
{
    uint16_t rsp_freq = jpeg_rsp_req;
    if (jpeg_rsp_req == JPEG_RSP_FREQ && (phone_type == SC_PHONE_TYPE_ANDROID_SPP || phone_type == SC_PHONE_TYPE_BT_PAN))
    {
#if defined(SOC_SF32LB58X) || defined(SOC_SF32LB57X)
        rsp_freq = 8;
#else
        rsp_freq = 4;
#endif
        if (phone_type == SC_PHONE_TYPE_ANDROID_SPP)
        {
            rsp_freq = 1;
        }
    }
    LOG_I("update rsp_freq %d", rsp_freq);
    return rsp_freq;
}

int sc_proc_file_start(void)
{
    g_casting = true;
    buffer_init();
    return RT_EOK;
}

int sc_proc_file_end(void)
{
    sc_free_data();
    return RT_EOK;
}

int sc_proc_session_end(void)
{
    g_casting = false;
    sc_free_data();
    return RT_EOK;
}

int sc_proc_err(void)
{
    g_casting = false;
    sc_free_data();
    return RT_EOK;
}

int sc_proc_on_packet(sc_recv_t *recv)
{
    uint8_t *data = recv->packet_data;
    uint32_t len = recv->packet_len;
    CircularBuffer *buffer = &g_map_context;
    if (recv->index == 1)
    {
        if (buffer->is_judge_protocol == MAP_DATA_NO_JUDGE)
        {
            buffer->is_judge_protocol = judge_protocol(recv, data, len, recv->file_size);
        }
        uint16_t type;
        uint32_t total_len;
        uint32_t timestamp;
        if (support_camera_info && buffer->is_judge_protocol != MAP_DATA_PROTOCOL)
        {
            LOG_I("support camera info, but not protocol data, drop it");
            return RT_ERROR;
        }
        if (buffer->is_judge_protocol == MAP_DATA_PROTOCOL)
        {
            rt_memcpy(&type, data, 2);
            rt_memcpy(&timestamp, data + 2, 4);
            total_len = recv->file_size - 10;
            data += 10;
            len -= 10;
        }
        else
        {
            if (is_jpg(data, len))
            {
                type = SC_DATA_TYPE_JPEG;
            }
            else if (is_audio(data, len))
            {
                type = SC_DATA_TYPE_MP3;
            }
            else
            {
                type = SC_DATA_TYPE_NAV_INFO;
            }
            total_len = recv->file_size;
            timestamp = 0;
        }
        if (type != SC_DATA_TYPE_MP3)
        {
            if (type == SC_DATA_TYPE_NAV_INFO && buffer->map_type != SC_MAP_TYPE_NAV_MODE)
            {
                buffer->map_type = SC_MAP_TYPE_NAV_MODE;
                LOG_I("map type change nav mode");
            }
            else if ((type == SC_DATA_TYPE_JPEG || type == SC_DATA_TYPE_CAMERA_INFO) && buffer->map_type != SC_MAP_TYPE_FULL_MODE)
            {
                buffer->map_type = SC_MAP_TYPE_FULL_MODE;
                LOG_I("map type change full mode");
            }
        }
        if (p_rcving_frame)
        {
            buffer_update_writing_frame_err(buffer, buffer->cur_type);
        }

        buffer->cur_type = type;
        p_rcving_frame = buffer_get_frame_to_write(buffer, type, total_len);
        if (NULL == p_rcving_frame)
        {
            return RT_ERROR;
        }
        p_rcving_frame->generate_timestamp = timestamp;
    }
    if (NULL == p_rcving_frame)
    {
        LOG_I("p_rcving_frame is NULL, idx %d, type %d, data %p, len %d buffer_init %d", recv->index, buffer->cur_type, data, len, buffer->inited);
        return RT_ERROR;
    }
    if (p_rcving_frame->data)
    {
        if (p_rcving_frame->offset + len > p_rcving_frame->size)
        {
            rt_kprintf("p_rcving_frame->offset %d + len %d > p_rcving_frame->size %d\n", p_rcving_frame->offset, len, p_rcving_frame->size);
            RT_ASSERT(0);
        }
        sifli_memcpy(&p_rcving_frame->data[p_rcving_frame->offset], data, len);
        p_rcving_frame->offset += len;
        if (recv->packet_type == SC_CONTINUAL_WHOLE_PACKET || recv->packet_type == SC_CONTINUAL_ENDDING_PACKET)
        {
            if (dump_on && p_rcving_frame->data && p_rcving_frame->offset > 0)
            {
                sc_dump_frame(buffer->cur_type, p_rcving_frame);
            }
            buffer_set_frame_done(buffer, p_rcving_frame);
            p_rcving_frame = NULL;
            if (buffer->frame_droped) return -1;
        }
    }
    return RT_EOK;
}

int sc_proc_on_packet_error(void)
{
    CircularBuffer *buffer = &g_map_context;
    buffer_update_writing_frame_err(buffer, buffer->cur_type);
    return RT_EOK;
}

void sc_proc_init(void)
{
    rt_mutex_init(&g_photoview_mutex, "sc_mutex", RT_IPC_FLAG_FIFO);
}

#ifdef RT_USING_FINSH

int sc_cast(int argc, char *argv[])
{
    if (argc < 2) return -1;
    if (!strcmp(argv[1], "raw"))
    {

        raw_dump_type = (argc >= 3) ? (uint8_t)atoi(argv[2]) : SC_DATA_TYPE_JPEG;
        raw_dump_count = (argc >= 4) ? (uint16_t)atoi(argv[3]) : 1;
        LOG_I("raw dump armed: type %d, count %d", raw_dump_type, raw_dump_count);
        return 0;
    }
    if (argc < 3) return -1;
    if (!strcmp(argv[1], "rsp_freq"))
    {
        jpeg_rsp_req = atoi(argv[2]);
        LOG_I("jpeg_rsp_req:%d", jpeg_rsp_req);
    }
    else if (!strcmp(argv[1], "log"))
    {
        log_on = atoi(argv[2]);
        LOG_I("log_on:%d", log_on);
    }
    else if (!strcmp(argv[1], "mode"))
    {
        uint8_t mode = atoi(argv[2]);
        sc_set_switch_mode(mode);
    }
    else if (!strcmp(argv[1], "dump"))
    {
        dump_on = atoi(argv[2]);
        LOG_I("dump_on:%d", dump_on);
    }
    return 0;
}

MSH_CMD_EXPORT(sc_cast, screen cast cmd);

#endif
