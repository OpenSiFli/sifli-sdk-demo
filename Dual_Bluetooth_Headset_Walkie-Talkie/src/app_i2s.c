/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>
#include <drivers/audio.h>
#include <ipc/ringbuffer.h>

#include "bf0_hal.h"
#include "drv_gpio.h"
#include "drv_i2s_audio.h"
#include "drv_io.h"

#include "app_i2s.h"

#if defined(SF32LB57X)
#define APP_I2S_DEVICE_NAME "i2s1"
#else
#define APP_I2S_DEVICE_NAME "i2s2"
#endif
#define APP_I2S_EVENT_DATA (1U << 0)
#define APP_PCM_FRAME_BYTES (APP_PCM_CHANNELS * sizeof(int16_t))

static rt_device_t g_i2s_device;
static rt_event_t g_i2s_event;
static rt_mutex_t g_i2s_lock;
static struct rt_ringbuffer g_rx_ring;
ALIGN(4) static uint8_t g_rx_pool[APP_I2S_RX_RING_SIZE];
static app_i2s_stats_t g_stats;
static volatile bool g_rx_enabled;
static bool g_rx_running;
static bool g_i2s_device_opened;
static volatile bool g_i2s_initialized;
#if defined(SF32LB57X)
#include "board_pins.h"
#endif

static void app_i2s_config_pins(void)
{
#if defined(SF32LB57X)
    /*
     * The relay is an I2S slave: only SDI/BCK/LRCK are inputs.  MCLK and SDO
     * are driven by the source board, so keep them as high-impedance inputs
     * here to avoid fighting the source.
     */
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_MCLK_PIN),
                BOARD_GPIO(BOARD_57Z_I2S_MCLK_PIN), PIN_PULLDOWN, 1);
    rt_pin_mode(GET_PIN(1, BOARD_57Z_I2S_MCLK_PIN),
                PIN_MODE_INPUT_PULLDOWN);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_SDO_PIN),
                BOARD_GPIO(BOARD_57Z_I2S_SDO_PIN), PIN_PULLDOWN, 1);
    rt_pin_mode(GET_PIN(1, BOARD_57Z_I2S_SDO_PIN),
                PIN_MODE_INPUT_PULLDOWN);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_SDI_PIN), I2S1_SDI,
                PIN_PULLDOWN, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_BCK_PIN), I2S1_BCK,
                PIN_NOPULL, 1);
    HAL_PIN_Set(BOARD_PAD(BOARD_57Z_I2S_LRCK_PIN), I2S1_LRCK,
                PIN_NOPULL, 1);
#else
    HAL_PIN_Set(PAD_PA24, GPIO_A24, PIN_PULLDOWN, 1);
    rt_pin_mode(GET_PIN(1, 24), PIN_MODE_INPUT_PULLDOWN);
    HAL_PIN_Set(PAD_PA25, GPIO_A25, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA28, I2S1_SDI, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA29, I2S1_BCK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA30, I2S1_LRCK, PIN_NOPULL, 1);
#endif
}

static int app_i2s_configure(void)
{
    struct rt_audio_caps caps = {0};
    rt_err_t error;

    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.channels = APP_PCM_CHANNELS;
    caps.udata.config.samplerate = APP_PCM_SAMPLE_RATE;
    caps.udata.config.samplefmt = APP_PCM_BITS;
    error = rt_device_control(g_i2s_device, AUDIO_CTL_CONFIGURE, &caps);
    if (error != RT_EOK)
    {
        return error;
    }

    error = rt_device_control(g_i2s_device, AUDIO_CTL_SETINPUT, (void *)0);
    if (error != RT_EOK)
    {
        return error;
    }

    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type = AUDIO_DSP_MODE;
    caps.udata.value = 1U;
    return rt_device_control(g_i2s_device, AUDIO_CTL_CONFIGURE, &caps);
}

/* The SDK invokes this callback in the DMA interrupt. */
static void app_i2s_dma_rx(char *name, uint8_t *data, rt_size_t length)
{
    rt_size_t space;

    (void)name;
    g_stats.rx_indicated_bytes += length;
    if (!g_rx_enabled)
    {
        return;
    }
    if ((length % APP_PCM_FRAME_BYTES) != 0U)
    {
        g_stats.device_errors++;
        return;
    }

    space = rt_ringbuffer_space_len(&g_rx_ring);
    if (space < length)
    {
        g_stats.rx_overflow_bytes += length - space;
    }
    rt_ringbuffer_put_force(&g_rx_ring, data, length);
    g_stats.rx_bytes += length;
    rt_event_send(g_i2s_event, APP_I2S_EVENT_DATA);
}

static void app_i2s_cleanup(void)
{
    g_i2s_initialized = false;
    g_rx_enabled = false;
    if (g_rx_running && g_i2s_device_opened)
    {
        int stream = AUDIO_STREAM_RECORD;
        rt_device_control(g_i2s_device, AUDIO_CTL_STOP, &stream);
    }
    g_rx_running = false;

    if (g_i2s_device_opened)
    {
        rt_device_set_i2s_dma_rx_callback(g_i2s_device, RT_NULL);
        rt_device_close(g_i2s_device);
    }
    g_i2s_device_opened = false;
    g_i2s_device = RT_NULL;

    if (g_i2s_event != RT_NULL)
    {
        rt_event_delete(g_i2s_event);
        g_i2s_event = RT_NULL;
    }
    if (g_i2s_lock != RT_NULL)
    {
        rt_mutex_delete(g_i2s_lock);
        g_i2s_lock = RT_NULL;
    }
}

