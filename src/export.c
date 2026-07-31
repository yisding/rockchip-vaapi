#include "export.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <drm/drm_fourcc.h>
#include <va/va_drmcommon.h>

#include "convert.h"
#include "driver_internal.h"
#include "frame_layout.h"
#include "surface.h"

enum { DRM_LINEAR_NV12_PITCH_ALIGNMENT = 64 };

VAStatus rk_ExportSurfaceHandle(VADriverContextP context, VASurfaceID id,
                                uint32_t mem_type, uint32_t flags,
                                void *descriptor)
{
    RKDriver *driver = drv_from_ctx(context);
    RKSurface *surface = surface_acquire(driver, id);
    LOG("ExportSurfaceHandle: surface=0x%x mem_type=0x%x flags=0x%x",
        id, mem_type, flags);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        LOG("ExportSurfaceHandle: unsupported mem_type 0x%x", mem_type);
        rk_object_unref(&surface->base);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (!descriptor) {
        rk_object_unref(&surface->base);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    /* If decode is in progress, sync now so the exported DMA-BUF contains the
     * correct frame. Firefox calls ExportSurfaceHandle before SyncSurface when
     * EndPicture is async; without this the EGLImage gets stale data. */
    pthread_mutex_lock(&surface->lock);
    bool needs_sync = !surface->decoded && surface->ctx_id != 0;
    pthread_mutex_unlock(&surface->lock);
    if (needs_sync) {
        VAStatus sync_status = rk_SyncSurface(context, id);
        if (sync_status != VA_STATUS_SUCCESS) {
            rk_object_unref(&surface->base);
            return sync_status;
        }
    }

    pthread_mutex_lock(&surface->lock);
    bool imported_multiplane = surface->imported_multiplane;
    if (imported_multiplane) {
        int y_fd = dup(surface->import_fd);
        int uv_fd = dup(surface->import_chroma_fd);
        size_t y_size = surface->import_size;
        size_t uv_size = surface->import_chroma_size;
        uint32_t y_pitch = surface->import_pitch;
        uint32_t uv_pitch = surface->import_chroma_pitch;
        bool is_10bit = MPP_FRAME_FMT_IS_YUV_10BIT(surface->fmt);
        int width = surface->width;
        int height = surface->height;
        pthread_mutex_unlock(&surface->lock);
        if (y_fd < 0 || uv_fd < 0 || y_size > UINT32_MAX ||
            uv_size > UINT32_MAX) {
            if (y_fd >= 0)
                close(y_fd);
            if (uv_fd >= 0)
                close(uv_fd);
            rk_object_unref(&surface->base);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }

        VADRMPRIMESurfaceDescriptor *desc = descriptor;
        memset(desc, 0, sizeof(*desc));
        desc->fourcc = is_10bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
        desc->width = (uint32_t)width;
        desc->height = (uint32_t)height;
        desc->num_objects = 2;
        desc->objects[0].fd = y_fd;
        desc->objects[0].size = (uint32_t)y_size;
        desc->objects[1].fd = uv_fd;
        desc->objects[1].size = (uint32_t)uv_size;
        bool composed =
            (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0;
        if (composed) {
            desc->num_layers = 1;
            desc->layers[0].drm_format =
                is_10bit ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
            desc->layers[0].num_planes = 2;
            desc->layers[0].object_index[0] = 0;
            desc->layers[0].pitch[0] = y_pitch;
            desc->layers[0].object_index[1] = 1;
            desc->layers[0].pitch[1] = uv_pitch;
        } else {
            desc->num_layers = 2;
            desc->layers[0].drm_format =
                is_10bit ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
            desc->layers[0].num_planes = 1;
            desc->layers[0].object_index[0] = 0;
            desc->layers[0].pitch[0] = y_pitch;
            desc->layers[1].drm_format =
                is_10bit ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
            desc->layers[1].num_planes = 1;
            desc->layers[1].object_index[0] = 1;
            desc->layers[1].pitch[0] = uv_pitch;
        }
        LOG("ExportSurfaceHandle: two-object surface=0x%x %dx%d "
            "pitch=%u/%u 10bit=%d", id, width, height, y_pitch,
            uv_pitch, is_10bit);
        rk_object_unref(&surface->base);
        return VA_STATUS_SUCCESS;
    }
    MppBuffer active_buffer = surface->import_buf ? surface->import_buf
                            : surface->backing_buf ? surface->backing_buf
                            : surface->frame ? mpp_frame_get_buffer(surface->frame)
                            : surface->priv_buf;
    int fd = active_buffer ? mpp_buffer_get_fd(active_buffer) : -1;
    size_t object_size = active_buffer ? mpp_buffer_get_size(active_buffer) : 0;
    int hstride = surface->hstride ? surface->hstride : surface->width;
    int vstride = surface->vstride ? surface->vstride : surface->height;
    int width = surface->width;
    int height = surface->height;
    bool decoded = surface->decoded;
    bool is_placeholder = surface->import_buf == NULL &&
                          surface->frame == NULL &&
                          surface->backing_buf == NULL;
    bool is_10bit = MPP_FRAME_FMT_IS_YUV_10BIT(surface->fmt);
    bool imported_rgb = surface->imported_rgb;
    uint32_t import_pitch = surface->import_pitch;
    uint32_t import_drm_format = surface->import_drm_format;
    uint32_t surface_fourcc = surface->fourcc;

    /* Panfrost rejects a linear NV12 EGL import whose pitch is not 64-byte
     * aligned. MPP's RK3588 H.264 output is only 16-byte aligned for widths
     * such as CIF (352), while HEVC and VP9 already use compatible layouts.
     * Repack only those decoded, driver-owned surfaces and cache the result
     * for the surface fence; all compatible layouts retain zero-copy export. */
    if (decoded && !surface->import_buf && !is_10bit &&
        hstride % DRM_LINEAR_NV12_PITCH_ALIGNMENT != 0) {
        int aligned_hstride =
            (hstride + DRM_LINEAR_NV12_PITCH_ALIGNMENT - 1) &
            ~(DRM_LINEAR_NV12_PITCH_ALIGNMENT - 1);
        size_t aligned_size = 0;
        bool layout_valid = aligned_hstride >= width && vstride >= height &&
            rk_nv12_layout_size((size_t)aligned_hstride, (size_t)vstride,
                                &aligned_size);
        bool allocation_valid = surface->export_buf &&
            surface->export_hstride == aligned_hstride &&
            surface->export_vstride == vstride &&
            mpp_buffer_get_size(surface->export_buf) >= aligned_size;
        if (layout_valid && !allocation_valid) {
            MppBuffer replacement = NULL;
            if (surface->priv_group &&
                mpp_buffer_get(surface->priv_group, &replacement,
                               aligned_size) == MPP_OK) {
                if (surface->export_buf)
                    mpp_buffer_put(surface->export_buf);
                surface->export_buf = replacement;
                surface->export_hstride = aligned_hstride;
                surface->export_vstride = vstride;
                surface->export_fence = 0;
                allocation_valid = true;
            }
        }
        if (!layout_valid || !allocation_valid ||
            (surface->export_fence != surface->fence &&
             !rk_repack_nv12(active_buffer, (uint32_t)width,
                              (uint32_t)height, (uint32_t)hstride,
                              (uint32_t)vstride, surface->export_buf,
                              (uint32_t)aligned_hstride,
                              (uint32_t)vstride))) {
            pthread_mutex_unlock(&surface->lock);
            rk_object_unref(&surface->base);
            return layout_valid ? VA_STATUS_ERROR_OPERATION_FAILED
                                : VA_STATUS_ERROR_INVALID_SURFACE;
        }
        surface->export_fence = surface->fence;
        active_buffer = surface->export_buf;
        fd = mpp_buffer_get_fd(active_buffer);
        hstride = aligned_hstride;
        object_size = mpp_buffer_get_size(active_buffer);
        LOG("ExportSurfaceHandle: repacked surface=0x%x fence=%llu "
            "pitch=%d->%d", id, (unsigned long long)surface->fence,
            surface->hstride, aligned_hstride);
    }
    int export_fd = fd >= 0 ? dup(fd) : -1;
    int duplicate_error = export_fd < 0 ? errno : 0;
    pthread_mutex_unlock(&surface->lock);

    if (fd < 0 || object_size == 0 || object_size > UINT32_MAX) {
        if (export_fd >= 0)
            close(export_fd);
        LOG("ExportSurfaceHandle: buffer not exportable (fd=%d size=%zu decoded=%d)",
            fd, object_size, decoded);
        rk_object_unref(&surface->base);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (export_fd < 0) {
        LOG("ExportSurfaceHandle: dup(%d) failed errno=%d, ERROR_ALLOCATION_FAILED",
            fd, duplicate_error);
        rk_object_unref(&surface->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    LOG("ExportSurfaceHandle: surface=0x%x %dx%d stride=%dx%d export_fd=%d decoded=%d placeholder=%d 10bit=%d",
        id, width, height, hstride, vstride, export_fd, decoded,
        is_placeholder, is_10bit);

    VADRMPRIMESurfaceDescriptor *desc = descriptor;
    memset(desc, 0, sizeof(*desc));
    desc->width = (uint32_t)width;
    desc->height = (uint32_t)height;
    desc->num_objects = 1;
    desc->objects[0].fd = export_fd;
    desc->objects[0].size = (uint32_t)object_size;
    desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;

    if (imported_rgb) {
        desc->fourcc = surface_fourcc;
        desc->num_layers = 1;
        desc->layers[0].drm_format = import_drm_format;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = import_pitch;
        rk_object_unref(&surface->base);
        return VA_STATUS_SUCCESS;
    }

    bool composed = (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0;

    /* COMPOSED_LAYERS: single NV12/P010 layer with 2 planes (mpv, GStreamer).
     * SEPARATE_LAYERS (default): R8/GR88 split planes (Firefox DMABufSurfaceYUV). */
    if (!is_10bit && composed) {
        desc->fourcc = VA_FOURCC_NV12;
        desc->num_layers = 1;
        desc->layers[0].drm_format = DRM_FORMAT_NV12;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = (uint32_t)hstride;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1] = (uint32_t)(hstride * vstride);
        desc->layers[0].pitch[1] = (uint32_t)hstride;
        rk_object_unref(&surface->base);
        return VA_STATUS_SUCCESS;
    }
    if (is_10bit && composed) {
        desc->fourcc = VA_FOURCC_P010;
        desc->num_layers = 1;
        desc->layers[0].drm_format = DRM_FORMAT_P010;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = (uint32_t)(hstride * 2);
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1] = (uint32_t)(hstride * vstride * 2);
        desc->layers[0].pitch[1] = (uint32_t)(hstride * 2);
        rk_object_unref(&surface->base);
        return VA_STATUS_SUCCESS;
    }

    desc->num_layers = 2;

    if (is_10bit) {
        /* P010 as R16 luma and GR1616 interleaved chroma layers. */
        desc->fourcc = VA_FOURCC_P010;
        desc->layers[0].drm_format = DRM_FORMAT_R16;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = (uint32_t)(hstride * 2);
        desc->layers[1].drm_format = DRM_FORMAT_GR1616;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = (uint32_t)(hstride * vstride * 2);
        desc->layers[1].pitch[0] = (uint32_t)(hstride * 2);
    } else {
        /* NV12 as R8 luma and GR88 interleaved chroma layers. */
        desc->fourcc = VA_FOURCC_NV12;
        desc->layers[0].drm_format = DRM_FORMAT_R8;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = (uint32_t)hstride;
        desc->layers[1].drm_format = DRM_FORMAT_GR88;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = (uint32_t)(hstride * vstride);
        desc->layers[1].pitch[0] = (uint32_t)hstride;
    }
    rk_object_unref(&surface->base);
    return VA_STATUS_SUCCESS;
}
