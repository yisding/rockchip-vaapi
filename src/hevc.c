/*
 * hevc.c — HEVC Annex B parameter-set reconstruction from VA-API structs
 *
 * Copyright (C) 2026 rockchip-vaapi contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hevc.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <va/va.h>

#include "bs.h"

enum {
    HEVC_NAL_VPS = 32,
    HEVC_NAL_SPS = 33,
    HEVC_NAL_PPS = 34,
    HEVC_MAX_CURRENT_REFS = 8,
    HEVC_MAX_LONG_TERM_SPS = 32,
    HEVC_RAW_NAL_CAPACITY = 32768,
};

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit;
} HEVCBitReader;

typedef struct {
    uint32_t negative[HEVC_MAX_CURRENT_REFS];
    uint32_t positive[HEVC_MAX_CURRENT_REFS];
    uint32_t long_term_poc[HEVC_MAX_CURRENT_REFS];
    unsigned int negative_count;
    unsigned int positive_count;
    unsigned int long_term_count;
} HEVCRPS;

static bool br_read_bit(HEVCBitReader *br, uint32_t *value)
{
    if (!br || !value || br->bit >= br->size * 8)
        return false;
    *value = (br->data[br->bit / 8] >> (7 - br->bit % 8)) & 1u;
    br->bit++;
    return true;
}

static bool br_read_ue(HEVCBitReader *br, uint32_t *value)
{
    uint32_t bit = 0;
    unsigned int zeros = 0;

    while (br_read_bit(br, &bit) && !bit) {
        if (++zeros > 31)
            return false;
    }
    if (!bit)
        return false;

    uint32_t suffix = 0;
    for (unsigned int i = 0; i < zeros; i++) {
        if (!br_read_bit(br, &bit))
            return false;
        suffix = (suffix << 1) | bit;
    }
    *value = (zeros == 31 ? UINT32_MAX : (UINT32_C(1) << zeros) - 1) +
             suffix;
    return true;
}

static size_t nal_offset(const uint8_t *data, size_t size)
{
    if (!data)
        return SIZE_MAX;
    if (size >= 4 && data[0] == 0 && data[1] == 0 &&
        data[2] == 0 && data[3] == 1)
        return 4;
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return 3;
    return 0;
}

static size_t unescape_prefix(const uint8_t *src, size_t src_size,
                              uint8_t *dst, size_t dst_size)
{
    size_t out = 0;
    unsigned int zeros = 0;

    for (size_t i = 0; i < src_size && out < dst_size; i++) {
        if (zeros >= 2 && src[i] == 3 && i + 1 < src_size && src[i + 1] <= 3) {
            zeros = 0;
            continue;
        }
        dst[out++] = src[i];
        zeros = src[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

bool rk_hevc_parse_slice_info(const uint8_t *data, size_t size,
                              RKHEVCSliceInfo *info)
{
    uint8_t rbsp[128];
    uint32_t bit;
    uint32_t pps_id;
    size_t offset = nal_offset(data, size);

    if (!info || offset == SIZE_MAX || size - offset < 3)
        return false;

    const uint8_t *nal = data + offset;
    size_t nal_size = size - offset;
    if ((nal[0] & 0x80) != 0)
        return false;

    uint8_t nal_type = (nal[0] >> 1) & 0x3f;
    uint8_t temporal_id_plus1 = nal[1] & 7;
    if (nal_type > 31 || temporal_id_plus1 == 0)
        return false;

    size_t rbsp_size = unescape_prefix(nal + 2, nal_size - 2,
                                       rbsp, sizeof(rbsp));
    HEVCBitReader br = { rbsp, rbsp_size, 0 };
    if (!br_read_bit(&br, &bit))
        return false;
    bool first = bit != 0;
    if (nal_type >= 16 && nal_type <= 23 && !br_read_bit(&br, &bit))
        return false;
    if (!br_read_ue(&br, &pps_id) || pps_id > 63)
        return false;

    *info = (RKHEVCSliceInfo) {
        .nal_unit_type = nal_type,
        .temporal_id_plus1 = temporal_id_plus1,
        .pps_id = (uint8_t)pps_id,
        .first_slice_segment_in_pic = first,
    };
    return true;
}

static size_t emulation_prevent(const uint8_t *src, size_t src_size,
                                uint8_t *dst, size_t dst_size)
{
    size_t out = 0;
    unsigned int zeros = 0;

    for (size_t i = 0; i < src_size; i++) {
        if (zeros >= 2 && src[i] <= 3) {
            if (out >= dst_size)
                return 0;
            dst[out++] = 3;
            zeros = 0;
        }
        if (out >= dst_size)
            return 0;
        dst[out++] = src[i];
        zeros = src[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

static int finish_nal(uint8_t *buf, size_t buf_size,
                      const uint8_t *raw, size_t raw_size)
{
    if (!buf || !raw || buf_size < 4)
        return -1;
    buf[0] = 0;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 1;
    size_t escaped = emulation_prevent(raw, raw_size, buf + 4, buf_size - 4);
    if (!escaped || escaped > (size_t)INT_MAX - 4)
        return -1;
    return (int)(escaped + 4);
}

static void write_nal_header(BSWriter *bs, unsigned int nal_type)
{
    bs_write(bs, 0, 1);          /* forbidden_zero_bit */
    bs_write(bs, nal_type, 6);
    bs_write(bs, 0, 6);          /* nuh_layer_id */
    bs_write(bs, 1, 3);          /* nuh_temporal_id_plus1 */
}

