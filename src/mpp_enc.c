#include "mpp_enc.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/rk_venc_rc.h>

#include "buffer.h"
#include "convert.h"
#include "log.h"
#include "surface.h"

static bool enc_set_s32(MppEncCfg cfg, const char *key, int32_t value)
{
    if (mpp_enc_cfg_set_s32(cfg, key, value) != MPP_OK) {
        LOG_WARNING("encoder config rejected %s=%d", key, value);
        return false;
    }
    return true;
}

static uint32_t mpp_rc_mode(uint32_t va_mode)
{
    if (va_mode == VA_RC_CBR)
        return MPP_ENC_RC_MODE_CBR;
    if (va_mode == VA_RC_VBR)
        return MPP_ENC_RC_MODE_VBR;
    return MPP_ENC_RC_MODE_FIXQP;
}

static int32_t bounded_bitrate(int64_t value)
{
    if (value < 1)
        return 1;
    if (value > INT_MAX)
        return INT_MAX;
    return (int32_t)value;
}

static uint32_t encoder_bitrate(const RKContext *context)
{
    if (context->enc_bitrate)
        return context->enc_bitrate;
    return context->coding == MPP_VIDEO_CodingHEVC
         ? context->enc_hevc_seq.bits_per_second
         : context->enc_seq.bits_per_second;
}

static uint32_t encoder_gop(const RKContext *context)
{
    uint32_t value = context->coding == MPP_VIDEO_CodingHEVC
                   ? context->enc_hevc_seq.intra_period
                   : context->enc_seq.intra_period;
    return value ? value : 60;
}

static int32_t encoder_qp(const RKContext *context)
{
    uint8_t value = context->coding == MPP_VIDEO_CodingHEVC
                  ? context->enc_hevc_pic.pic_init_qp
                  : context->enc_pic.pic_init_qp;
    return value >= 1 && value <= 51 ? value : 26;
}

static uint32_t hevc_ctu_size(const RKContext *context)
{
    const VAEncSequenceParameterBufferHEVC *seq = &context->enc_hevc_seq;
    unsigned int log2_size =
        3u + seq->log2_min_luma_coding_block_size_minus3 +
        seq->log2_diff_max_min_luma_coding_block_size;
    return log2_size <= 6 ? 1u << log2_size : 0;
}

static bool add_encoder_slice(RKContext *context, uint32_t address,
                              uint32_t units, uint32_t units_per_row)
{
    if (!units || !units_per_row || address != context->enc_slice_units ||
        address % units_per_row || units % units_per_row ||
        (context->enc_slice_count &&
         (context->enc_slice_last_units != context->enc_slice_unit_span ||
          units > context->enc_slice_unit_span)) ||
        units > UINT32_MAX - context->enc_slice_units)
        return false;

    if (!context->enc_slice_count)
        context->enc_slice_unit_span = units;
    context->enc_slice_count++;
    context->enc_slice_units += units;
    context->enc_slice_last_units = units;
    return true;
}

static bool hevc_slices_compatible(
    const VAEncSliceParameterBufferHEVC *first,
    const VAEncSliceParameterBufferHEVC *next)
{
    return first->slice_type == next->slice_type &&
           first->slice_pic_parameter_set_id ==
               next->slice_pic_parameter_set_id &&
           first->slice_qp_delta == next->slice_qp_delta &&
           first->slice_cb_qp_offset == next->slice_cb_qp_offset &&
           first->slice_cr_qp_offset == next->slice_cr_qp_offset &&
           first->slice_beta_offset_div2 == next->slice_beta_offset_div2 &&
           first->slice_tc_offset_div2 == next->slice_tc_offset_div2 &&
           first->slice_fields.bits.dependent_slice_segment_flag ==
               next->slice_fields.bits.dependent_slice_segment_flag &&
           first->slice_fields.bits.slice_sao_luma_flag ==
               next->slice_fields.bits.slice_sao_luma_flag &&
           first->slice_fields.bits.slice_sao_chroma_flag ==
               next->slice_fields.bits.slice_sao_chroma_flag &&
           first->slice_fields.bits
                   .slice_deblocking_filter_disabled_flag ==
               next->slice_fields.bits
                   .slice_deblocking_filter_disabled_flag &&
           first->slice_fields.bits
                   .slice_loop_filter_across_slices_enabled_flag ==
               next->slice_fields.bits
                   .slice_loop_filter_across_slices_enabled_flag;
}

