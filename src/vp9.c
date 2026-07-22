/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "vp9.h"

#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t bit_size;
    size_t bit_pos;
} BitReader;

static bool read_bits(BitReader *reader, unsigned count, uint32_t *value)
{
    if (!reader || !value || count > 32 ||
        reader->bit_pos > reader->bit_size ||
        count > reader->bit_size - reader->bit_pos)
        return false;

    uint32_t result = 0;
    for (unsigned i = 0; i < count; i++) {
        size_t pos = reader->bit_pos++;
        result = (result << 1) |
                 ((reader->data[pos / 8] >> (7 - pos % 8)) & 1u);
    }
    *value = result;
    return true;
}

bool rk_vp9_parse_profile0_frame(const uint8_t *data, size_t size,
                                 RKVP9FrameInfo *info)
{
    if (!data || !size || !info || size > SIZE_MAX / 8)
        return false;

    BitReader reader = {
        .data = data,
        .bit_size = size * 8,
        .bit_pos = 0,
    };
    uint32_t value;
    memset(info, 0, sizeof(*info));

    if (!read_bits(&reader, 2, &value) || value != 2 ||
        !read_bits(&reader, 2, &value) || value != 0 ||
        !read_bits(&reader, 1, &value))
        return false;

    info->show_existing_frame = value != 0;
    if (info->show_existing_frame) {
        if (!read_bits(&reader, 3, &value))
            return false;
        info->show_frame = true;
        info->frame_to_show_map_idx = (uint8_t)value;
        return true;
    }

    uint32_t frame_type;
    uint32_t error_resilient;
    if (!read_bits(&reader, 1, &frame_type) ||
        !read_bits(&reader, 1, &value) ||
        !read_bits(&reader, 1, &error_resilient))
        return false;
    info->show_frame = value != 0;

    if (frame_type == 0) {
        /* Key frames refresh all eight reference slots. */
        if (!read_bits(&reader, 24, &value) || value != 0x498342)
            return false;
        info->refresh_frame_flags = 0xff;
        return true;
    }

    uint32_t intra_only = 0;
    if (!info->show_frame && !read_bits(&reader, 1, &intra_only))
        return false;
    if (!error_resilient && !read_bits(&reader, 2, &value))
        return false;

    if (intra_only) {
        if (!read_bits(&reader, 24, &value) || value != 0x498342)
            return false;
        /* Profile 0 infers its color configuration for intra-only frames. */
    }

    if (!read_bits(&reader, 8, &value))
        return false;
    info->refresh_frame_flags = (uint8_t)value;
    return true;
}

bool rk_vp9_make_profile0_show_existing(uint8_t slot, uint8_t *packet)
{
    if (!packet || slot >= 8)
        return false;

    /* frame_marker=2, profile=0, show_existing_frame=1, then the slot. */
    *packet = (uint8_t)(0x88u | slot);
    return true;
}
