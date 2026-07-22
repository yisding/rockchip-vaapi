/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef ROCKCHIP_VAAPI_VP9_H
#define ROCKCHIP_VAAPI_VP9_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool show_existing_frame;
    bool show_frame;
    uint8_t frame_to_show_map_idx;
    uint8_t refresh_frame_flags;
} RKVP9FrameInfo;

/* Parse the part of a VP9 Profile 0 uncompressed header needed to preserve
 * hidden reference frames. Returns false for malformed or unsupported input. */
bool rk_vp9_parse_profile0_frame(const uint8_t *data, size_t size,
                                 RKVP9FrameInfo *info);

/* Build the complete one-byte Profile 0 show_existing_frame header. */
bool rk_vp9_make_profile0_show_existing(uint8_t slot, uint8_t *packet);

#endif
