#include "xiaozhi_audio.h"

#include <string.h>

#include <opus.h>
#include <audio_server.h>
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
    uint16_t uplink_samples;
    uint16_t downlink_samples;
    uint32_t downlink_rate;
    struct rt_ringbuffer *uplink;
    rt_mq_t downlink;
    rt_event_t events;
    rt_thread_t worker;
    audio_client_t microphone;
    audio_client_t speaker;
    ai_xiaozhi_audio_send_t send;
    void *send_context;
} ai_xiaozhi_audio_context_t;

static ai_xiaozhi_audio_context_t s_audio;

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
    copied = rt_ringbuffer_put(s_audio.uplink, incoming->data,
                               incoming->data_len);
    if (copied == incoming->data_len &&
        rt_ringbuffer_data_len(s_audio.uplink) >= AI_XIAOZHI_AUDIO_UPLINK_BYTES)
        rt_event_send(s_audio.events, AI_XIAOZHI_AUDIO_EVENT_UPLINK);
    return 0;
}

static int ai_xiaozhi_audio_open_speaker(void)
{
    audio_parameter_t parameter = {0};

    if (s_audio.speaker != RT_NULL)
        return 0;
    parameter.write_bits_per_sample = 16U;
    parameter.write_channnel_num = 1U;
    parameter.write_samplerate = s_audio.downlink_rate;
    parameter.write_cache_size = 8192U;
    s_audio.speaker = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &parameter,
                                 RT_NULL, RT_NULL);
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
}

static void ai_xiaozhi_audio_process_uplink(OpusEncoder *encoder)
{
    uint8_t pcm[AI_XIAOZHI_AUDIO_UPLINK_BYTES];
    uint8_t packet[AI_XIAOZHI_AUDIO_DOWNLINK_PACKET_MAX];

    while (s_audio.listening && s_audio.uplink != RT_NULL &&
           rt_ringbuffer_data_len(s_audio.uplink) >= sizeof(pcm))
    {
        int encoded;

        if (rt_ringbuffer_get(s_audio.uplink, pcm, sizeof(pcm)) != sizeof(pcm))
            return;
        encoded = opus_encode(encoder, (const opus_int16 *)pcm,
                              s_audio.uplink_samples, packet,
                              sizeof(packet));
        if (encoded > 0 && s_audio.send != RT_NULL &&
            s_audio.send(s_audio.send_context, packet, (size_t)encoded) != 0)
            rt_kprintf("xiaozhi: audio uplink send failed\n");
    }
}

static void ai_xiaozhi_audio_process_downlink(OpusDecoder *decoder)
{
    ai_xiaozhi_audio_packet_t packet;
    uint8_t pcm[2880U];

    while (s_audio.downlink != RT_NULL &&
           rt_mq_recv(s_audio.downlink, &packet, sizeof(packet), 0) == RT_EOK)
    {
        int decoded = opus_decode(decoder, packet.data, packet.size,
                                  (opus_int16 *)pcm, s_audio.downlink_samples,
                                  0);

        if (decoded > 0)
            ai_xiaozhi_audio_write_speaker(pcm, (size_t)decoded * 2U);
    }
}

static void ai_xiaozhi_audio_worker(void *parameter)
{
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    int error;

    (void)parameter;
    encoder = opus_encoder_create(AI_XIAOZHI_AUDIO_UPLINK_RATE, 1,
                                  OPUS_APPLICATION_VOIP, &error);
    decoder = opus_decoder_create((opus_int32)s_audio.downlink_rate, 1, &error);
    if (encoder == RT_NULL || decoder == RT_NULL)
    {
        rt_kprintf("xiaozhi: Opus initialization failed\n");
        if (encoder != RT_NULL)
            opus_encoder_destroy(encoder);
        if (decoder != RT_NULL)
            opus_decoder_destroy(decoder);
        s_audio.running = RT_FALSE;
        return;
    }
    opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(
                                      OPUS_FRAMESIZE_60_MS));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));

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
    s_audio.running = RT_FALSE;
}

static int ai_xiaozhi_audio_start_worker(void)
{
    if (s_audio.running)
        return 0;
    s_audio.uplink = rt_ringbuffer_create(AI_XIAOZHI_AUDIO_UPLINK_BYTES * 3U);
    s_audio.downlink = rt_mq_create("xz_aud_q", sizeof(ai_xiaozhi_audio_packet_t),
                                    AI_XIAOZHI_AUDIO_DOWNLINK_QUEUE_DEPTH,
                                    RT_IPC_FLAG_FIFO);
    s_audio.events = rt_event_create("xz_aud", RT_IPC_FLAG_PRIO);
    if (s_audio.uplink == RT_NULL || s_audio.downlink == RT_NULL ||
        s_audio.events == RT_NULL)
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
        return -1;
    }
    s_audio.running = RT_TRUE;
    s_audio.worker = rt_thread_create("xz_opus", ai_xiaozhi_audio_worker,
                                      RT_NULL, 4096, 18, 10);
    if (s_audio.worker == RT_NULL)
    {
        s_audio.running = RT_FALSE;
        rt_ringbuffer_destroy(s_audio.uplink);
        rt_mq_delete(s_audio.downlink);
        rt_event_delete(s_audio.events);
        s_audio.uplink = RT_NULL;
        s_audio.downlink = RT_NULL;
        s_audio.events = RT_NULL;
        return -1;
    }
    rt_thread_startup(s_audio.worker);
    return 0;
}

int ai_xiaozhi_audio_configure(const ai_xiaozhi_server_hello_t *hello)
{
    uint16_t samples;

    if (hello == RT_NULL || strcmp(hello->audio_format, "opus") != 0 ||
        hello->channels != 1U ||
        ai_xiaozhi_audio_frame_samples(hello->sample_rate,
                                       hello->frame_duration, &samples) < 0 ||
        ai_xiaozhi_audio_frame_samples(AI_XIAOZHI_AUDIO_UPLINK_RATE,
                                       AI_XIAOZHI_AUDIO_UPLINK_DURATION_MS,
                                       &s_audio.uplink_samples) < 0)
        return -1;
    if (s_audio.running && (s_audio.downlink_rate != hello->sample_rate ||
                            s_audio.downlink_samples != samples))
        return -1;
    s_audio.downlink_rate = hello->sample_rate;
    s_audio.downlink_samples = samples;
    s_audio.configured = RT_TRUE;
    return ai_xiaozhi_audio_start_worker();
}

int ai_xiaozhi_audio_listen_start(ai_xiaozhi_audio_send_t send,
                                  void *send_context)
{
    audio_parameter_t parameter = {0};

    if (!s_audio.configured || send == RT_NULL ||
        ai_xiaozhi_audio_start_worker() < 0)
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
    while (s_audio.running)
        rt_thread_mdelay(1);
    if (s_audio.speaker != RT_NULL)
        audio_close(s_audio.speaker);
    if (s_audio.uplink != RT_NULL)
        rt_ringbuffer_destroy(s_audio.uplink);
    if (s_audio.downlink != RT_NULL)
        rt_mq_delete(s_audio.downlink);
    if (s_audio.events != RT_NULL)
        rt_event_delete(s_audio.events);
    rt_memset(&s_audio, 0, sizeof(s_audio));
}
