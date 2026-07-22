#ifndef ROCKCHIP_VAAPI_FRAME_LAYOUT_H
#define ROCKCHIP_VAAPI_FRAME_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

/* Size of a linear NV12 buffer whose luma and chroma planes share byte_stride. */
bool rk_nv12_layout_size(size_t byte_stride, size_t vertical_stride,
                         size_t *size_out);

/* Conservative allocation for a surface before MPP reports its actual stride. */
bool rk_surface_buffer_size(unsigned width, unsigned height, size_t *size_out);

/* Copy one complete padded NV12 layout after checking both buffer bounds. */
bool rk_copy_nv12_frame(void *dst, size_t dst_size,
                        const void *src, size_t src_size,
                        size_t byte_stride, size_t vertical_stride);

#endif
