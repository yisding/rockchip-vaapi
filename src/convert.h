#ifndef RK_VAAPI_CONVERT_H
#define RK_VAAPI_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

#include <rockchip/mpp_buffer.h>

bool rk_rga_available(void);

bool rk_convert_nv15_to_p010(MppBufferGroup group, MppBuffer source,
                             uint32_t width, uint32_t height,
                             uint32_t source_byte_stride,
                             uint32_t pixel_stride,
                             uint32_t vertical_stride,
                             uint32_t source_offset_x,
                             uint32_t source_offset_y,
                             bool source_afbc,
                             MppBuffer *converted_out);

#endif