static int derive_level_idc(const VAPictureParameterBufferHEVC *pp)
{
    uint64_t samples = (uint64_t)pp->pic_width_in_luma_samples *
                       pp->pic_height_in_luma_samples;

    if (samples <= 36864) return 30;
    if (samples <= 122880) return 60;
    if (samples <= 245760) return 63;
    if (samples <= 552960) return 90;
    if (samples <= 983040) return 93;
    if (samples <= 2228224) return 123;
    if (samples <= 8912896) return 156;
    if (samples <= 35651584) return 186;
    return 186;
}

static void write_profile_tier_level(BSWriter *bs, int profile_idc,
                                     int level_idc, bool field_pic)
{
    bs_write(bs, 0, 2);                    /* general_profile_space */
    bs_write(bs, 0, 1);                    /* general_tier_flag */
    bs_write(bs, (uint32_t)profile_idc, 5);
    for (int i = 0; i < 32; i++)
        bs_write(bs, i == profile_idc, 1);
    bs_write(bs, !field_pic, 1);           /* progressive_source */
    bs_write(bs, field_pic, 1);            /* interlaced_source */
    bs_write(bs, 1, 1);                    /* non_packed_constraint */
    bs_write(bs, !field_pic, 1);           /* frame_only_constraint */
    if (profile_idc == 2) {
        bs_write(bs, 0, 7);
        bs_write(bs, 0, 1);                /* one_picture_only */
        bs_write(bs, 0, 24);
        bs_write(bs, 0, 11);
    } else {
        bs_write(bs, 0, 24);
        bs_write(bs, 0, 19);
    }
    bs_write(bs, 0, 1);                    /* general_inbld_flag */
    bs_write(bs, (uint32_t)level_idc, 8);
}

static bool valid_picture(const VAPictureHEVC *picture)
{
    return picture->picture_id != VA_INVALID_SURFACE &&
           !(picture->flags & VA_PICTURE_HEVC_INVALID);
}

