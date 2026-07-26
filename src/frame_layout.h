#ifndef ROCKCHIP_VAAPI_FRAME_LAYOUT_H
#define ROCKCHIP_VAAPI_FRAME_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

/* Size of a linear NV12 buffer whose luma and chroma planes share byte_stride. */
bool rk_nv12_layout_size(size_t byte_stride, size_t vertical_stride,
                         size_t *size_out);

/* Size of a linear P010 buffer whose luma/chroma planes share pixel_stride. */
bool rk_p010_layout_size(size_t pixel_stride, size_t vertical_stride,
                         size_t *size_out);

/* Conservative 8-bit MPP decode allocation, including codec/HAL side data. */
bool rk_surface_buffer_size(unsigned width, unsigned height, size_t *size_out);

/* Pre-decode export allocation large enough for the declared linear format. */
bool rk_surface_placeholder_size(unsigned width, unsigned height, bool is_10bit,
                                 size_t *size_out);

#endif
