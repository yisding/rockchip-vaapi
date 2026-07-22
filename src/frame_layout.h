#ifndef ROCKCHIP_VAAPI_FRAME_LAYOUT_H
#define ROCKCHIP_VAAPI_FRAME_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

/* Size of a linear NV12 buffer whose luma and chroma planes share byte_stride. */
bool rk_nv12_layout_size(size_t byte_stride, size_t vertical_stride,
                         size_t *size_out);

/* Conservative 8-bit MPP decode allocation, including codec/HAL side data. */
bool rk_surface_buffer_size(unsigned width, unsigned height, size_t *size_out);

#endif
