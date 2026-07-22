/*
 * h264.h — H.264 Annex B SPS/PPS reconstruction
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once
#include <va/va.h>
#include <stdint.h>
#include <stddef.h>

int h264_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int profile_idc);

/* VA-API does not expose the original level_idc.  Derive the lowest Annex A
 * level that satisfies the frame-size, DPB, and MinLumaBiPredSize8x8
 * constraints which are preserved in VAPictureParameterBufferH264. */
int h264_derive_level_idc(const VAPictureParameterBufferH264 *pp);

/* num_ref_idx_l0/l1_default_active_minus1 are taken from the current frame's
 * slice parameters: VA-API does not expose the original PPS defaults, and
 * hardcoding them corrupts any stream whose slices rely on the PPS default
 * instead of carrying num_ref_idx_active_override_flag. The caller re-emits
 * the PPS before every frame so the "default" always matches the frame. */
int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   const VAIQMatrixBufferH264 *iq,
                   int num_ref_idx_l0_default_minus1,
                   int num_ref_idx_l1_default_minus1);
