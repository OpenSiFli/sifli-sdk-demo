// SPDX-FileCopyrightText: 2021 Larry Bank <bitbank@pobox.com>
// SPDX-License-Identifier: Apache-2.0

#include "camera_sw_jpeg_encoder.h"
#include <JPEGENC.h>
#include <limits.h>
#include <new>
#include <string.h>

static_assert(sizeof(JPEGENC) <= CAMERA_SW_JPEG_ENCODER_STATIC_WORKSPACE_SIZE,
              "static JPEG encoder workspace is too small");

static JPEGENC *camera_sw_encoder(camera_sw_jpeg_encoder_t *encoder)
{
    return reinterpret_cast<JPEGENC *>(encoder->workspace);
}

static int camera_sw_jpeg_pixel_type(const camera_sw_jpeg_frame_t *frame,
                                     camera_sw_jpeg_quality_t quality,
                                     uint8_t *pixel_type)
{
    if ((frame == NULL) || (frame->data == NULL) || (frame->width == 0U) ||
        (frame->height == 0U) ||
        (frame->stride < (uint32_t)frame->width * 2U) ||
        (quality > CAMERA_SW_JPEG_QUALITY_LOW))
        return -1;
    if (frame->pixel_format == CAMERA_SW_JPEG_PIXEL_RGB565)
        *pixel_type = JPEGE_PIXEL_RGB565;
    else if (frame->pixel_format == CAMERA_SW_JPEG_PIXEL_RGB565_BE)
        *pixel_type = JPEGE_PIXEL_RGB565_BE;
    else
        return -1;
    return 0;
}

/* JPEGENC reads complete 16x16 blocks for 4:2:0. Replicate the border
 * pixels of a partial block without reading beyond the caller's frame. */
static int camera_sw_jpeg_add_frame(JPEGENC *core, JPEGENCODE *state,
                                    const camera_sw_jpeg_frame_t *frame)
{
    uint16_t edge[16U * 16U];

    if ((frame->width % 16U) == 0U && (frame->height % 16U) == 0U)
        return core->addFrame(state, frame->data, (int)frame->stride);
    for (uint32_t y = 0U; y < frame->height; y += 16U)
    {
        uint32_t rows = frame->height - y;
        if (rows > 16U)
            rows = 16U;
        for (uint32_t x = 0U; x < frame->width; x += 16U)
        {
            uint32_t columns = frame->width - x;
            uint8_t *pixels = frame->data + y * frame->stride + x * 2U;
            int stride = (int)frame->stride;
            int status;

            if (columns > 16U)
                columns = 16U;
            if (rows < 16U || columns < 16U)
            {
                for (uint32_t row = 0U; row < 16U; ++row)
                {
                    uint32_t source_row = row < rows ? row : rows - 1U;
                    const uint8_t *source = pixels + source_row * frame->stride;
                    uint8_t *destination = reinterpret_cast<uint8_t *>(&edge[row * 16U]);
                    memcpy(destination, source, columns * 2U);
                    for (uint32_t column = columns; column < 16U; ++column)
                        memcpy(destination + column * 2U, source + (columns - 1U) * 2U, 2U);
                }
                pixels = reinterpret_cast<uint8_t *>(edge);
                stride = 16U * 2U;
            }
            status = core->addMCU(state, pixels, stride);
            if (status != JPEGE_SUCCESS)
                return status;
        }
    }
    return JPEGE_SUCCESS;
}

extern "C" uint32_t camera_sw_jpeg_encoder_workspace_size(void)
{
    return (uint32_t)sizeof(JPEGENC);
}

extern "C" int camera_sw_jpeg_encoder_init(camera_sw_jpeg_encoder_t *encoder,
                                             void *workspace,
                                             uint32_t workspace_size)
{
    if ((encoder == NULL) || (workspace == NULL) ||
        (workspace_size < sizeof(JPEGENC)) ||
        ((reinterpret_cast<uintptr_t>(workspace) % alignof(JPEGENC)) != 0U))
        return -1;
    encoder->workspace = workspace;
    encoder->workspace_size = workspace_size;
    new (workspace) JPEGENC();
    return 0;
}