static void sort_u32(uint32_t *values, unsigned int count)
{
    for (unsigned int i = 1; i < count; i++) {
        uint32_t value = values[i];
        unsigned int j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

static bool collect_rps(const VAPictureParameterBufferHEVC *pp, HEVCRPS *rps)
{
    memset(rps, 0, sizeof(*rps));
    int64_t current_poc = pp->CurrPic.pic_order_cnt;

    for (size_t i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pp->ReferenceFrames[i];
        if (!valid_picture(ref))
            continue;

        uint32_t current_flags = ref->flags &
            (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
             VA_PICTURE_HEVC_RPS_ST_CURR_AFTER |
             VA_PICTURE_HEVC_RPS_LT_CURR);
        if (!current_flags)
            continue;
        if ((current_flags & (current_flags - 1)) != 0)
            return false;

        unsigned int total = rps->negative_count + rps->positive_count +
                             rps->long_term_count;
        if (total >= HEVC_MAX_CURRENT_REFS)
            return false;

        int64_t delta = (int64_t)ref->pic_order_cnt - current_poc;
        if (current_flags == VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE) {
            if (delta >= 0 || -delta > UINT16_MAX)
                return false;
            rps->negative[rps->negative_count++] = (uint32_t)-delta;
        } else if (current_flags == VA_PICTURE_HEVC_RPS_ST_CURR_AFTER) {
            if (delta <= 0 || delta > UINT16_MAX)
                return false;
            rps->positive[rps->positive_count++] = (uint32_t)delta;
        } else {
            rps->long_term_poc[rps->long_term_count++] =
                (uint32_t)ref->pic_order_cnt;
        }
    }

    sort_u32(rps->negative, rps->negative_count);
    sort_u32(rps->positive, rps->positive_count);
    return true;
}

static bool validate_picture_parameters(const VAPictureParameterBufferHEVC *pp,
                                        const VAIQMatrixBufferHEVC *iq,
                                        int profile_idc, HEVCRPS *rps)
{
    if (!pp || !rps || (profile_idc != 1 && profile_idc != 2) ||
        !valid_picture(&pp->CurrPic) || !pp->pic_width_in_luma_samples ||
        !pp->pic_height_in_luma_samples ||
        pp->pic_fields.bits.chroma_format_idc != 1 ||
        pp->pic_fields.bits.separate_colour_plane_flag ||
        pp->sps_max_dec_pic_buffering_minus1 > 15 ||
        pp->bit_depth_luma_minus8 != pp->bit_depth_chroma_minus8 ||
        (profile_idc == 1 && pp->bit_depth_luma_minus8 != 0) ||
        (profile_idc == 2 && pp->bit_depth_luma_minus8 > 2) ||
        pp->log2_min_luma_coding_block_size_minus3 > 3 ||
        pp->log2_diff_max_min_luma_coding_block_size > 3 ||
        pp->log2_min_transform_block_size_minus2 > 3 ||
        pp->log2_diff_max_min_transform_block_size > 3 ||
        pp->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        pp->num_short_term_ref_pic_sets > 64 ||
        pp->num_long_term_ref_pic_sps > HEVC_MAX_LONG_TERM_SPS ||
        pp->num_ref_idx_l0_default_active_minus1 > 14 ||
        pp->num_ref_idx_l1_default_active_minus1 > 14 ||
        pp->num_extra_slice_header_bits > 7 ||
        pp->init_qp_minus26 < -26 || pp->init_qp_minus26 > 25 ||
        pp->pps_cb_qp_offset < -12 || pp->pps_cb_qp_offset > 12 ||
        pp->pps_cr_qp_offset < -12 || pp->pps_cr_qp_offset > 12 ||
        pp->pps_beta_offset_div2 < -6 || pp->pps_beta_offset_div2 > 6 ||
        pp->pps_tc_offset_div2 < -6 || pp->pps_tc_offset_div2 > 6 ||
        pp->num_tile_columns_minus1 > 19 ||
        pp->num_tile_rows_minus1 > 21 ||
        (pp->pic_fields.bits.scaling_list_enabled_flag && !iq))
        return false;

    unsigned int min_cb_log2 =
        pp->log2_min_luma_coding_block_size_minus3 + 3;
    unsigned int ctb_log2 = min_cb_log2 +
                            pp->log2_diff_max_min_luma_coding_block_size;
    unsigned int min_cb_size = 1u << min_cb_log2;
    unsigned int ctb_size = 1u << ctb_log2;
    if (ctb_log2 > 6 ||
        pp->pic_width_in_luma_samples % min_cb_size != 0 ||
        pp->pic_height_in_luma_samples % min_cb_size != 0 ||
        pp->log2_parallel_merge_level_minus2 > ctb_log2 - 2 ||
        (pp->pic_fields.bits.cu_qp_delta_enabled_flag &&
         pp->diff_cu_qp_delta_depth >
            pp->log2_diff_max_min_luma_coding_block_size))
        return false;

    if (pp->pic_fields.bits.pcm_enabled_flag &&
        (pp->pcm_sample_bit_depth_luma_minus1 > 15 ||
         pp->pcm_sample_bit_depth_chroma_minus1 > 15 ||
         pp->log2_min_pcm_luma_coding_block_size_minus3 > 2 ||
         pp->log2_min_pcm_luma_coding_block_size_minus3 +
             pp->log2_diff_max_min_pcm_luma_coding_block_size >
             ctb_log2 - 3))
        return false;

    if (pp->pic_fields.bits.tiles_enabled_flag) {
        unsigned int width_ctbs =
            (pp->pic_width_in_luma_samples + ctb_size - 1) / ctb_size;
        unsigned int height_ctbs =
            (pp->pic_height_in_luma_samples + ctb_size - 1) / ctb_size;
        unsigned int columns = pp->num_tile_columns_minus1 + 1;
        unsigned int rows = pp->num_tile_rows_minus1 + 1;
        unsigned int explicit_width = 0;
        unsigned int explicit_height = 0;

        if (columns > width_ctbs || rows > height_ctbs)
            return false;
        for (unsigned int i = 0; i < pp->num_tile_columns_minus1; i++)
            explicit_width += pp->column_width_minus1[i] + 1;
        for (unsigned int i = 0; i < pp->num_tile_rows_minus1; i++)
            explicit_height += pp->row_height_minus1[i] + 1;
        if (explicit_width >= width_ctbs || explicit_height >= height_ctbs)
            return false;
    }
    return collect_rps(pp, rps);
}

static int write_vps(uint8_t *buf, size_t buf_size,
                     const VAPictureParameterBufferHEVC *pp, int profile_idc)
{
    uint8_t raw[512];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));
    bool field_pic = (pp->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) != 0;
    unsigned int reorder = pp->pic_fields.bits.NoPicReorderingFlag ? 0 :
                           pp->sps_max_dec_pic_buffering_minus1;

    write_nal_header(&bs, HEVC_NAL_VPS);
    bs_write(&bs, 0, 4);           /* vps_video_parameter_set_id */
    bs_write(&bs, 1, 1);           /* base_layer_internal */
    bs_write(&bs, 1, 1);           /* base_layer_available */
    bs_write(&bs, 0, 6);           /* max_layers_minus1 */
    bs_write(&bs, 0, 3);           /* max_sub_layers_minus1 */
    bs_write(&bs, 1, 1);           /* temporal_id_nesting */
    bs_write(&bs, 0xffff, 16);
    write_profile_tier_level(&bs, profile_idc, derive_level_idc(pp), field_pic);
    bs_write(&bs, 0, 1);           /* ordering_info_present */
    bs_write_ue(&bs, pp->sps_max_dec_pic_buffering_minus1);
    bs_write_ue(&bs, reorder);
    bs_write_ue(&bs, 0);           /* max_latency_increase_plus1 */
    bs_write(&bs, 0, 6);           /* max_layer_id */
    bs_write_ue(&bs, 0);           /* num_layer_sets_minus1 */
    bs_write(&bs, 0, 1);           /* timing_info_present */
    bs_write(&bs, 0, 1);           /* extension_flag */
    bs_rbsp_trailing(&bs);
    return finish_nal(buf, buf_size, raw, bs_bytes(&bs));
}

