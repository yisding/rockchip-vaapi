#include "convert.h"

#include <limits.h>
#include <stddef.h>

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
        LOG("convert: RGA NV15->P010 failed status=%d (%s)", (int)status,
            imStrError_t(status));
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
