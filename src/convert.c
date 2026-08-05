#include "convert.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include <va/va.h>

#include "frame_layout.h"
#include "log.h"

#ifdef RK_HAVE_RGA
#include <im2d.h>
#endif

bool rk_rga_available(void)
{
#ifdef RK_HAVE_RGA
    return true;
#else
    return false;
#endif
}

bool rk_rga_nv15_to_p010_geometry_supported(uint32_t width,
                                             bool source_afbc)
{
    /* RK3588 AFBC jobs can run only on RGA3. The vendor driver gives RGA3
     * both an input and output minimum active width of 68; RGA2 can handle
     * narrower raster jobs, so this must not become a global width floor. */
    return !source_afbc || width >= RK_RGA3_MIN_ACTIVE_WIDTH;
}

static int rgb_rga_format(uint32_t fourcc)
{
#ifdef RK_HAVE_RGA
    switch (fourcc) {
    case VA_FOURCC_RGBA: return RK_FORMAT_RGBA_8888;
    case VA_FOURCC_RGBX: return RK_FORMAT_RGBX_8888;
    case VA_FOURCC_BGRA: return RK_FORMAT_BGRA_8888;
    case VA_FOURCC_BGRX: return RK_FORMAT_BGRX_8888;
    default:             return -1;
    }
#else
    (void)fourcc;
    return -1;
#endif
}

bool rk_convert_rgb_to_nv12(int source_fd, size_t source_size,
                            uint32_t source_fourcc, uint32_t source_pitch,
                            uint32_t width, uint32_t height,
                            MppBuffer destination,
                            uint32_t destination_stride,
                            uint32_t destination_vertical_stride)
{
    size_t source_layout_size = 0;
    size_t destination_layout_size = 0;
    int source_format = rgb_rga_format(source_fourcc);
    if (height && source_pitch > SIZE_MAX / height)
        source_layout_size = SIZE_MAX;
    else if (height)
        source_layout_size = (size_t)source_pitch * height;
    if (source_fd < 0 || !width || !height || (width & 1) || (height & 1) ||
        source_format < 0 || source_pitch % 4 != 0 ||
        source_pitch / 4 < width || source_layout_size > source_size ||
        !destination || destination_stride < width ||
        destination_vertical_stride < height ||
        !rk_nv12_layout_size(destination_stride,
                             destination_vertical_stride,
                             &destination_layout_size) ||
        destination_layout_size > mpp_buffer_get_size(destination)) {
        LOG("convert: invalid RGB/NV12 layout fourcc=0x%x source_fd=%d "
            "source_size=%zu source_pitch=%u size=%ux%u dst_stride=%u "
            "dst_vstride=%u",
            source_fourcc, source_fd, source_size, source_pitch, width,
            height, destination_stride, destination_vertical_stride);
        return false;
    }

#ifndef RK_HAVE_RGA
    LOG("convert: RGA support not compiled in; cannot convert RGB to NV12");
    return false;
#else
    int destination_fd = mpp_buffer_get_fd(destination);
    if (destination_fd < 0 || source_size > (size_t)INT_MAX ||
        destination_layout_size > (size_t)INT_MAX ||
        source_pitch / 4 > (uint32_t)INT_MAX ||
        width > (uint32_t)INT_MAX || height > (uint32_t)INT_MAX ||
        destination_stride > (uint32_t)INT_MAX ||
        destination_vertical_stride > (uint32_t)INT_MAX) {
        LOG("convert: RGB/NV12 buffer not importable source_fd=%d "
            "destination_fd=%d source_size=%zu destination_size=%zu",
            source_fd, destination_fd, source_size, destination_layout_size);
        return false;
    }

    rga_buffer_t source_image = wrapbuffer_fd_t(
        source_fd, (int)width, (int)height, (int)(source_pitch / 4),
        (int)height, source_format);
    rga_buffer_t destination_image = wrapbuffer_fd_t(
        destination_fd, (int)width, (int)height, (int)destination_stride,
        (int)destination_vertical_stride, RK_FORMAT_YCbCr_420_SP);
    im_rect source_rect = {
        .x = 0,
        .y = 0,
        .width = (int)width,
        .height = (int)height,
    };
    im_rect destination_rect = source_rect;
    IM_STATUS status = improcess(source_image, destination_image,
                                 (rga_buffer_t){0}, source_rect,
                                 destination_rect, (im_rect){0}, IM_SYNC);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        LOG("convert: RGA RGB->NV12 failed status=%d (%s)", (int)status,
            imStrError_t(status));
        return false;
    }

    LOG("convert: RGB->NV12 fourcc=0x%x %ux%u source_pitch=%u "
        "destination_stride=%u destination_vstride=%u source_fd=%d "
        "destination_fd=%d",
        source_fourcc, width, height, source_pitch, destination_stride,
        destination_vertical_stride, source_fd, destination_fd);
    return true;
