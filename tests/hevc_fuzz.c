/*
 * HEVC reconstruction and slice-rewrite fuzz target.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hevc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_CAPACITY 16384

static uint8_t take_byte(const uint8_t *data, size_t size, size_t *offset)
{
    if (*offset >= size)
        return 0;
    return data[(*offset)++];
}

static void fill_object(void *object, size_t object_size,
                        const uint8_t *data, size_t size, size_t rotation)
{
    memset(object, 0, object_size);
    if (!size)
        return;

    uint8_t *bytes = object;
    for (size_t i = 0; i < object_size; i++)
        bytes[i] = data[(i + rotation) % size];
}

static void check_result(int result, size_t output_size)
{
    if (result > 0 && (size_t)result > output_size)
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    VAPictureParameterBufferHEVC picture;
    VAIQMatrixBufferHEVC iq;
    VASliceParameterBufferHEVC slice;
    RKHEVCSliceInfo info;
    uint8_t output[OUTPUT_CAPACITY];
    size_t offset = 0;

    uint8_t control = take_byte(data, size, &offset);
    size_t output_size =
        ((size_t)take_byte(data, size, &offset) |
         ((size_t)take_byte(data, size, &offset) << 8)) %
        (sizeof(output) + 1);
    uint8_t pps_id = take_byte(data, size, &offset);
    int profile = (control & 2) ? 2 : 1;
    const uint8_t *payload = offset < size ? data + offset : NULL;
    size_t payload_size = offset < size ? size - offset : 0;

    fill_object(&picture, sizeof(picture), payload, payload_size, 0);
    fill_object(&iq, sizeof(iq), payload, payload_size, 17);
    fill_object(&slice, sizeof(slice), payload, payload_size, 31);

    if ((control & 3) == 0 || (control & 3) == 3) {
        (void)rk_hevc_parse_slice_info(payload, payload_size, &info);
        check_result(rk_hevc_rewrite_slice_nal(
                         output, output_size, payload, payload_size,
                         &picture, &slice),
                     output_size);
    }

    if ((control & 3) != 0) {
        const VAIQMatrixBufferHEVC *matrix =
            control & 4 ? &iq : NULL;
        check_result(rk_hevc_write_parameter_sets(
                         output, output_size, &picture, matrix,
                         pps_id, profile),
                     output_size);
        check_result(rk_hevc_write_sequence_parameter_sets(
                         output, output_size, &picture, matrix, profile),
                     output_size);
        check_result(rk_hevc_write_picture_parameter_set(
                         output, output_size, &picture, matrix,
                         pps_id, profile),
                     output_size);
    }
    return 0;
}