static const uint8_t diag4x4_x[16] = {
    0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 1, 2, 3, 2, 3, 3,
};
static const uint8_t diag4x4_y[16] = {
    0, 1, 0, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 3, 2, 3,
};
static const uint8_t diag8x8_x[64] = {
    0,0,1,0,1,2,0,1,2,3,0,1,2,3,4,0,
    1,2,3,4,5,0,1,2,3,4,5,6,0,1,2,3,
    4,5,6,7,1,2,3,4,5,6,7,2,3,4,5,6,
    7,3,4,5,6,7,4,5,6,7,5,6,7,6,7,7,
};
static const uint8_t diag8x8_y[64] = {
    0,1,0,2,1,0,3,2,1,0,4,3,2,1,0,5,
    4,3,2,1,0,6,5,4,3,2,1,0,7,6,5,4,
    3,2,1,0,7,6,5,4,3,2,1,7,6,5,4,3,
    2,7,6,5,4,3,7,6,5,4,7,6,5,7,6,7,
};

static const uint8_t *scaling_matrix(const VAIQMatrixBufferHEVC *iq,
                                     int size_id, int matrix_id)
{
    switch (size_id) {
    case 0: return iq->ScalingList4x4[matrix_id];
    case 1: return iq->ScalingList8x8[matrix_id];
    case 2: return iq->ScalingList16x16[matrix_id];
    case 3: return iq->ScalingList32x32[matrix_id / 3];
    default: return NULL;
    }
}

