#include "xiaozhi_audio.h"

#include <string.h>

#include <opus.h>
#include <audio_server.h>
#include <mem_section.h>
#include <rtthread.h>

#include "xiaozhi_audio_format.h"

#define AI_XIAOZHI_AUDIO_UPLINK_RATE 16000U
#define AI_XIAOZHI_AUDIO_UPLINK_DURATION_MS 60U
#define AI_XIAOZHI_AUDIO_UPLINK_BYTES 1920U
#define AI_XIAOZHI_AUDIO_DOWNLINK_PACKET_MAX 1275U
#define AI_XIAOZHI_AUDIO_DOWNLINK_QUEUE_DEPTH 4U
#define AI_XIAOZHI_AUDIO_EVENT_UPLINK (1U << 0)
#define AI_XIAOZHI_AUDIO_EVENT_DOWNLINK (1U << 1)
#define AI_XIAOZHI_AUDIO_EVENT_STOP (1U << 2)
#define AI_XIAOZHI_AUDIO_EVENT_INITIALIZED (1U << 3)
#define AI_XIAOZHI_AUDIO_INITIALIZE_TIMEOUT_MS 1000U
#define AI_XIAOZHI_OPUS_HEAP_SIZE (64U * 1024U)
#define AI_XIAOZHI_AUDIO_SPEAKER_VOLUME 15U

typedef struct
{
    uint16_t size;
    uint8_t data[AI_XIAOZHI_AUDIO_DOWNLINK_PACKET_MAX];
} ai_xiaozhi_audio_packet_t;

typedef struct
{
    rt_bool_t configured;
    volatile rt_bool_t listening;
    volatile rt_bool_t running;
    volatile rt_bool_t worker_ready;
    uint16_t uplink_samples;
    uint16_t downlink_samples;
    uint32_t downlink_rate;
    struct rt_ringbuffer *uplink;
    rt_mq_t downlink;
    rt_event_t events;
    rt_thread_t worker;
    struct rt_thread worker_object;
    audio_client_t microphone;
    audio_client_t speaker;
    ai_xiaozhi_audio_send_t send;
    void *send_context;
    volatile uint32_t microphone_callbacks;
    volatile uint32_t microphone_bytes;
    volatile uint32_t microphone_dropped_bytes;
    uint32_t uplink_frames;
    uint32_t downlink_packets;
    uint32_t speaker_frames;
} ai_xiaozhi_audio_context_t;

static ai_xiaozhi_audio_context_t s_audio;
static struct rt_memheap s_ai_xiaozhi_opus_heap;
static rt_bool_t s_ai_xiaozhi_opus_heap_initialized;

#if defined(__CC_ARM) || defined(__CLANG_ARM)
L2_RET_BSS_SECT_BEGIN(ai_xiaozhi_opus_memory)
static uint32_t s_ai_xiaozhi_opus_stack[
    AI_XIAOZHI_AUDIO_WORKER_STACK_SIZE / sizeof(uint32_t)];
static uint32_t s_ai_xiaozhi_opus_heap_pool[
    AI_XIAOZHI_OPUS_HEAP_SIZE / sizeof(uint32_t)];
L2_RET_BSS_SECT_END
#else
static uint32_t s_ai_xiaozhi_opus_stack[
    AI_XIAOZHI_AUDIO_WORKER_STACK_SIZE / sizeof(uint32_t)]
    L2_RET_BSS_SECT(s_ai_xiaozhi_opus_stack);
static uint32_t s_ai_xiaozhi_opus_heap_pool[
    AI_XIAOZHI_OPUS_HEAP_SIZE / sizeof(uint32_t)]
    L2_RET_BSS_SECT(s_ai_xiaozhi_opus_heap_pool);
#endif

static int ai_xiaozhi_opus_heap_init(void)
{
    rt_err_t result;

    if (s_ai_xiaozhi_opus_heap_initialized)
        return 0;
    result = rt_memheap_init(&s_ai_xiaozhi_opus_heap, "xz_opus_mem",
                             s_ai_xiaozhi_opus_heap_pool,
                             sizeof(s_ai_xiaozhi_opus_heap_pool));
    if (result != RT_EOK)
    {
        rt_kprintf("xiaozhi: Opus heap initialization failed %d\n", result);
        return -1;
    }
    s_ai_xiaozhi_opus_heap_initialized = RT_TRUE;
    return 0;
}

