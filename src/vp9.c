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

static bool read_profile(BitReader *reader, uint8_t *profile)
{
    uint32_t low_bit;
    uint32_t high_bit;
    if (!read_bits(reader, 1, &low_bit) ||
        !read_bits(reader, 1, &high_bit))
        return false;

    uint32_t parsed = low_bit | (high_bit << 1);
    if (parsed == 3) {
        uint32_t reserved_zero;
        if (!read_bits(reader, 1, &reserved_zero) || reserved_zero)
            return false;
    }
    *profile = (uint8_t)parsed;
    return true;
}

static bool skip_color_config(BitReader *reader, uint8_t profile)
{
    uint32_t value;
    if (profile >= 2) {
        if (!read_bits(reader, 1, &value) || value != 0)
            return false;
    }
    if (!read_bits(reader, 3, &value))
        return false;

    /* Profiles 0 and 2 are 4:2:0. RGB requires the 4:4:4 profiles. */
    if (value == 7)
        return false;
    return read_bits(reader, 1, &value);
}

bool rk_vp9_parse_frame(const uint8_t *data, size_t size, uint8_t profile,
                        RKVP9FrameInfo *info)
{
    if (!data || !size || !info || (profile != 0 && profile != 2) ||
        size > SIZE_MAX / 8)
        return false;

    BitReader reader = {
        .data = data,
        .bit_size = size * 8,
        .bit_pos = 0,
    };
    uint32_t value;
    memset(info, 0, sizeof(*info));

    uint8_t parsed_profile;
    if (!read_bits(&reader, 2, &value) || value != 2 ||
        !read_profile(&reader, &parsed_profile) ||
        parsed_profile != profile ||
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
        if (profile > 0 && !skip_color_config(&reader, profile))
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
        if (profile > 0 && !skip_color_config(&reader, profile))
            return false;
    }

    if (!read_bits(&reader, 8, &value))
        return false;
    info->refresh_frame_flags = (uint8_t)value;
    return true;
}

bool rk_vp9_make_show_existing(uint8_t profile, uint8_t slot, uint8_t *packet)
{
    if (!packet || (profile != 0 && profile != 2) || slot >= 8)
        return false;

    /* frame_marker=2, profile bits low-first, show_existing_frame=1, slot. */
    *packet = (uint8_t)(0x88u | (profile == 2 ? 0x10u : 0u) | slot);
    return true;
}
