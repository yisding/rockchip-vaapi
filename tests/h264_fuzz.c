/*
 * H.264 reconstruction fuzz target.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "h264.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_CAPACITY 4096

static uint8_t take_byte(const uint8_t *data, size_t size, size_t *offset)
{
    if (*offset >= size)
        return 0;
    return data[(*offset)++];
}

static void fill_object(void *object, size_t object_size,
                        const uint8_t *data, size_t size)
{
    memset(object, 0, object_size);
    if (!size)
        return;

    uint8_t *bytes = object;
    for (size_t i = 0; i < object_size; i++)
        bytes[i] = data[i % size];
}

static void check_result(int result, size_t output_size)
{
    if (result > 0 && (size_t)result > output_size)
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const int profiles[] = {
        66, 77, 100, 110, 122, 244, 44, 83, 86, 118, 128,
    };
    VAPictureParameterBufferH264 picture;
    VAIQMatrixBufferH264 iq;
    uint8_t output[OUTPUT_CAPACITY];
    size_t offset = 0;

    uint8_t control = take_byte(data, size, &offset);
    size_t output_size =
        ((size_t)take_byte(data, size, &offset) |
         ((size_t)take_byte(data, size, &offset) << 8)) %
        (sizeof(output) + 1);
    int profile = profiles[take_byte(data, size, &offset) %
                           (sizeof(profiles) / sizeof(profiles[0]))];
    int l0_default = (int8_t)take_byte(data, size, &offset);
    int l1_default = (int8_t)take_byte(data, size, &offset);

    const uint8_t *payload = offset < size ? data + offset : NULL;
    size_t payload_size = offset < size ? size - offset : 0;
    fill_object(&picture, sizeof(picture), payload, payload_size);
    fill_object(&iq, sizeof(iq), payload, payload_size);

    (void)h264_derive_level_idc(&picture);
    check_result(h264_write_sps(output, output_size, &picture, profile),
                 output_size);
    check_result(h264_write_pps(output, output_size, &picture, NULL,
                                l0_default, l1_default),
                 output_size);
    check_result(h264_write_pps(output, output_size, &picture,
                                control & 1 ? &iq : NULL,
                                l0_default, l1_default),
                 output_size);
    return 0;
}