void *opus_heap_malloc(uint32_t size)
{
    return s_ai_xiaozhi_opus_heap_initialized
               ? rt_memheap_alloc(&s_ai_xiaozhi_opus_heap, size)
               : RT_NULL;
}

void opus_heap_free(void *pointer)
{
    if (pointer != RT_NULL)
        rt_memheap_free(pointer);
}

static int ai_xiaozhi_audio_microphone_callback(
    audio_server_callback_cmt_t command, void *context, uint32_t reserved)
{
    audio_server_coming_data_t *incoming;
    rt_size_t copied;

    (void)context;
    if (command != as_callback_cmd_data_coming || !s_audio.listening ||
        s_audio.uplink == RT_NULL)
        return 0;
    incoming = (audio_server_coming_data_t *)reserved;
    if (incoming == RT_NULL || incoming->data == RT_NULL ||
        incoming->data_len == 0U)
        return 0;
    s_audio.microphone_callbacks++;
    s_audio.microphone_bytes += incoming->data_len;
    copied = rt_ringbuffer_put(s_audio.uplink, incoming->data,
                               incoming->data_len);
    if (copied != incoming->data_len)
        s_audio.microphone_dropped_bytes += incoming->data_len - copied;
    if (copied == incoming->data_len &&
        rt_ringbuffer_data_len(s_audio.uplink) >= AI_XIAOZHI_AUDIO_UPLINK_BYTES)
        rt_event_send(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_UPLINK);
    return 0;
}

static int ai_xiaozhi_audio_open_speaker(void)
{
    audio_parameter_t parameter = {0};
    uint32_t cache_size;

    if (s_audio.speaker != RT_NULL)
        return 0;
    cache_size = ai_xiaozhi_audio_speaker_cache_bytes(
        s_audio.downlink_samples);
    if (cache_size == 0U)
    {
        rt_kprintf("xiaozhi: unsupported downlink frame %u samples\n",
                   (unsigned int)s_audio.downlink_samples);
        return -1;
    }
    parameter.write_bits_per_sample = 16U;
    parameter.write_channnel_num = 1U;
    parameter.write_samplerate = s_audio.downlink_rate;
    parameter.write_cache_size = cache_size;
    (void)audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC,
                                          AI_XIAOZHI_AUDIO_SPEAKER_VOLUME);
    s_audio.speaker = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &parameter,
                                 RT_NULL, RT_NULL);
    if (s_audio.speaker == RT_NULL)
        rt_kprintf("xiaozhi: speaker open failed\n");
    else
        rt_kprintf("xiaozhi: speaker opened %u Hz\n",
                   (unsigned int)s_audio.downlink_rate);
    return s_audio.speaker == RT_NULL ? -1 : 0;
}

static void ai_xiaozhi_audio_write_speaker(const uint8_t *data, size_t size)
{
    size_t written = 0U;
    unsigned int retries = 0U;

    if (ai_xiaozhi_audio_open_speaker() < 0)
        return;
    while (written < size && retries++ < 10U)
    {
        int result = audio_write(s_audio.speaker, (uint8_t *)data + written,
                                 (uint32_t)(size - written));

        if (result > 0)
        {
            written += (size_t)result;
            continue;
        }
        rt_thread_mdelay(10);
    }
    if (written != size)
        rt_kprintf("xiaozhi: speaker write short %u/%u bytes\\n",
                   (unsigned int)written, (unsigned int)size);
}