#endif
}

bool rk_repack_nv12(MppBuffer source, uint32_t width, uint32_t height,
                    uint32_t source_stride,
                    uint32_t source_vertical_stride,
                    MppBuffer destination, uint32_t destination_stride,
                    uint32_t destination_vertical_stride)
{
    size_t source_layout_size = 0;
    size_t destination_layout_size = 0;
    if (!source || !destination || !width || !height ||
        (width & 1u) || (height & 1u) || source_stride < width ||
        source_vertical_stride < height || destination_stride < width ||
        destination_vertical_stride < height ||
        !rk_nv12_layout_size(source_stride, source_vertical_stride,
                             &source_layout_size) ||
        !rk_nv12_layout_size(destination_stride,
                             destination_vertical_stride,
                             &destination_layout_size) ||
        source_layout_size > mpp_buffer_get_size(source) ||
        destination_layout_size > mpp_buffer_get_size(destination)) {
        LOG("convert: invalid NV12 repack layout size=%ux%u source=%ux%u "
            "destination=%ux%u source_size=%zu destination_size=%zu",
            width, height, source_stride, source_vertical_stride,
            destination_stride, destination_vertical_stride,
            source ? mpp_buffer_get_size(source) : 0,
            destination ? mpp_buffer_get_size(destination) : 0);
        return false;
    }

#ifndef RK_HAVE_RGA
    LOG("convert: RGA support not compiled in; cannot repack NV12");
    return false;
#else
    int source_fd = mpp_buffer_get_fd(source);
    int destination_fd = mpp_buffer_get_fd(destination);
    if (source_fd < 0 || destination_fd < 0 ||
        source_layout_size > (size_t)INT_MAX ||
        destination_layout_size > (size_t)INT_MAX ||
        width > (uint32_t)INT_MAX || height > (uint32_t)INT_MAX ||
        source_stride > (uint32_t)INT_MAX ||
        source_vertical_stride > (uint32_t)INT_MAX ||
        destination_stride > (uint32_t)INT_MAX ||
        destination_vertical_stride > (uint32_t)INT_MAX) {
        LOG("convert: NV12 repack buffer not importable source_fd=%d "
            "destination_fd=%d source_size=%zu destination_size=%zu",
            source_fd, destination_fd, source_layout_size,
            destination_layout_size);
        return false;
    }

    rga_buffer_t source_image = wrapbuffer_fd_t(
        source_fd, (int)width, (int)height, (int)source_stride,
        (int)source_vertical_stride, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t destination_image = wrapbuffer_fd_t(
        destination_fd, (int)width, (int)height, (int)destination_stride,
        (int)destination_vertical_stride, RK_FORMAT_YCbCr_420_SP);
    im_rect rect = {
        .x = 0,
        .y = 0,
        .width = (int)width,
        .height = (int)height,
    };
    IM_STATUS status = improcess(source_image, destination_image,
                                 (rga_buffer_t){0}, rect, rect,
                                 (im_rect){0}, IM_SYNC);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        LOG_WARNING("convert: RGA NV12 repack failed %ux%u %ux%u -> "
                    "%ux%u status=%d (%s)", width, height, source_stride,
                    source_vertical_stride, destination_stride,
                    destination_vertical_stride, (int)status,
                    imStrError_t(status));
        return false;
    }

    LOG("convert: NV12 repack %ux%u %ux%u -> %ux%u source_fd=%d "
        "destination_fd=%d", width, height, source_stride,
        source_vertical_stride, destination_stride,
        destination_vertical_stride, source_fd, destination_fd);
    return true;
#endif
}

