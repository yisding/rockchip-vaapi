#ifndef RK_VAAPI_CONVERT_H
#define RK_VAAPI_CONVERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rockchip/mpp_buffer.h>

#define RK_RGA3_MIN_ACTIVE_WIDTH 68u

bool rk_rga_available(void);

bool rk_rga_nv15_to_p010_geometry_supported(uint32_t width,
                                             bool source_afbc);

bool rk_convert_nv15_to_p010(MppBufferGroup group, MppBuffer source,
                             uint32_t width, uint32_t height,
                             uint32_t source_byte_stride,
                             uint32_t pixel_stride,
                             uint32_t vertical_stride,
                             uint32_t source_offset_x,
                             uint32_t source_offset_y,
                             bool source_afbc,
                             MppBuffer *converted_out);

bool rk_convert_rgb_to_nv12(int source_fd, size_t source_size,
                            uint32_t source_fourcc, uint32_t source_pitch,
                            uint32_t width, uint32_t height,
                            MppBuffer destination,
                            uint32_t destination_stride,
                            uint32_t destination_vertical_stride);

bool rk_repack_nv12(MppBuffer source, uint32_t width, uint32_t height,
                    uint32_t source_stride,
                    uint32_t source_vertical_stride,
                    MppBuffer destination, uint32_t destination_stride,
                    uint32_t destination_vertical_stride);

bool rk_repack_p010(MppBuffer source, uint32_t width, uint32_t height,
                    uint32_t source_pixel_stride,
                    uint32_t source_vertical_stride,
                    MppBuffer destination,
                    uint32_t destination_pixel_stride,
                    uint32_t destination_vertical_stride);

#endif
