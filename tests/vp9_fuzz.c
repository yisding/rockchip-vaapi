/*
 * VP9 uncompressed-header fuzz target.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "vp9.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    RKVP9FrameInfo info;
    uint8_t control = size ? data[0] : 0;
    const uint8_t *payload = size > 1 ? data + 1 : NULL;
    size_t payload_size = size > 1 ? size - 1 : 0;

    (void)rk_vp9_parse_frame(payload, payload_size, 0, &info);
    (void)rk_vp9_parse_frame(payload, payload_size, 2, &info);
    (void)rk_vp9_parse_frame(payload, payload_size, control & 3, &info);

    uint8_t profile = control & 3;
    uint8_t slot = control >> 2;
    uint8_t packet;
    if (rk_vp9_make_show_existing(profile, slot, &packet)) {
        if (!rk_vp9_parse_frame(&packet, 1, profile, &info) ||
            !info.show_existing_frame || !info.show_frame ||
            info.frame_to_show_map_idx != slot)
            abort();
    }
    (void)rk_vp9_make_show_existing(profile, slot, NULL);
    return 0;
}
