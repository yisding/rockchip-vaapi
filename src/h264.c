/*
 * h264.c — H.264 Annex B SPS/PPS reconstruction from VA-API structs
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "h264.h"
#include "bs.h"
#include <string.h>
#include <stdint.h>

/* Insert emulation prevention bytes (0x03) to avoid 0x000001/0x000002 sequences */
static size_t emulation_prevent(const uint8_t *in, size_t in_sz,
                                uint8_t *out, size_t out_cap) {
    size_t out_pos = 0;
    int zeros = 0;
    for (size_t i = 0; i < in_sz; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            if (out_pos >= out_cap) return 0;
            out[out_pos++] = 0x03;
            zeros = 0;
        }
        if (out_pos >= out_cap) return 0;
        out[out_pos++] = in[i];
        zeros = (in[i] == 0) ? zeros + 1 : 0;
    }
    return out_pos;
}

int h264_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int profile_idc)
{
    uint8_t raw[512];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));

    /* NAL header: forbidden_zero=0, nal_ref_idc=3, nal_unit_type=7 */
    bs_write(&bs, 0x67, 8);

    bs_write(&bs, (uint32_t)profile_idc, 8);

    /* constraint_set flags (derive from profile) + reserved.
     * Must be exactly 8 bits: constraint_set0..5 (6) + reserved_zero_2bits (2). */
    int c0 = (profile_idc == 66) ? 1 : 0;
    int c1 = (profile_idc == 66 || profile_idc == 77) ? 1 : 0;
    bs_write(&bs, c0, 1);
    bs_write(&bs, c1, 1);
    bs_write(&bs, 0, 1); /* constraint_set2 */
    bs_write(&bs, 0, 5); /* constraint_set3..5 (3) + reserved_zero_2bits (2) */

    bs_write(&bs, 51, 8); /* level_idc = 5.1 (safe for all content) */

    bs_write_ue(&bs, 0); /* seq_parameter_set_id */

    /* High-profile family gets chroma/bit-depth fields */
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128) {
        int cfi = pp->seq_fields.bits.chroma_format_idc;
        bs_write_ue(&bs, (uint32_t)cfi);
        if (cfi == 3)
            bs_write(&bs, pp->seq_fields.bits.residual_colour_transform_flag, 1);
        bs_write_ue(&bs, pp->bit_depth_luma_minus8);
        bs_write_ue(&bs, pp->bit_depth_chroma_minus8);
        bs_write(&bs, 0, 1); /* qpprime_y_zero_transform_bypass_flag */
        bs_write(&bs, 0, 1); /* seq_scaling_matrix_present_flag = 0 */
    }

    bs_write_ue(&bs, pp->seq_fields.bits.log2_max_frame_num_minus4);

    int poc_type = pp->seq_fields.bits.pic_order_cnt_type;
    bs_write_ue(&bs, (uint32_t)poc_type);
    if (poc_type == 0) {
        bs_write_ue(&bs, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (poc_type == 1) {
        bs_write(&bs, pp->seq_fields.bits.delta_pic_order_always_zero_flag, 1);
        bs_write_se(&bs, 0); /* offset_for_non_ref_pic */
        bs_write_se(&bs, 0); /* offset_for_top_to_bottom_field */
        bs_write_ue(&bs, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
    }

    bs_write_ue(&bs, pp->num_ref_frames);
    bs_write(&bs, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag, 1);
    bs_write_ue(&bs, pp->picture_width_in_mbs_minus1);

    int fmo = pp->seq_fields.bits.frame_mbs_only_flag;
    uint32_t ph_units = fmo ? (uint32_t)pp->picture_height_in_mbs_minus1
                            : ((uint32_t)pp->picture_height_in_mbs_minus1 + 1) / 2 - 1;
    bs_write_ue(&bs, ph_units);
    bs_write(&bs, fmo, 1);
    if (!fmo)
        bs_write(&bs, pp->seq_fields.bits.mb_adaptive_frame_field_flag, 1);

    bs_write(&bs, pp->seq_fields.bits.direct_8x8_inference_flag, 1);
    bs_write(&bs, 0, 1); /* frame_cropping_flag = 0 */
    bs_write(&bs, 0, 1); /* vui_parameters_present_flag = 0 */

    bs_rbsp_trailing(&bs);

    size_t raw_sz = bs_bytes(&bs);
    if (4 + raw_sz * 2 > buf_size) return -1;

    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    size_t ep = emulation_prevent(raw, raw_sz, buf + 4, buf_size - 4);
    if (!ep) return -1;
    return (int)(4 + ep);
}

static int write_scaling_list(BSWriter *bs, const uint8_t *list,
                              const uint8_t *scan, size_t count)
{
    int last_scale = 8;

    for (size_t i = 0; i < count; i++) {
        int next_scale = list[scan[i]];
        int delta;

        /* VA-API supplies resolved scaling lists. A zero value cannot be
         * represented as an explicit list entry: in H.264 it terminates the
         * delta-coded list and selects fallback/default inference instead. */
        if (next_scale == 0)
            return -1;

        delta = next_scale - last_scale;
        if (delta > 127)
            delta -= 256;
        else if (delta < -128)
            delta += 256;
        bs_write_se(bs, delta);
        last_scale = next_scale;
    }
    return 0;
}

int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   const VAIQMatrixBufferH264 *iq,
                   int num_ref_idx_l0_default_minus1,
                   int num_ref_idx_l1_default_minus1)
{
    static const uint8_t scan4x4[16] = {
         0,  1,  4,  8,  5,  2,  3,  6,
         9, 12, 13, 10,  7, 11, 14, 15,
    };
    static const uint8_t scan8x8[64] = {
         0,  1,  8, 16,  9,  2,  3, 10,
        17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63,
    };
    uint8_t raw[1024];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));

    /* NAL header: forbidden_zero=0, nal_ref_idc=3, nal_unit_type=8 */
    bs_write(&bs, 0x68, 8);

    bs_write_ue(&bs, 0); /* pic_parameter_set_id */
    bs_write_ue(&bs, 0); /* seq_parameter_set_id */
    bs_write(&bs, pp->pic_fields.bits.entropy_coding_mode_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.pic_order_present_flag, 1);
    bs_write_ue(&bs, 0); /* num_slice_groups_minus1 = 0 */

    /* Slices without num_ref_idx_active_override_flag inherit these; see
     * h264.h — the caller supplies the current frame's slice values. */
    if (num_ref_idx_l0_default_minus1 < 0) num_ref_idx_l0_default_minus1 = 0;
    if (num_ref_idx_l1_default_minus1 < 0) num_ref_idx_l1_default_minus1 = 0;
    bs_write_ue(&bs, (uint32_t)num_ref_idx_l0_default_minus1);
    bs_write_ue(&bs, (uint32_t)num_ref_idx_l1_default_minus1);

    bs_write(&bs, pp->pic_fields.bits.weighted_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.weighted_bipred_idc, 2);
    bs_write_se(&bs, pp->pic_init_qp_minus26);
    bs_write_se(&bs, pp->pic_init_qs_minus26);
    bs_write_se(&bs, pp->chroma_qp_index_offset);
    bs_write(&bs, pp->pic_fields.bits.deblocking_filter_control_present_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.constrained_intra_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.redundant_pic_cnt_present_flag, 1);

    /* More-data present if high-profile extensions needed */
    if (iq || pp->pic_fields.bits.transform_8x8_mode_flag ||
        pp->second_chroma_qp_index_offset != pp->chroma_qp_index_offset) {
        bs_write(&bs, pp->pic_fields.bits.transform_8x8_mode_flag, 1);
        bs_write(&bs, iq != NULL, 1); /* pic_scaling_matrix_present_flag */
        if (iq) {
            for (size_t i = 0; i < 6; i++) {
                bs_write(&bs, 1, 1); /* pic_scaling_list_present_flag */
                if (write_scaling_list(&bs, iq->ScalingList4x4[i],
                                       scan4x4, sizeof(scan4x4)) < 0)
                    return -1;
            }
            if (pp->pic_fields.bits.transform_8x8_mode_flag) {
                for (size_t i = 0; i < 2; i++) {
                    bs_write(&bs, 1, 1); /* pic_scaling_list_present_flag */
                    if (write_scaling_list(&bs, iq->ScalingList8x8[i],
                                           scan8x8, sizeof(scan8x8)) < 0)
                        return -1;
                }
            }
        }
        bs_write_se(&bs, pp->second_chroma_qp_index_offset);
    }

    bs_rbsp_trailing(&bs);

    size_t raw_sz = bs_bytes(&bs);
    if (4 + raw_sz * 2 > buf_size) return -1;

    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    size_t ep = emulation_prevent(raw, raw_sz, buf + 4, buf_size - 4);
    if (!ep) return -1;
    return (int)(4 + ep);
}