static int scaling_dc(const VAIQMatrixBufferHEVC *iq,
                      int size_id, int matrix_id)
{
    if (size_id == 2)
        return iq->ScalingListDC16x16[matrix_id];
    if (size_id == 3)
        return iq->ScalingListDC32x32[matrix_id / 3];
    return 8;
}

static bool write_scaling_lists(BSWriter *bs,
                                const VAIQMatrixBufferHEVC *iq)
{
    for (int size_id = 0; size_id < 4; size_id++) {
        int step = size_id == 3 ? 3 : 1;
        int count = size_id == 0 ? 16 : 64;
        for (int matrix_id = 0; matrix_id < 6; matrix_id += step) {
            const uint8_t *matrix = scaling_matrix(iq, size_id, matrix_id);
            int next = 8;
            bs_write(bs, 1, 1);   /* scaling_list_pred_mode_flag */
            if (size_id > 1) {
                int dc = scaling_dc(iq, size_id, matrix_id);
                if (dc == 0)
                    return false;
                bs_write_se(bs, dc - 8);
                next = dc;
            }
            for (int i = 0; i < count; i++) {
                int pos = size_id == 0
                        ? 4 * diag4x4_y[i] + diag4x4_x[i]
                        : 8 * diag8x8_y[i] + diag8x8_x[i];
                int value = matrix[pos];
                int delta = value - next;
                if (delta > 127) delta -= 256;
                if (delta < -128) delta += 256;
                bs_write_se(bs, delta);
                next = value;
            }
        }
    }
    return true;
}

static void write_short_term_rps(BSWriter *bs, const HEVCRPS *rps,
                                 unsigned int index)
{
    if (index > 0)
        bs_write(bs, 0, 1);       /* inter_ref_pic_set_prediction_flag */
    bs_write_ue(bs, rps->negative_count);
    bs_write_ue(bs, rps->positive_count);

    uint32_t previous = 0;
    for (unsigned int i = 0; i < rps->negative_count; i++) {
        bs_write_ue(bs, rps->negative[i] - previous - 1);
        bs_write(bs, 1, 1);       /* used_by_curr_pic_s0_flag */
        previous = rps->negative[i];
    }
    previous = 0;
    for (unsigned int i = 0; i < rps->positive_count; i++) {
        bs_write_ue(bs, rps->positive[i] - previous - 1);
        bs_write(bs, 1, 1);       /* used_by_curr_pic_s1_flag */
        previous = rps->positive[i];
    }
}

