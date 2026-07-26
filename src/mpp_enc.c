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
#include "log.h"

static bool enc_set_s32(MppEncCfg cfg, const char *key, int32_t value)
{
    if (mpp_enc_cfg_set_s32(cfg, key, value) != MPP_OK) {
        LOG("encoder config rejected %s=%d", key, value);
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

static bool configure_encoder(RKContext *context)
{
    const VAEncSequenceParameterBufferH264 *seq = &context->enc_seq;
    const VAEncPictureParameterBufferH264 *pic = &context->enc_pic;
    int32_t hstride = (context->width + 15) & ~15;
    int32_t vstride = (context->height + 15) & ~15;
    int32_t bitrate = bounded_bitrate(context->enc_bitrate
                                   ? context->enc_bitrate
                                   : seq->bits_per_second
                                   ? seq->bits_per_second : 2000000);
    int32_t gop = seq->intra_period
                ? (int32_t)seq->intra_period : 60;
    int32_t qp = pic->pic_init_qp >= 1 && pic->pic_init_qp <= 51
               ? pic->pic_init_qp : 26;
    uint32_t rc_mode = mpp_rc_mode(context->rate_control);
    uint32_t fps_num = context->enc_fps_num ? context->enc_fps_num : 30;
    uint32_t fps_den = context->enc_fps_den ? context->enc_fps_den : 1;

    if (!enc_set_s32(context->enc_cfg, "prep:width", context->width) ||
        !enc_set_s32(context->enc_cfg, "prep:height", context->height) ||
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
        !enc_set_s32(context->enc_cfg, "h264:profile",
                     context->profile == VAProfileH264High ? 100 : 77) ||
        !enc_set_s32(context->enc_cfg, "h264:level",
                     seq->level_idc ? seq->level_idc : 40) ||
        !enc_set_s32(context->enc_cfg, "h264:cabac_en",
                     pic->pic_fields.bits.entropy_coding_mode_flag) ||
        !enc_set_s32(context->enc_cfg, "h264:cabac_idc", 0) ||
        !enc_set_s32(context->enc_cfg, "h264:trans8x8",
                     pic->pic_fields.bits.transform_8x8_mode_flag)) {
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
        LOG("encoder MPP_ENC_SET_CFG failed");
        return false;
    }
    return true;
}

VAStatus rk_mpp_enc_init(RKContext *context)
{
    if (mpp_enc_cfg_init(&context->enc_cfg) != MPP_OK ||
        context->mpi->control(context->mpp, MPP_ENC_GET_CFG,
                              context->enc_cfg) != MPP_OK) {
        LOG("encoder config initialization failed");
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
        LOG("encoder timeout/header configuration failed");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus rk_mpp_enc_render_buffer(RKContext *context, RKBuffer *buffer)
{
    size_t bytes = (size_t)buffer->size * buffer->num_elements;
    switch (buffer->type) {
    case VAEncSequenceParameterBufferType:
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
        context->has_enc_seq = true;
        return VA_STATUS_SUCCESS;
    case VAEncPictureParameterBufferType:
        if (bytes < sizeof(context->enc_pic))
            return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&context->enc_pic, buffer->data, sizeof(context->enc_pic));
        context->has_enc_pic = true;
        return VA_STATUS_SUCCESS;
    case VAEncSliceParameterBufferType:
        if (buffer->num_elements != 1 || bytes < sizeof(context->enc_slice))
            return VA_STATUS_ERROR_INVALID_BUFFER;
        memcpy(&context->enc_slice, buffer->data, sizeof(context->enc_slice));
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
    if (context->enc_slice.macroblock_address != 0 ||
        context->enc_slice.num_macroblocks !=
            (uint32_t)(((context->width + 15) / 16) *
                       ((context->height + 15) / 16)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    RKBuffer *coded = buffer_acquire(context->driver,
                                     context->enc_pic.coded_buf);
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
    MppBuffer input = surface->priv_buf;
    if (!input || MPP_FRAME_FMT_IS_YUV_10BIT(surface->fmt) ||
        !configure_encoder(context) ||
        mpp_frame_init(&frame) != MPP_OK) {
        goto out;
    }

    mpp_frame_set_width(frame, (RK_U32)context->width);
    mpp_frame_set_height(frame, (RK_U32)context->height);
    mpp_frame_set_hor_stride(frame, (RK_U32)surface->hstride);
    mpp_frame_set_ver_stride(frame, (RK_U32)surface->vstride);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buf_size(frame, mpp_buffer_get_size(input));
    mpp_frame_set_buffer(frame, input);

    if (context->enc_pic.pic_fields.bits.idr_pic_flag &&
        context->mpi->control(context->mpp, MPP_ENC_SET_IDR_FRAME, NULL) !=
            MPP_OK) {
        LOG("encoder failed to request IDR frame");
        goto out;
    }
    if (context->mpi->encode_put_frame(context->mpp, frame) != MPP_OK) {
        LOG("encoder failed to submit frame");
        goto out;
    }
    if (context->mpi->encode_get_packet(context->mpp, &packet) != MPP_OK ||
        !packet) {
        LOG("encoder failed to receive packet");
        goto out;
    }

    const void *position = mpp_packet_get_pos(packet);
    size_t length = mpp_packet_get_length(packet);
    if (!position || !length || length > UINT32_MAX) {
        LOG("encoder returned invalid packet pos=%p length=%zu",
            position, length);
        goto out;
    }
    status = rk_buffer_store_coded(coded, position, length, 0);
    LOG("encoder produced %zu bytes for surface=0x%x coded=0x%x",
        length, context->render_target, context->enc_pic.coded_buf);

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