static void ai_xiaozhi_audio_process_uplink(OpusEncoder *encoder)
{
    uint8_t pcm[AI_XIAOZHI_AUDIO_UPLINK_BYTES];
    uint8_t packet[AI_XIAOZHI_AUDIO_DOWNLINK_PACKET_MAX];

    while (s_audio.listening && s_audio.uplink != RT_NULL &&
           rt_ringbuffer_data_len(s_audio.uplink) >= sizeof(pcm))
    {
        int encoded;
        int send_result = -1;

        if (rt_ringbuffer_get(s_audio.uplink, pcm, sizeof(pcm)) != sizeof(pcm))
            return;
        encoded = opus_encode(encoder, (const opus_int16 *)pcm,
                              s_audio.uplink_samples, packet,
                              sizeof(packet));
        if (encoded > 0 && s_audio.send != RT_NULL)
            send_result = s_audio.send(s_audio.send_context, packet,
                                       (size_t)encoded);
        s_audio.uplink_frames++;
        if (ai_xiaozhi_audio_should_log_frame(s_audio.uplink_frames))
            rt_kprintf("xiaozhi: uplink frame=%u mic=%uB/%u cb drop=%uB opus=%d send=%d\n",
                       (unsigned int)s_audio.uplink_frames,
                       (unsigned int)s_audio.microphone_bytes,
                       (unsigned int)s_audio.microphone_callbacks, encoded,
                       (unsigned int)s_audio.microphone_dropped_bytes,
                       send_result);
        if (encoded <= 0)
            rt_kprintf("xiaozhi: Opus encode failed %d\n", encoded);
        else if (send_result != 0)
            rt_kprintf("xiaozhi: audio uplink send failed\n");
    }
}

static void ai_xiaozhi_audio_process_downlink(OpusDecoder *decoder)
{
    ai_xiaozhi_audio_packet_t packet;
    uint8_t pcm[AI_XIAOZHI_AUDIO_DOWNLINK_PCM_MAX_BYTES];

    while (s_audio.downlink != RT_NULL &&
           rt_mq_recv(s_audio.downlink, &packet, sizeof(packet), 0) == RT_EOK)
    {
        int decoded = opus_decode(decoder, packet.data, packet.size,
                                  (opus_int16 *)pcm, s_audio.downlink_samples,
                                  0);

        if (decoded > 0)
        {
            uint16_t peak = ai_xiaozhi_audio_pcm_peak(
                (const int16_t *)pcm, (uint16_t)decoded);

            s_audio.speaker_frames++;
            if (ai_xiaozhi_audio_should_log_frame(s_audio.speaker_frames))
                rt_kprintf("xiaozhi: downlink frame=%u packet=%uB pcm=%uB peak=%u\n",
                           (unsigned int)s_audio.speaker_frames,
                           (unsigned int)packet.size,
                           (unsigned int)decoded * 2U,
                           (unsigned int)peak);
            ai_xiaozhi_audio_write_speaker(pcm, (size_t)decoded * 2U);
        }
        else
            rt_kprintf("xiaozhi: Opus decode failed %d (%u bytes)\n", decoded,
                       (unsigned int)packet.size);
    }
}

static void ai_xiaozhi_audio_worker(void *parameter)
{
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    int encoder_error = OPUS_OK;
    int decoder_error = OPUS_OK;

    (void)parameter;
    encoder = opus_encoder_create(AI_XIAOZHI_AUDIO_UPLINK_RATE, 1,
                                  OPUS_APPLICATION_VOIP, &encoder_error);
    decoder = opus_decoder_create((opus_int32)s_audio.downlink_rate, 1,
                                  &decoder_error);
    if (encoder == RT_NULL || decoder == RT_NULL)
    {
        rt_kprintf("xiaozhi: Opus initialization failed encoder=%d decoder=%d\n",
                   encoder_error, decoder_error);
        if (encoder != RT_NULL)
            opus_encoder_destroy(encoder);
        if (decoder != RT_NULL)
            opus_decoder_destroy(decoder);
        s_audio.worker_ready = RT_FALSE;
        s_audio.running = RT_FALSE;
        rt_event_send(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_INITIALIZED);
        return;
    }
    opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(
                                      OPUS_FRAMESIZE_60_MS));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    s_audio.worker_ready = RT_TRUE;
    rt_event_send(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_INITIALIZED);

    while (s_audio.running)
    {
        rt_uint32_t events = 0U;

        if (rt_event_recv(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_UPLINK |
                                             AI_XIAOZHI_AUDIO_EVENT_DOWNLINK |
                                             AI_XIAOZHI_AUDIO_EVENT_STOP,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &events) != RT_EOK)
            continue;
        if ((events & AI_XIAOZHI_AUDIO_EVENT_STOP) != 0U)
            break;
        if ((events & AI_XIAOZHI_AUDIO_EVENT_UPLINK) != 0U)
            ai_xiaozhi_audio_process_uplink(encoder);
        if ((events & AI_XIAOZHI_AUDIO_EVENT_DOWNLINK) != 0U)
            ai_xiaozhi_audio_process_downlink(decoder);
    }

    opus_encoder_destroy(encoder);
    opus_decoder_destroy(decoder);
    s_audio.worker_ready = RT_FALSE;
    s_audio.running = RT_FALSE;
}