static int write_sps(uint8_t *buf, size_t buf_size,
                     const VAPictureParameterBufferHEVC *pp,
                     const VAIQMatrixBufferHEVC *iq, int profile_idc,
                     const HEVCRPS *rps)
{
    uint8_t raw[HEVC_RAW_NAL_CAPACITY];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));
    bool field_pic = (pp->CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC) != 0;
    unsigned int reorder = pp->pic_fields.bits.NoPicReorderingFlag ? 0 :
                           pp->sps_max_dec_pic_buffering_minus1;

    write_nal_header(&bs, HEVC_NAL_SPS);
    bs_write(&bs, 0, 4);           /* sps_video_parameter_set_id */
    bs_write(&bs, 0, 3);           /* max_sub_layers_minus1 */
    bs_write(&bs, 1, 1);           /* temporal_id_nesting */
    write_profile_tier_level(&bs, profile_idc, derive_level_idc(pp), field_pic);
    bs_write_ue(&bs, 0);           /* sps_seq_parameter_set_id */
    bs_write_ue(&bs, pp->pic_fields.bits.chroma_format_idc);
    bs_write_ue(&bs, pp->pic_width_in_luma_samples);
    bs_write_ue(&bs, pp->pic_height_in_luma_samples);
    bs_write(&bs, 0, 1);           /* conformance_window_flag */
    bs_write_ue(&bs, pp->bit_depth_luma_minus8);
    bs_write_ue(&bs, pp->bit_depth_chroma_minus8);
    bs_write_ue(&bs, pp->log2_max_pic_order_cnt_lsb_minus4);
    bs_write(&bs, 0, 1);           /* sub_layer_ordering_info_present */
    bs_write_ue(&bs, pp->sps_max_dec_pic_buffering_minus1);
    bs_write_ue(&bs, reorder);
    bs_write_ue(&bs, 0);           /* max_latency_increase_plus1 */
    bs_write_ue(&bs, pp->log2_min_luma_coding_block_size_minus3);
    bs_write_ue(&bs, pp->log2_diff_max_min_luma_coding_block_size);
    bs_write_ue(&bs, pp->log2_min_transform_block_size_minus2);
    bs_write_ue(&bs, pp->log2_diff_max_min_transform_block_size);
    bs_write_ue(&bs, pp->max_transform_hierarchy_depth_inter);
    bs_write_ue(&bs, pp->max_transform_hierarchy_depth_intra);
    bs_write(&bs, pp->pic_fields.bits.scaling_list_enabled_flag, 1);
    if (pp->pic_fields.bits.scaling_list_enabled_flag) {
        bs_write(&bs, 1, 1);       /* sps_scaling_list_data_present_flag */
        if (!write_scaling_lists(&bs, iq))
            return -1;
    }
    bs_write(&bs, pp->pic_fields.bits.amp_enabled_flag, 1);
    bs_write(&bs, pp->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.pcm_enabled_flag, 1);
    if (pp->pic_fields.bits.pcm_enabled_flag) {
        bs_write(&bs, pp->pcm_sample_bit_depth_luma_minus1, 4);
        bs_write(&bs, pp->pcm_sample_bit_depth_chroma_minus1, 4);
        bs_write_ue(&bs, pp->log2_min_pcm_luma_coding_block_size_minus3);
        bs_write_ue(&bs, pp->log2_diff_max_min_pcm_luma_coding_block_size);
        bs_write(&bs, pp->pic_fields.bits.pcm_loop_filter_disabled_flag, 1);
    }
    bs_write_ue(&bs, pp->num_short_term_ref_pic_sets);
    for (unsigned int i = 0; i < pp->num_short_term_ref_pic_sets; i++)
        write_short_term_rps(&bs, rps, i);

    bool has_long_term = pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag;
    bs_write(&bs, has_long_term, 1);
    if (has_long_term) {
        bs_write_ue(&bs, pp->num_long_term_ref_pic_sps);
        unsigned int poc_bits = pp->log2_max_pic_order_cnt_lsb_minus4 + 4;
        uint32_t mask = poc_bits == 32 ? UINT32_MAX :
                        (UINT32_C(1) << poc_bits) - 1;
        for (unsigned int i = 0; i < pp->num_long_term_ref_pic_sps; i++) {
            bool used = rps->long_term_count > 0;
            uint32_t poc = used
                ? rps->long_term_poc[i % rps->long_term_count] & mask : 0;
            bs_write(&bs, poc, (int)poc_bits);
            bs_write(&bs, used, 1);
        }
    }
    bs_write(&bs, pp->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.strong_intra_smoothing_enabled_flag, 1);
    bs_write(&bs, 0, 1);           /* vui_parameters_present_flag */
    bs_write(&bs, 0, 1);           /* sps_extension_present_flag */
    bs_rbsp_trailing(&bs);
    return finish_nal(buf, buf_size, raw, bs_bytes(&bs));
}