extern "C" int camera_sw_jpeg_encode_to_buffer(
    camera_sw_jpeg_encoder_t *encoder, const camera_sw_jpeg_frame_t *frame,
    camera_sw_jpeg_quality_t quality, uint8_t *output,
    uint32_t output_capacity, uint32_t *output_size)
{
    JPEGENCODE state;
    uint8_t pixel_type;
    int size;

    if ((encoder == NULL) || (encoder->workspace == NULL) ||
        (encoder->workspace_size < sizeof(JPEGENC)) || (output == NULL) ||
        (output_size == NULL) || (output_capacity > (uint32_t)INT_MAX) ||
        (output_capacity < 1024U) ||
        (camera_sw_jpeg_pixel_type(frame, quality, &pixel_type) != 0))
        return -1;
    *output_size = 0U;
    JPEGENC *core = camera_sw_encoder(encoder);
    if ((core->open(output, (int)output_capacity) != JPEGE_SUCCESS) ||
        (core->encodeBegin(&state, frame->width, frame->height, pixel_type,
                           JPEGE_SUBSAMPLE_420, (uint8_t)quality) != JPEGE_SUCCESS) ||
        (camera_sw_jpeg_add_frame(core, &state, frame) != JPEGE_SUCCESS))
        return -1;
    size = core->close();
    if (size <= 0)
        return -1;
    *output_size = (uint32_t)size;
    return 0;
}

extern "C" int camera_sw_jpeg_encode_to_sink(
    camera_sw_jpeg_encoder_t *encoder, const camera_sw_jpeg_frame_t *frame,
    camera_sw_jpeg_quality_t quality, camera_sw_jpeg_sink_fn sink,
    void *sink_context, uint32_t *output_size)
{
    struct sink_state { camera_sw_jpeg_sink_fn fn; void *context; uint32_t bytes; int failed; } state = {sink, sink_context, 0U, 0};
    JPEGENCODE encode_state;
    uint8_t pixel_type;
    if ((encoder == NULL) || (encoder->workspace == NULL) ||
        (encoder->workspace_size < sizeof(JPEGENC)) || (sink == NULL) ||
        (output_size == NULL) ||
        (camera_sw_jpeg_pixel_type(frame, quality, &pixel_type) != 0))
        return -1;
    *output_size = 0U;
    /* JPEGENC forwards this opaque argument directly to the open callback. */
    auto open_sink = [](const char *opaque) -> void * {
        return const_cast<char *>(opaque);
    };
    auto close_sink = [](JPEGE_FILE *) {};
    auto read_sink = [](JPEGE_FILE *, uint8_t *, int32_t) -> int32_t { return 0; };
    auto seek_sink = [](JPEGE_FILE *, int32_t) -> int32_t { return 0; };
    auto write_sink = [](JPEGE_FILE *file, uint8_t *data, int32_t size) -> int32_t {
        sink_state *out = static_cast<sink_state *>(file->fHandle);
        if ((out == NULL) || (size < 0) || out->failed ||
            (out->fn(out->context, data, (size_t)size) != 0)) {
            if (out != NULL) out->failed = 1;
            return 0;
        }
        out->bytes += (uint32_t)size;
        return size;
    };
    JPEGENC *core = camera_sw_encoder(encoder);
    if (core->open(reinterpret_cast<const char *>(&state), open_sink,
                   close_sink, read_sink,
                   write_sink, seek_sink) != JPEGE_SUCCESS)
        return -1;
    if ((core->encodeBegin(&encode_state, frame->width, frame->height,
                           pixel_type, JPEGE_SUBSAMPLE_420,
                           (uint8_t)quality) != JPEGE_SUCCESS) ||
        (camera_sw_jpeg_add_frame(core, &encode_state, frame) != JPEGE_SUCCESS))
        return -1;
    if (core->close() <= 0 || state.failed)
        return -1;
    *output_size = state.bytes;
    return 0;
}
