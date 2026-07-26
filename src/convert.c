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
                             MppBuffer *converted_out)
{
    if (!converted_out)
        return false;
    *converted_out = NULL;

    size_t source_size = source ? mpp_buffer_get_size(source) : 0;
    size_t source_layout_size = 0;
    size_t converted_size = 0;
    if (!group || !source || !width || !height || !source_byte_stride ||
        !pixel_stride || !vertical_stride ||
        !rk_nv12_layout_size(source_byte_stride, vertical_stride,
                             &source_layout_size) ||
        !rk_p010_layout_size(pixel_stride, vertical_stride,
                             &converted_size) ||
        source_size < source_layout_size) {
        LOG("convert: invalid NV15/P010 layout src_stride=%u pixel_stride=%u "
            "vstride=%u source_size=%zu source_layout=%zu",
            source_byte_stride, pixel_stride, vertical_stride, source_size,
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

    im_handle_param_t source_param = {
        .width = pixel_stride,
        .height = vertical_stride,
        .format = RK_FORMAT_YCbCr_420_SP_10B,
    };
    im_handle_param_t converted_param = {
        .width = pixel_stride,
        .height = vertical_stride,
        .format = RK_FORMAT_P010,
    };
    rga_buffer_handle_t source_handle =
        importbuffer_fd(source_fd, &source_param);
    rga_buffer_handle_t converted_handle =
        importbuffer_fd(converted_fd, &converted_param);
    if (!source_handle || !converted_handle) {
        LOG("convert: RGA import failed source_handle=%u converted_handle=%u",
            source_handle, converted_handle);
        if (source_handle)
            releasebuffer_handle(source_handle);
        if (converted_handle)
            releasebuffer_handle(converted_handle);
        mpp_buffer_put(converted);
        return false;
    }

    rga_buffer_t source_image = wrapbuffer_handle_t(
        source_handle, (int)width, (int)height, (int)pixel_stride,
        (int)vertical_stride, RK_FORMAT_YCbCr_420_SP_10B);
    rga_buffer_t converted_image = wrapbuffer_handle_t(
        converted_handle, (int)width, (int)height, (int)pixel_stride,
        (int)vertical_stride, RK_FORMAT_P010);

    IM_STATUS status = imcvtcolor_t(source_image, converted_image,
                                    RK_FORMAT_YCbCr_420_SP_10B,
                                    RK_FORMAT_P010,
                                    IM_COLOR_SPACE_DEFAULT, 1);
    releasebuffer_handle(source_handle);
    releasebuffer_handle(converted_handle);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        LOG("convert: RGA NV15->P010 failed status=%d (%s)", (int)status,
            imStrError_t(status));
        mpp_buffer_put(converted);
        return false;
    }

    LOG("convert: NV15->P010 %ux%u src_stride_bytes=%u pixel_stride=%u "
        "vstride=%u fd=%d",
        width, height, source_byte_stride, pixel_stride, vertical_stride,
        converted_fd);
    *converted_out = converted;
    return true;
#endif
}