bool rk_repack_p010(MppBuffer source, uint32_t width, uint32_t height,
                    uint32_t source_pixel_stride,
                    uint32_t source_vertical_stride,
                    MppBuffer destination,
                    uint32_t destination_pixel_stride,
                    uint32_t destination_vertical_stride)
{
    size_t source_layout_size = 0;
    size_t destination_layout_size = 0;
    if (!source || !destination || !width || !height || (width & 1u) ||
        (height & 1u) || source_pixel_stride < width ||
        source_vertical_stride < height ||
        destination_pixel_stride < width ||
        destination_vertical_stride < height ||
        !rk_p010_layout_size(source_pixel_stride, source_vertical_stride,
                             &source_layout_size) ||
        !rk_p010_layout_size(destination_pixel_stride,
                             destination_vertical_stride,
                             &destination_layout_size) ||
        source_layout_size > mpp_buffer_get_size(source) ||
        destination_layout_size > mpp_buffer_get_size(destination)) {
        LOG("convert: invalid P010 repack layout size=%ux%u source=%ux%u "
            "destination=%ux%u source_size=%zu destination_size=%zu",
            width, height, source_pixel_stride, source_vertical_stride,
            destination_pixel_stride, destination_vertical_stride,
            source ? mpp_buffer_get_size(source) : 0,
            destination ? mpp_buffer_get_size(destination) : 0);
        return false;
    }

    /* RK3588 RGA reports success for a P010-to-P010 blit but leaves the
     * destination unchanged on the qualified stack. Both objects are MPP DRM
     * buffers and CPU-mappable, so use explicit cache ownership and checked
     * row copies for this opt-in 10-bit stable-export path. */
    if (mpp_buffer_sync_ro_begin(source) != MPP_OK)
        return false;
    bool destination_started =
        mpp_buffer_sync_begin(destination) == MPP_OK;
    uint8_t *source_bytes = mpp_buffer_get_ptr(source);
    uint8_t *destination_bytes = mpp_buffer_get_ptr(destination);
    bool copied = destination_started && source_bytes && destination_bytes;
    if (copied) {
        size_t source_byte_stride = (size_t)source_pixel_stride * 2u;
        size_t destination_byte_stride =
            (size_t)destination_pixel_stride * 2u;
        size_t row_bytes = (size_t)width * 2u;
        memset(destination_bytes, 0, destination_layout_size);
        for (uint32_t row = 0; row < height; row++)
            memcpy(destination_bytes +
                       (size_t)row * destination_byte_stride,
                   source_bytes + (size_t)row * source_byte_stride,
                   row_bytes);
        uint8_t *source_uv = source_bytes +
            source_byte_stride * source_vertical_stride;
        uint8_t *destination_uv = destination_bytes +
            destination_byte_stride * destination_vertical_stride;
        for (uint32_t row = 0; row < height / 2u; row++)
            memcpy(destination_uv +
                       (size_t)row * destination_byte_stride,
                   source_uv + (size_t)row * source_byte_stride,
                   row_bytes);
    }
    bool sync_ok = true;
    if (destination_started)
        sync_ok = mpp_buffer_sync_end(destination) == MPP_OK;
    sync_ok &= mpp_buffer_sync_ro_end(source) == MPP_OK;
    if (!copied || !sync_ok) {
        LOG("convert: synchronized P010 repack failed %ux%u %ux%u -> "
            "%ux%u", width, height, source_pixel_stride,
            source_vertical_stride, destination_pixel_stride,
            destination_vertical_stride);
        return false;
    }

    LOG("convert: synchronized P010 repack %ux%u %ux%u -> %ux%u",
        width, height, source_pixel_stride, source_vertical_stride,
        destination_pixel_stride, destination_vertical_stride);
    return true;
}