static int32_t encoder_width(const RKContext *context)
{
    return context->render_surface ? context->render_surface->width
                                   : context->width;
}

static int32_t encoder_height(const RKContext *context)
{
    return context->render_surface ? context->render_surface->height
                                   : context->height;
}

static bool configure_encoder(RKContext *context)
{
    int32_t width = encoder_width(context);
    int32_t height = encoder_height(context);
    int32_t hstride = context->render_surface
                    ? context->render_surface->hstride
                    : (width + 15) & ~15;
    int32_t vstride = context->render_surface
                    ? context->render_surface->vstride
                    : (height + 15) & ~15;
    uint32_t requested_bitrate = encoder_bitrate(context);
    int32_t bitrate = bounded_bitrate(requested_bitrate
                                   ? requested_bitrate : 2000000);
    int32_t gop = (int32_t)encoder_gop(context);
    int32_t qp = encoder_qp(context);
    uint32_t rc_mode = mpp_rc_mode(context->rate_control);
    uint32_t fps_num = context->enc_fps_num ? context->enc_fps_num : 30;
    uint32_t fps_den = context->enc_fps_den ? context->enc_fps_den : 1;
    int32_t split_mode = context->enc_slice_count > 1
                       ? MPP_ENC_SPLIT_BY_CTU : MPP_ENC_SPLIT_NONE;
    int32_t split_arg = context->enc_slice_count > 1
                      ? (int32_t)context->enc_slice_unit_span : 0;

    if (!enc_set_s32(context->enc_cfg, "prep:width", width) ||
        !enc_set_s32(context->enc_cfg, "prep:height", height) ||
        !enc_set_s32(context->enc_cfg, "prep:hor_stride", hstride) ||
        !enc_set_s32(context->enc_cfg, "prep:ver_stride", vstride) ||
        !enc_set_s32(context->enc_cfg, "prep:format", MPP_FMT_YUV420SP) ||
        !enc_set_s32(context->enc_cfg, "rc:fps_in_flex", 0) ||
        !enc_set_s32(context->enc_cfg, "rc:fps_in_num", (int32_t)fps_num) ||
        !enc_set_s32(context->enc_cfg, "rc:fps_in_denom", (int32_t)fps_den) ||
        !enc_set_s32(context->enc_cfg, "rc:fps_out_flex", 0) ||
        !enc_set_s32(context->enc_cfg, "rc:fps_out_num", (int32_t)fps_num) ||
        !enc_set_s32(context->enc_cfg, "rc:fps_out_denom", (int32_t)fps_den) ||
        !enc_set_s32(context->enc_cfg, "rc:gop", gop) ||
        !enc_set_s32(context->enc_cfg, "rc:mode", (int32_t)rc_mode) ||
        !enc_set_s32(context->enc_cfg, "rc:qp_init", qp) ||
        !enc_set_s32(context->enc_cfg, "split:mode", split_mode) ||
        !enc_set_s32(context->enc_cfg, "split:arg", split_arg) ||
        !enc_set_s32(context->enc_cfg, "split:out", 0)) {
        return false;
    }

    if (context->coding == MPP_VIDEO_CodingAVC) {
        const VAEncSequenceParameterBufferH264 *seq = &context->enc_seq;
        const VAEncPictureParameterBufferH264 *pic = &context->enc_pic;
        if (!enc_set_s32(context->enc_cfg, "h264:profile",
                         context->profile == VAProfileH264High ? 100 : 77) ||
            !enc_set_s32(context->enc_cfg, "h264:level",
                         seq->level_idc ? seq->level_idc : 40) ||
            !enc_set_s32(context->enc_cfg, "h264:cabac_en",
                         pic->pic_fields.bits.entropy_coding_mode_flag) ||
            !enc_set_s32(context->enc_cfg, "h264:cabac_idc", 0) ||
            !enc_set_s32(context->enc_cfg, "h264:trans8x8",
                         pic->pic_fields.bits.transform_8x8_mode_flag))
            return false;
    } else {
        const VAEncSequenceParameterBufferHEVC *seq =
            &context->enc_hevc_seq;
        const VAEncPictureParameterBufferHEVC *pic =
            &context->enc_hevc_pic;
        const VAEncSliceParameterBufferHEVC *slice =
            &context->enc_hevc_slice;
        if (!enc_set_s32(context->enc_cfg, "h265:profile",
                         seq->general_profile_idc) ||
            !enc_set_s32(context->enc_cfg, "h265:tier",
                         seq->general_tier_flag) ||
            !enc_set_s32(context->enc_cfg, "h265:level",
                         seq->general_level_idc
                             ? seq->general_level_idc : 120) ||
            !enc_set_s32(context->enc_cfg, "h265:lcu_size",
                         (int32_t)hevc_ctu_size(context)) ||
            !enc_set_s32(context->enc_cfg,
                         "h265:diff_cu_qp_delta_depth",
                         pic->diff_cu_qp_delta_depth) ||
            !enc_set_s32(context->enc_cfg, "h265:cb_qp_offset",
                         pic->pps_cb_qp_offset) ||
            !enc_set_s32(context->enc_cfg, "h265:cr_qp_offset",
                         pic->pps_cr_qp_offset) ||
            !enc_set_s32(context->enc_cfg, "h265:const_intra",
                         pic->pic_fields.bits.constrained_intra_pred_flag) ||
            !enc_set_s32(context->enc_cfg, "h265:sao_luma_disable",
                         !slice->slice_fields.bits.slice_sao_luma_flag) ||
            !enc_set_s32(context->enc_cfg, "h265:sao_chroma_disable",
                         !slice->slice_fields.bits.slice_sao_chroma_flag) ||
            !enc_set_s32(context->enc_cfg, "h265:dblk_disable",
                         slice->slice_fields.bits
                             .slice_deblocking_filter_disabled_flag != 0) ||
            !enc_set_s32(context->enc_cfg, "h265:dblk_alpha",
                         slice->slice_beta_offset_div2) ||
            !enc_set_s32(context->enc_cfg, "h265:dblk_beta",
                         slice->slice_tc_offset_div2))
            return false;
    }

    if (rc_mode == MPP_ENC_RC_MODE_FIXQP) {
        if (!enc_set_s32(context->enc_cfg, "rc:qp_min", qp) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_max", qp) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_min_i", qp) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_max_i", qp) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_ip", 0))
            return false;
    } else {
        int32_t max_bps = bounded_bitrate(
            rc_mode == MPP_ENC_RC_MODE_CBR
                ? (int64_t)bitrate * 17 / 16
                : (int64_t)bitrate * 3 / 2);
        int32_t min_bps = bounded_bitrate(
            rc_mode == MPP_ENC_RC_MODE_CBR
                ? (int64_t)bitrate * 15 / 16 : bitrate / 16);
        if (!enc_set_s32(context->enc_cfg, "rc:bps_target", bitrate) ||
            !enc_set_s32(context->enc_cfg, "rc:bps_max", max_bps) ||
            !enc_set_s32(context->enc_cfg, "rc:bps_min", min_bps) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_min", 0) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_max", 48) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_min_i", 0) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_max_i", 48) ||
            !enc_set_s32(context->enc_cfg, "rc:qp_ip", 2))
            return false;
    }

    if (context->mpi->control(context->mpp, MPP_ENC_SET_CFG,
                              context->enc_cfg) != MPP_OK) {
        LOG_ERROR("encoder MPP_ENC_SET_CFG failed");
        return false;
    }
    return true;
}

