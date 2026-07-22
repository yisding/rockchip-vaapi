#include "frame_layout.h"

#include <stdint.h>

static bool align_up(size_t value, size_t alignment, size_t *result)
{
    if (!result || !alignment || value > SIZE_MAX - (alignment - 1))
        return false;
    *result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

bool rk_nv12_layout_size(size_t byte_stride, size_t vertical_stride,
                         size_t *size_out)
{
    if (!size_out || !byte_stride || !vertical_stride ||
        (vertical_stride & 1) || byte_stride > SIZE_MAX / vertical_stride)
        return false;

    size_t luma_size = byte_stride * vertical_stride;
    if (luma_size > SIZE_MAX - luma_size / 2)
        return false;

    *size_out = luma_size + luma_size / 2;
    return true;
}

bool rk_surface_buffer_size(unsigned width, unsigned height, size_t *size_out)
{
    size_t aligned_width;
    size_t aligned_height;
    if (!width || !height || !size_out ||
        !align_up(width, 256, &aligned_width) ||
        !align_up(height, 64, &aligned_height))
        return false;

    /* VP9's rkvdec path uses mpp_align_256_odd(width), which can turn a
     * 352-pixel frame into a 768-byte stride. MPP's external decode-buffer
     * contract reserves twice the padded luma size: the first 3/2 is the
     * linear NV12 image and the remaining 1/2 is codec/HAL side data. */
    size_t vp9_stride = aligned_width | 256;
    if (vp9_stride > SIZE_MAX / aligned_height)
        return false;
    size_t luma_size = vp9_stride * aligned_height;
    if (luma_size > SIZE_MAX / 2)
        return false;
    *size_out = luma_size * 2;
    return true;
}