bool rk_convert_nv15_to_p010(MppBufferGroup group, MppBuffer source,
                             uint32_t width, uint32_t height,
                             uint32_t source_byte_stride,
                             uint32_t pixel_stride,
                             uint32_t vertical_stride,
                             uint32_t source_offset_x,
                             uint32_t source_offset_y,
                             bool source_afbc,
                             MppBuffer *converted_out)
{
    if (!converted_out)
        return false;
    *converted_out = NULL;

    if (!rk_rga_nv15_to_p010_geometry_supported(width, source_afbc)) {
        LOG_WARNING("convert: AFBC NV15->P010 active width=%u is below "
                    "RGA3 minimum=%u; refusing before RGA submission",
                    width, RK_RGA3_MIN_ACTIVE_WIDTH);
        return false;
    }

    size_t source_size = source ? mpp_buffer_get_size(source) : 0;
    size_t source_layout_size = 0;
    size_t converted_size = 0;
    bool source_layout_valid = source_afbc ||
        (source_byte_stride &&
         rk_nv12_layout_size(source_byte_stride, vertical_stride,
                             &source_layout_size) &&
         source_size >= source_layout_size);
    if (!group || !source || !width || !height || !pixel_stride ||
        !vertical_stride || !source_layout_valid ||
        source_offset_x > pixel_stride ||
        width > pixel_stride - source_offset_x ||
        source_offset_y > vertical_stride ||
        height > vertical_stride - source_offset_y ||
        !rk_p010_layout_size(pixel_stride, vertical_stride,
                             &converted_size)) {
        LOG("convert: invalid NV15/P010 layout src_stride=%u pixel_stride=%u "
            "vstride=%u offset=(%u,%u) afbc=%d source_size=%zu "
            "source_layout=%zu",
            source_byte_stride, pixel_stride, vertical_stride,
            source_offset_x, source_offset_y, source_afbc, source_size,
            source_layout_size);
        return false;
    }

    MppBuffer converted = NULL;
    if (mpp_buffer_get(group, &converted, converted_size) != MPP_OK) {
        LOG("convert: P010 buffer allocation failed size=%zu", converted_size);
        return false;
    }

#ifndef RK_HAVE_RGA
    LOG("convert: RGA support not compiled in; cannot convert NV15 to P010");
    mpp_buffer_put(converted);
    return false;
#else
    int source_fd = mpp_buffer_get_fd(source);
    int converted_fd = mpp_buffer_get_fd(converted);
    if (source_fd < 0 || converted_fd < 0 ||
        source_size > (size_t)INT_MAX ||
        converted_size > (size_t)INT_MAX ||
        width > (uint32_t)INT_MAX || height > (uint32_t)INT_MAX ||
        source_byte_stride > (uint32_t)INT_MAX ||
        pixel_stride > (uint32_t)INT_MAX ||
        vertical_stride > (uint32_t)INT_MAX) {
        LOG("convert: buffer not importable source_fd=%d converted_fd=%d "
            "source_size=%zu converted_size=%zu",
            source_fd, converted_fd, source_size, converted_size);
        mpp_buffer_put(converted);
        return false;
    }

    rga_buffer_t source_image = wrapbuffer_fd_t(
        source_fd, (int)width, (int)height, (int)pixel_stride,
        (int)vertical_stride, RK_FORMAT_YCbCr_420_SP_10B);
    if (source_afbc)
        source_image.rd_mode = IM_AFBC16x16_MODE;
    rga_buffer_t converted_image = wrapbuffer_fd_t(
        converted_fd, (int)width, (int)height, (int)pixel_stride,
        (int)vertical_stride, RK_FORMAT_P010);

    im_rect source_rect = {
        .x = (int)source_offset_x,
        .y = (int)source_offset_y,
        .width = (int)width,
        .height = (int)height,
    };
    im_rect converted_rect = {
        .x = 0,
        .y = 0,
        .width = (int)width,
        .height = (int)height,
    };
    IM_STATUS status = improcess(source_image, converted_image,
                                 (rga_buffer_t){0}, source_rect,
                                 converted_rect, (im_rect){0}, IM_SYNC);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        /* RGA3 on RK3588 refuses sources below its minimum active width, so a
         * very narrow 10-bit picture cannot be repacked here at all. Name the
         * geometry: the status string alone reads as a transient driver fault
         * rather than a size the hardware will never accept. */
        LOG_WARNING("convert: RGA NV15->P010 refused %ux%u "
                    "(pixel_stride=%u vstride=%u afbc=%d): status=%d (%s)",
                    width, height, pixel_stride, vertical_stride, source_afbc,
                    (int)status, imStrError_t(status));
        mpp_buffer_put(converted);
        return false;
    }

    LOG("convert: NV15->P010 %ux%u src_stride_bytes=%u pixel_stride=%u "
        "vstride=%u offset=(%u,%u) afbc=%d fd=%d",
        width, height, source_byte_stride, pixel_stride, vertical_stride,
        source_offset_x, source_offset_y, source_afbc, converted_fd);
    *converted_out = converted;
    return true;
#endif
}