VAStatus rk_mpp_enc_init(RKContext *context)
{
    if (mpp_enc_cfg_init(&context->enc_cfg) != MPP_OK ||
        context->mpi->control(context->mpp, MPP_ENC_GET_CFG,
                              context->enc_cfg) != MPP_OK) {
        LOG_ERROR("encoder config initialization failed");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    RK_S64 input_timeout = 1000;
    RK_S64 output_timeout = 1000;
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (context->mpi->control(context->mpp, MPP_SET_INPUT_TIMEOUT,
                              &input_timeout) != MPP_OK ||
        context->mpi->control(context->mpp, MPP_SET_OUTPUT_TIMEOUT,
                              &output_timeout) != MPP_OK ||
        context->mpi->control(context->mpp, MPP_ENC_SET_HEADER_MODE,
                              &header_mode) != MPP_OK) {
        LOG_ERROR("encoder timeout/header configuration failed");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus rk_mpp_enc_render_buffer(RKContext *context, RKBuffer *buffer)
{
    size_t bytes = (size_t)buffer->size * buffer->num_elements;
    switch (buffer->type) {
    case VAEncSequenceParameterBufferType:
        if (context->coding == MPP_VIDEO_CodingAVC) {
            if (bytes < sizeof(context->enc_seq))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            memcpy(&context->enc_seq, buffer->data, sizeof(context->enc_seq));
            if (!context->enc_seq.picture_width_in_mbs ||
                !context->enc_seq.picture_height_in_mbs ||
                context->enc_seq.picture_width_in_mbs !=
                    (uint16_t)((context->width + 15) / 16) ||
                context->enc_seq.picture_height_in_mbs !=
                    (uint16_t)((context->height + 15) / 16) ||
                context->enc_seq.intra_period > INT_MAX)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
        } else {
            if (bytes < sizeof(context->enc_hevc_seq))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            memcpy(&context->enc_hevc_seq, buffer->data,
                   sizeof(context->enc_hevc_seq));
            const VAEncSequenceParameterBufferHEVC *seq =
                &context->enc_hevc_seq;
            bool visible_dimensions =
                context->render_surface &&
                seq->pic_width_in_luma_samples ==
                    (uint16_t)context->render_surface->width &&
                seq->pic_height_in_luma_samples ==
                    (uint16_t)context->render_surface->height;
            bool aligned_context_dimensions =
                context->render_surface &&
                ((context->render_surface->width + 15) & ~15) ==
                    context->width &&
                ((context->render_surface->height + 15) & ~15) ==
                    context->height &&
                seq->pic_width_in_luma_samples ==
                    (uint16_t)context->width &&
                seq->pic_height_in_luma_samples ==
                    (uint16_t)context->height;
            if (seq->general_profile_idc != 1 ||
                (!visible_dimensions && !aligned_context_dimensions) ||
                seq->seq_fields.bits.chroma_format_idc != 1 ||
                seq->seq_fields.bits.separate_colour_plane_flag ||
                seq->seq_fields.bits.bit_depth_luma_minus8 ||
                seq->seq_fields.bits.bit_depth_chroma_minus8 ||
                seq->seq_fields.bits.scaling_list_enabled_flag ||
                seq->seq_fields.bits.pcm_enabled_flag ||
                seq->ip_period > 1 || seq->intra_period > INT_MAX ||
                !hevc_ctu_size(context))
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            LOG("HEVC encoder sequence profile=%u level=%u %ux%u "
                "va_ctu=%u gop=%u ip=%u bitrate=%u",
                seq->general_profile_idc, seq->general_level_idc,
                seq->pic_width_in_luma_samples,
                seq->pic_height_in_luma_samples, hevc_ctu_size(context),
                seq->intra_period, seq->ip_period, seq->bits_per_second);
        }
        context->has_enc_seq = true;
        return VA_STATUS_SUCCESS;
    case VAEncPictureParameterBufferType:
        if (context->coding == MPP_VIDEO_CodingAVC) {
            if (bytes < sizeof(context->enc_pic))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            memcpy(&context->enc_pic, buffer->data,
                   sizeof(context->enc_pic));
        } else {
            if (bytes < sizeof(context->enc_hevc_pic))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            memcpy(&context->enc_hevc_pic, buffer->data,
                   sizeof(context->enc_hevc_pic));
            const VAEncPictureParameterBufferHEVC *pic =
                &context->enc_hevc_pic;
            if (pic->num_tile_columns_minus1 ||
                pic->num_tile_rows_minus1 ||
                pic->pic_fields.bits.tiles_enabled_flag ||
                pic->pic_fields.bits.scaling_list_data_present_flag ||
                pic->pic_fields.bits.weighted_pred_flag ||
                pic->pic_fields.bits.weighted_bipred_flag ||
                pic->pic_fields.bits.coding_type < 1 ||
                pic->pic_fields.bits.coding_type > 2 ||
                pic->diff_cu_qp_delta_depth > 2)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            LOG("HEVC encoder picture qp=%u diff_cu_qp=%u coding=%u "
                "idr=%u sao=%u weighted=%u/%u",
                pic->pic_init_qp, pic->diff_cu_qp_delta_depth,
                pic->pic_fields.bits.coding_type,
                pic->pic_fields.bits.idr_pic_flag,
                context->enc_hevc_seq.seq_fields.bits
                    .sample_adaptive_offset_enabled_flag,
                pic->pic_fields.bits.weighted_pred_flag,
                pic->pic_fields.bits.weighted_bipred_flag);
        }
        context->has_enc_pic = true;
        return VA_STATUS_SUCCESS;
    case VAEncSliceParameterBufferType:
        if (!buffer->num_elements)
            return VA_STATUS_ERROR_INVALID_BUFFER;
        if (context->coding == MPP_VIDEO_CodingAVC) {
            if (buffer->size < sizeof(context->enc_slice))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            uint32_t units_per_row = ((uint32_t)context->width + 15) / 16;
            for (unsigned int i = 0; i < buffer->num_elements; i++) {
                VAEncSliceParameterBufferH264 slice;
                memcpy(&slice, (const uint8_t *)buffer->data +
                               (size_t)i * buffer->size, sizeof(slice));
                if ((context->enc_slice_count &&
                     context->enc_slice.slice_type != slice.slice_type) ||
                    !add_encoder_slice(context, slice.macroblock_address,
                                       slice.num_macroblocks,
                                       units_per_row))
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                if (context->enc_slice_count == 1)
                    context->enc_slice = slice;
            }
        } else {
            if (buffer->size < sizeof(context->enc_hevc_slice))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            uint32_t ctu = hevc_ctu_size(context);
            uint32_t units_per_row =
                ctu ? ((uint32_t)context->render_surface->width + ctu - 1) /
                          ctu
                    : 0;
            for (unsigned int i = 0; i < buffer->num_elements; i++) {
                VAEncSliceParameterBufferHEVC slice;
                memcpy(&slice, (const uint8_t *)buffer->data +
                               (size_t)i * buffer->size, sizeof(slice));
                if ((context->enc_slice_count &&
                     (!hevc_slices_compatible(&context->enc_hevc_slice,
                                              &slice) ||
                      context->enc_hevc_slice.slice_fields.bits
                          .last_slice_of_pic_flag)) ||
                    !add_encoder_slice(context, slice.slice_segment_address,
                                       slice.num_ctu_in_slice,
                                       units_per_row))
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                if (context->enc_slice_count == 1)
                    context->enc_hevc_slice = slice;
                else if (slice.slice_fields.bits.last_slice_of_pic_flag)
                    context->enc_hevc_slice.slice_fields.bits
                        .last_slice_of_pic_flag = 1;
                LOG("HEVC encoder slice address=%u ctus=%u type=%u last=%u "
                    "qp_delta=%d sao=%u/%u",
                    slice.slice_segment_address, slice.num_ctu_in_slice,
                    slice.slice_type,
                    slice.slice_fields.bits.last_slice_of_pic_flag,
                    slice.slice_qp_delta,
                    slice.slice_fields.bits.slice_sao_luma_flag,
                    slice.slice_fields.bits.slice_sao_chroma_flag);
            }
        }
        context->has_enc_slice = true;
        return VA_STATUS_SUCCESS;
    case VAEncMiscParameterBufferType: {
        if (bytes < sizeof(VAEncMiscParameterBuffer))
            return VA_STATUS_ERROR_INVALID_BUFFER;
        const VAEncMiscParameterBuffer *misc = buffer->data;
        if (misc->type == VAEncMiscParameterTypeRateControl) {
            if (bytes < sizeof(*misc) + sizeof(VAEncMiscParameterRateControl))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            const VAEncMiscParameterRateControl *rc =
                (const VAEncMiscParameterRateControl *)misc->data;
            if (rc->bits_per_second > INT_MAX)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            context->enc_bitrate = rc->bits_per_second;
        } else if (misc->type == VAEncMiscParameterTypeFrameRate) {
            if (bytes < sizeof(*misc) + sizeof(VAEncMiscParameterFrameRate))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            const VAEncMiscParameterFrameRate *fps =
                (const VAEncMiscParameterFrameRate *)misc->data;
            context->enc_fps_num = fps->framerate & 0xffffu;
            context->enc_fps_den = fps->framerate >> 16;
            if (!context->enc_fps_den)
                context->enc_fps_den = 1;
            if (!context->enc_fps_num)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        return VA_STATUS_SUCCESS;
    }
    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
}

VAStatus rk_mpp_enc_encode(RKContext *context)
{
    if (!context->has_enc_seq || !context->has_enc_pic ||
        !context->has_enc_slice || !context->render_surface)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    VABufferID coded_id;
    bool request_idr;
    if (context->coding == MPP_VIDEO_CodingAVC) {
        if (context->enc_slice_units !=
                (uint32_t)(((context->width + 15) / 16) *
                           ((context->height + 15) / 16)))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        coded_id = context->enc_pic.coded_buf;
        request_idr = context->enc_pic.pic_fields.bits.idr_pic_flag;
    } else {
        uint32_t ctu = hevc_ctu_size(context);
        if (!ctu)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        uint32_t ctu_count =
            ((uint32_t)context->render_surface->width + ctu - 1) / ctu *
            (((uint32_t)context->render_surface->height + ctu - 1) / ctu);
        if (context->enc_slice_units != ctu_count ||
            !context->enc_hevc_slice.slice_fields.bits.last_slice_of_pic_flag)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        coded_id = context->enc_hevc_pic.coded_buf;
        request_idr = context->enc_hevc_pic.pic_fields.bits.idr_pic_flag;
    }
    LOG("encoder slice split count=%u units=%u span=%u",
        context->enc_slice_count, context->enc_slice_units,
        context->enc_slice_unit_span);

    RKBuffer *coded = buffer_acquire(context->driver, coded_id);
    if (!coded || coded->type != VAEncCodedBufferType) {
        if (coded)
            rk_object_unref(&coded->base);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    coded->coded_ready = false;
    coded->coded_failed = false;

    VAStatus status = VA_STATUS_ERROR_ENCODING_ERROR;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    RKSurface *surface = context->render_surface;
    pthread_mutex_lock(&surface->lock);
    bool import_ready = !surface->imported_multiplane ||
                        rk_surface_normalize_multiplane_import(surface);
    MppBuffer input = surface->import_buf && !surface->imported_rgb &&
                      !surface->imported_multiplane
                    ? surface->import_buf : surface->priv_buf;
    if (!import_ready || !input ||
        MPP_FRAME_FMT_IS_YUV_10BIT(surface->fmt) ||
        (surface->imported_rgb &&
         !rk_convert_rgb_to_nv12(
             mpp_buffer_get_fd(surface->import_buf), surface->import_size,
             surface->fourcc, surface->import_pitch,
             (uint32_t)surface->width, (uint32_t)surface->height, input,
             (uint32_t)surface->hstride, (uint32_t)surface->vstride)) ||
        !configure_encoder(context) ||
        mpp_frame_init(&frame) != MPP_OK) {
        goto out;
    }

    mpp_frame_set_width(frame, (RK_U32)surface->width);
    mpp_frame_set_height(frame, (RK_U32)surface->height);
    mpp_frame_set_hor_stride(frame, (RK_U32)surface->hstride);
    mpp_frame_set_ver_stride(frame, (RK_U32)surface->vstride);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buf_size(frame, mpp_buffer_get_size(input));
    mpp_frame_set_buffer(frame, input);

    if (request_idr &&
        context->mpi->control(context->mpp, MPP_ENC_SET_IDR_FRAME, NULL) !=
            MPP_OK) {
        LOG_ERROR("encoder failed to request IDR frame");
        goto out;
    }
    if (context->mpi->encode_put_frame(context->mpp, frame) != MPP_OK) {
        LOG_ERROR("encoder failed to submit frame");
        goto out;
    }
    if (context->mpi->encode_get_packet(context->mpp, &packet) != MPP_OK ||
        !packet) {
        LOG_ERROR("encoder failed to receive packet");
        goto out;
    }

    const void *position = mpp_packet_get_pos(packet);
    size_t length = mpp_packet_get_length(packet);
    if (!position || !length || length > UINT32_MAX) {
        LOG_ERROR("encoder returned invalid packet pos=%p length=%zu",
                  position, length);
        goto out;
    }
    status = rk_buffer_store_coded(coded, position, length, 0);
    LOG("encoder produced %zu bytes for surface=0x%x coded=0x%x",
        length, context->render_target, coded_id);

out:
    if (status != VA_STATUS_SUCCESS) {
        coded->coded_ready = true;
        coded->coded_failed = true;
    }
    if (packet)
        mpp_packet_deinit(&packet);
    if (frame)
        mpp_frame_deinit(&frame);
    pthread_mutex_unlock(&surface->lock);
    rk_object_unref(&coded->base);
    return status;
}