int app_i2s_init(void)
{
    rt_err_t error;
    int stream = AUDIO_STREAM_RECORD;

    if (g_i2s_initialized)
    {
        return RT_EOK;
    }
    app_i2s_cleanup();
    memset(&g_stats, 0, sizeof(g_stats));
    app_i2s_config_pins();

    g_i2s_event = rt_event_create("i2se", RT_IPC_FLAG_FIFO);
    g_i2s_lock = rt_mutex_create("i2srb", RT_IPC_FLAG_FIFO);
    if ((g_i2s_event == RT_NULL) || (g_i2s_lock == RT_NULL))
    {
        error = -RT_ENOMEM;
        goto failed;
    }
    rt_ringbuffer_init(&g_rx_ring, g_rx_pool, sizeof(g_rx_pool));

    g_i2s_device = rt_device_find(APP_I2S_DEVICE_NAME);
    if (g_i2s_device == RT_NULL)
    {
        error = -RT_ENOSYS;
        goto failed;
    }

    error = rt_device_open(g_i2s_device, RT_DEVICE_OFLAG_RDONLY);
    if (error != RT_EOK)
    {
        goto failed;
    }
    g_i2s_device_opened = true;

    error = app_i2s_configure();
    if (error != RT_EOK)
    {
        goto failed;
    }

    /* The driver initializes HAL at boot. STOP resets it so the first
     * capture START also applies the board's MSP clock configuration. */
    error = rt_device_control(g_i2s_device, AUDIO_CTL_STOP, &stream);
    if (error != RT_EOK)
    {
        goto failed;
    }
    rt_device_set_i2s_dma_rx_callback(g_i2s_device, app_i2s_dma_rx);

    g_i2s_initialized = true;
    return RT_EOK;

failed:
    app_i2s_cleanup();
    return error;
}

bool app_i2s_is_ready(void)
{
    return g_i2s_initialized;
}

int app_i2s_set_rx_enabled(bool enabled)
{
    int stream = AUDIO_STREAM_RECORD;
    int error = RT_EOK;
    rt_base_t level;

    if (!g_i2s_initialized)
    {
        return -RT_ENOSYS;
    }

    rt_mutex_take(g_i2s_lock, RT_WAITING_FOREVER);
    if (enabled && !g_rx_running)
    {
        error = rt_device_control(g_i2s_device, AUDIO_CTL_START, &stream);
        if (error == RT_EOK)
        {
            g_rx_running = true;
        }
        else
        {
            rt_device_control(g_i2s_device, AUDIO_CTL_STOP, &stream);
        }
    }
    else if (!enabled && g_rx_running)
    {
        g_rx_enabled = false;
        error = rt_device_control(g_i2s_device, AUDIO_CTL_STOP, &stream);
        if (error == RT_EOK)
        {
            g_rx_running = false;
        }
    }

    level = rt_hw_interrupt_disable();
    g_rx_enabled = enabled && g_rx_running && (error == RT_EOK);
    if (!g_rx_enabled)
    {
        rt_ringbuffer_reset(&g_rx_ring);
    }
    if (error != RT_EOK)
    {
        g_stats.device_errors++;
    }
    rt_hw_interrupt_enable(level);
    rt_mutex_release(g_i2s_lock);
    rt_event_send(g_i2s_event, APP_I2S_EVENT_DATA);
    return error;
}

rt_size_t app_i2s_rx_buffered(void)
{
    rt_size_t length;
    rt_base_t level;

    if (!g_i2s_initialized)
    {
        return 0U;
    }

    level = rt_hw_interrupt_disable();
    length = g_rx_enabled ? rt_ringbuffer_data_len(&g_rx_ring) : 0U;
    rt_hw_interrupt_enable(level);
    return length;
}

rt_size_t app_i2s_read(void *buffer, rt_size_t size, uint32_t timeout_ms)
{
    rt_tick_t timeout;
    rt_tick_t start;

    if (!g_i2s_initialized ||
            (buffer == RT_NULL) || (size == 0U) ||
            ((size % APP_I2S_DMA_HALF_SIZE) != 0U) ||
            (size > APP_I2S_RX_RING_SIZE))
    {
        return 0U;
    }

    timeout = rt_tick_from_millisecond(timeout_ms);
    start = rt_tick_get();
    while (1)
    {
        rt_size_t length;
        rt_uint32_t events;
        rt_tick_t elapsed;
        rt_tick_t remaining;
        rt_base_t level = rt_hw_interrupt_disable();
        if (!g_rx_enabled)
        {
            rt_hw_interrupt_enable(level);
            return 0U;
        }
        if (rt_ringbuffer_data_len(&g_rx_ring) >= size)
        {
            length = rt_ringbuffer_get(&g_rx_ring, buffer, size);
            rt_hw_interrupt_enable(level);
            return length;
        }
        rt_hw_interrupt_enable(level);

        elapsed = rt_tick_get() - start;
        if (elapsed >= timeout)
        {
            g_stats.rx_underflows++;
            return 0U;
        }
        remaining = timeout - elapsed;

        rt_event_recv(g_i2s_event, APP_I2S_EVENT_DATA,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      remaining, &events);
    }
}

void app_i2s_get_stats(app_i2s_stats_t *stats)
{
    if (stats != RT_NULL)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        *stats = g_stats;
        rt_hw_interrupt_enable(level);
    }
}