static void ai_xiaozhi_audio_release_worker_resources(void)
{
    if (s_audio.uplink != RT_NULL)
        rt_ringbuffer_destroy(s_audio.uplink);
    if (s_audio.downlink != RT_NULL)
        rt_mq_delete(s_audio.downlink);
    if (s_audio.events != RT_NULL)
        rt_event_delete(s_audio.events);
    s_audio.uplink = RT_NULL;
    s_audio.downlink = RT_NULL;
    s_audio.events = RT_NULL;
    s_audio.worker = RT_NULL;
    s_audio.worker_ready = RT_FALSE;
}

static void ai_xiaozhi_audio_wait_worker_exit(void)
{
    while (s_audio.running)
        rt_thread_mdelay(1);

    /* A static thread is detached by rt_thread_exit() after the entry
     * function returns. Keep its object alive until that handoff completes. */
    while (s_audio.worker != RT_NULL &&
           rt_object_get_type((rt_object_t)s_audio.worker) != 0U)
        rt_thread_mdelay(1);
}

static int ai_xiaozhi_audio_start_worker(void)
{
    ai_xiaozhi_audio_resource_t failed_resource;
    rt_err_t thread_result;
    rt_uint32_t events;

    if (s_audio.running)
        return s_audio.worker_ready ? 0 : -1;
    if (s_audio.uplink != RT_NULL || s_audio.downlink != RT_NULL ||
        s_audio.events != RT_NULL)
    {
        ai_xiaozhi_audio_wait_worker_exit();
        ai_xiaozhi_audio_release_worker_resources();
    }
    if (ai_xiaozhi_opus_heap_init() < 0)
    {
        failed_resource = AI_XIAOZHI_AUDIO_RESOURCE_OPUS;
        goto failed;
    }

    s_audio.uplink = rt_ringbuffer_create(AI_XIAOZHI_AUDIO_UPLINK_BYTES * 3U);
    if (s_audio.uplink == RT_NULL)
    {
        failed_resource = AI_XIAOZHI_AUDIO_RESOURCE_RINGBUFFER;
        goto failed;
    }
    s_audio.downlink = rt_mq_create("xz_aud_q", sizeof(ai_xiaozhi_audio_packet_t),
                                    AI_XIAOZHI_AUDIO_DOWNLINK_QUEUE_DEPTH,
                                    RT_IPC_FLAG_FIFO);
    if (s_audio.downlink == RT_NULL)
    {
        failed_resource = AI_XIAOZHI_AUDIO_RESOURCE_MESSAGE_QUEUE;
        goto failed;
    }
    s_audio.events = rt_event_create("xz_aud", RT_IPC_FLAG_PRIO);
    if (s_audio.events == RT_NULL)
    {
        failed_resource = AI_XIAOZHI_AUDIO_RESOURCE_EVENT;
        goto failed;
    }
    thread_result = rt_thread_init(&s_audio.worker_object, "xz_opus",
                                   ai_xiaozhi_audio_worker, RT_NULL,
                                   s_ai_xiaozhi_opus_stack,
                                   sizeof(s_ai_xiaozhi_opus_stack), 18, 10);
    if (thread_result != RT_EOK)
    {
        failed_resource = AI_XIAOZHI_AUDIO_RESOURCE_THREAD;
        goto failed;
    }
    s_audio.worker = &s_audio.worker_object;
    s_audio.running = RT_TRUE;
    s_audio.worker_ready = RT_FALSE;
    rt_thread_startup(s_audio.worker);
    if (rt_event_recv(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_INITIALIZED,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      rt_tick_from_millisecond(
                          AI_XIAOZHI_AUDIO_INITIALIZE_TIMEOUT_MS),
                      &events) != RT_EOK || !s_audio.worker_ready)
    {
        failed_resource = AI_XIAOZHI_AUDIO_RESOURCE_OPUS;
        s_audio.running = RT_FALSE;
        rt_event_send(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_STOP);
        ai_xiaozhi_audio_wait_worker_exit();
        goto failed;
    }
    return 0;

failed:
    rt_kprintf("xiaozhi: audio worker setup failed at %s\n",
               ai_xiaozhi_audio_resource_name(failed_resource));
    ai_xiaozhi_audio_release_worker_resources();
    return -1;
}

