/*
 * hevc.h — HEVC Annex B parameter-set reconstruction from VA-API structs
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <va/va.h>
#include <va/va_dec_hevc.h>

typedef struct {
    uint8_t nal_unit_type;
    uint8_t temporal_id_plus1;
    uint8_t pps_id;
    bool first_slice_segment_in_pic;
} RKHEVCSliceInfo;

/* Parse the fields which identify the parameter sets required by a raw HEVC
 * slice NAL.  Both Annex B-prefixed and bare NAL units are accepted. */
bool rk_hevc_parse_slice_info(const uint8_t *data, size_t size,
                              RKHEVCSliceInfo *info);

/* Write one Annex B VPS/SPS/PPS bundle.  profile_idc is 1 for Main and 2 for
 * Main10.  Returns the byte count or -1 when VA parameters are malformed or
 * cannot be represented without guessing. */
int rk_hevc_write_parameter_sets(uint8_t *buf, size_t buf_size,
                                 const VAPictureParameterBufferHEVC *pp,
                                 const VAIQMatrixBufferHEVC *iq,
                                 uint8_t pps_id, int profile_idc);