static int write_pps(uint8_t *buf, size_t buf_size,
                     const VAPictureParameterBufferHEVC *pp, uint8_t pps_id)
{
    uint8_t raw[4096];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));
    bool deblock_present =
        pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
        pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag ||
        pp->pps_beta_offset_div2 != 0 || pp->pps_tc_offset_div2 != 0;

    write_nal_header(&bs, HEVC_NAL_PPS);
    bs_write_ue(&bs, pps_id);
    bs_write_ue(&bs, 0);           /* pps_seq_parameter_set_id */
    bs_write(&bs, pp->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag, 1);
    bs_write(&bs, pp->slice_parsing_fields.bits.output_flag_present_flag, 1);
    bs_write(&bs, pp->num_extra_slice_header_bits, 3);
    bs_write(&bs, pp->pic_fields.bits.sign_data_hiding_enabled_flag, 1);
    bs_write(&bs, pp->slice_parsing_fields.bits.cabac_init_present_flag, 1);
    bs_write_ue(&bs, pp->num_ref_idx_l0_default_active_minus1);
    bs_write_ue(&bs, pp->num_ref_idx_l1_default_active_minus1);
    bs_write_se(&bs, pp->init_qp_minus26);
    bs_write(&bs, pp->pic_fields.bits.constrained_intra_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.transform_skip_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.cu_qp_delta_enabled_flag, 1);
    if (pp->pic_fields.bits.cu_qp_delta_enabled_flag)
        bs_write_ue(&bs, pp->diff_cu_qp_delta_depth);
    bs_write_se(&bs, pp->pps_cb_qp_offset);
    bs_write_se(&bs, pp->pps_cr_qp_offset);
    bs_write(&bs, pp->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.weighted_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.weighted_bipred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.transquant_bypass_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.tiles_enabled_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.entropy_coding_sync_enabled_flag, 1);
    if (pp->pic_fields.bits.tiles_enabled_flag) {
        bs_write_ue(&bs, pp->num_tile_columns_minus1);
        bs_write_ue(&bs, pp->num_tile_rows_minus1);
        bs_write(&bs, 0, 1);       /* uniform_spacing_flag */
        for (unsigned int i = 0; i < pp->num_tile_columns_minus1; i++)
            bs_write_ue(&bs, pp->column_width_minus1[i]);
        for (unsigned int i = 0; i < pp->num_tile_rows_minus1; i++)
            bs_write_ue(&bs, pp->row_height_minus1[i]);
        bs_write(&bs, pp->pic_fields.bits.loop_filter_across_tiles_enabled_flag, 1);
    }
    bs_write(&bs, pp->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag, 1);
    bs_write(&bs, deblock_present, 1);
    if (deblock_present) {
        bs_write(&bs, pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag, 1);
        bs_write(&bs, pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag, 1);
        if (!pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
            bs_write_se(&bs, pp->pps_beta_offset_div2);
            bs_write_se(&bs, pp->pps_tc_offset_div2);
        }
    }
    bs_write(&bs, 0, 1);           /* pps_scaling_list_data_present_flag */
    bs_write(&bs, pp->slice_parsing_fields.bits.lists_modification_present_flag, 1);
    bs_write_ue(&bs, pp->log2_parallel_merge_level_minus2);
    bs_write(&bs, pp->slice_parsing_fields.bits.slice_segment_header_extension_present_flag, 1);
    bs_write(&bs, 0, 1);           /* pps_extension_present_flag */
    bs_rbsp_trailing(&bs);
    return finish_nal(buf, buf_size, raw, bs_bytes(&bs));
}

int rk_hevc_write_parameter_sets(uint8_t *buf, size_t buf_size,
                                 const VAPictureParameterBufferHEVC *pp,
                                 const VAIQMatrixBufferHEVC *iq,
                                 uint8_t pps_id, int profile_idc)
{
    HEVCRPS rps;
    size_t used = 0;

    if (!buf || pps_id > 63 ||
        !validate_picture_parameters(pp, iq, profile_idc, &rps))
        return -1;

    int size = write_vps(buf + used, buf_size - used, pp, profile_idc);
    if (size < 0)
        return -1;
    used += (size_t)size;

    size = write_sps(buf + used, buf_size - used, pp, iq, profile_idc, &rps);
    if (size < 0)
        return -1;
    used += (size_t)size;

    size = write_pps(buf + used, buf_size - used, pp, pps_id);
    if (size < 0 || used > (size_t)INT_MAX - (size_t)size)
        return -1;
    used += (size_t)size;
    return (int)used;
}