int ai_xiaozhi_audio_configure(const ai_xiaozhi_server_hello_t *hello)
{
    uint16_t samples;

    if (hello == RT_NULL || strcmp(hello->audio_format, "opus") != 0 ||
        hello->channels != 1U ||
        ai_xiaozhi_audio_frame_samples(hello->sample_rate,
                                       hello->frame_duration, &samples) < 0 ||
        ai_xiaozhi_audio_speaker_cache_bytes(samples) == 0U ||
        ai_xiaozhi_audio_frame_samples(AI_XIAOZHI_AUDIO_UPLINK_RATE,
                                       AI_XIAOZHI_AUDIO_UPLINK_DURATION_MS,
                                       &s_audio.uplink_samples) < 0)
        return -1;
    if (s_audio.running && (s_audio.downlink_rate != hello->sample_rate ||
                            s_audio.downlink_samples != samples))
        return -1;
    s_audio.downlink_rate = hello->sample_rate;
    s_audio.downlink_samples = samples;
    s_audio.configured = RT_FALSE;
    if (ai_xiaozhi_audio_start_worker() < 0)
        return -1;
    s_audio.configured = RT_TRUE;
    return 0;
}

int ai_xiaozhi_audio_listen_start(ai_xiaozhi_audio_send_t send,
                                  void *send_context)
{
    audio_parameter_t parameter = {0};

    if (send == RT_NULL || ai_xiaozhi_audio_start_worker() < 0 ||
        !ai_xiaozhi_audio_worker_can_listen(s_audio.configured,
                                             s_audio.worker_ready))
        return -1;
    if (s_audio.microphone != RT_NULL)
        return 0;
    parameter.read_bits_per_sample = 16U;
    parameter.read_channnel_num = 1U;
    parameter.read_samplerate = AI_XIAOZHI_AUDIO_UPLINK_RATE;
    parameter.read_cache_size = 0U;
    parameter.read_which_mic = AUDIO_MIC0_ONLY;
    s_audio.send = send;
    s_audio.send_context = send_context;
    s_audio.microphone_callbacks = 0U;
    s_audio.microphone_bytes = 0U;
    s_audio.microphone_dropped_bytes = 0U;
    s_audio.uplink_frames = 0U;
    s_audio.downlink_packets = 0U;
    s_audio.speaker_frames = 0U;
    s_audio.microphone = audio_open(AUDIO_TYPE_LOCAL_RECORD, AUDIO_RX,
                                    &parameter,
                                    ai_xiaozhi_audio_microphone_callback,
                                    RT_NULL);
    if (s_audio.microphone == RT_NULL)
        return -1;
    rt_ringbuffer_reset(s_audio.uplink);
    s_audio.listening = RT_TRUE;
    return 0;
}

void ai_xiaozhi_audio_listen_stop(void)
{
    s_audio.listening = RT_FALSE;
    if (s_audio.microphone != RT_NULL)
    {
        audio_close(s_audio.microphone);
        s_audio.microphone = RT_NULL;
    }
    if (s_audio.uplink != RT_NULL)
        rt_ringbuffer_reset(s_audio.uplink);
}

int ai_xiaozhi_audio_receive(const uint8_t *data, size_t size)
{
    ai_xiaozhi_audio_packet_t packet;

    if (!s_audio.running || data == RT_NULL || size == 0U ||
        size > sizeof(packet.data))
        return -1;
    packet.size = (uint16_t)size;
    rt_memcpy(packet.data, data, size);
    s_audio.downlink_packets++;
    if (rt_mq_send(s_audio.downlink, &packet, sizeof(packet)) != RT_EOK)
        return -1;
    rt_event_send(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_DOWNLINK);
    return 0;
}

void ai_xiaozhi_audio_disconnect(void)
{
    ai_xiaozhi_audio_listen_stop();
    if (s_audio.events != RT_NULL)
        rt_event_send(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_STOP);
    ai_xiaozhi_audio_wait_worker_exit();
    if (s_audio.speaker != RT_NULL)
        audio_close(s_audio.speaker);
    ai_xiaozhi_audio_release_worker_resources();
    rt_memset(&s_audio, 0, sizeof(s_audio));
}
