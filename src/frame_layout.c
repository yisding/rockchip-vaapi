#include "frame_layout.h"

#include <stdint.h>
#include <string.h>

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
     * 352-pixel frame into a 768-byte stride. Also reserve enough byte stride
     * for a future 16-bit linear surface without growing the stable dmabuf. */
    size_t vp9_stride = aligned_width | 256;
    size_t linear_width;
    if (!align_up(width, 16, &linear_width) || linear_width > SIZE_MAX / 2)
        return false;
    size_t sixteen_bit_stride = linear_width * 2;
    size_t byte_stride = vp9_stride > sixteen_bit_stride
                       ? vp9_stride : sixteen_bit_stride;

    return rk_nv12_layout_size(byte_stride, aligned_height, size_out);
}

bool rk_copy_nv12_frame(void *dst, size_t dst_size,
                        const void *src, size_t src_size,
                        size_t byte_stride, size_t vertical_stride)
{
    size_t required;
    if (!dst || !src ||
        !rk_nv12_layout_size(byte_stride, vertical_stride, &required) ||
        dst_size < required || src_size < required)
        return false;

    memcpy(dst, src, required);
    return true;
}
