#include "export.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <va/va_drmcommon.h>

#include "driver_internal.h"
#include "surface.h"

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
    MppBuffer active_buffer = surface->frame
                            ? mpp_frame_get_buffer(surface->frame)
                            : surface->priv_buf;
    int fd = active_buffer ? mpp_buffer_get_fd(active_buffer) : -1;
    size_t object_size = active_buffer ? mpp_buffer_get_size(active_buffer) : 0;
    int hstride = surface->hstride ? surface->hstride : surface->width;
    int vstride = surface->vstride ? surface->vstride : surface->height;
    int width = surface->width;
    int height = surface->height;
    bool decoded = surface->decoded;
    bool is_placeholder = surface->frame == NULL;
    bool is_10bit = MPP_FRAME_FMT_IS_YUV_10BIT(surface->fmt);
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
    desc->objects[0].drm_format_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */

    bool composed = (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0;

    /* COMPOSED_LAYERS: single NV12/P010 layer with 2 planes (mpv, GStreamer).
     * SEPARATE_LAYERS (default): R8/GR88 split planes (Firefox DMABufSurfaceYUV). */
    if (!is_10bit && composed) {
        desc->fourcc = VA_FOURCC_NV12;
        desc->num_layers = 1;
        desc->layers[0].drm_format = 0x3231564e; /* DRM_FORMAT_NV12 */
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
        desc->layers[0].drm_format = 0x30313050; /* DRM_FORMAT_P010 */
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
        desc->layers[0].drm_format = 0x20363152; /* DRM_FORMAT_R16 */
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = (uint32_t)(hstride * 2);
        desc->layers[1].drm_format = 0x36315247; /* DRM_FORMAT_GR1616 */
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = (uint32_t)(hstride * vstride * 2);
        desc->layers[1].pitch[0] = (uint32_t)(hstride * 2);
    } else {
        /* NV12 as R8 luma and GR88 interleaved chroma layers. */
        desc->fourcc = VA_FOURCC_NV12;
        desc->layers[0].drm_format = 0x20203852; /* DRM_FORMAT_R8 */
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = 0;
        desc->layers[0].pitch[0] = (uint32_t)hstride;
        desc->layers[1].drm_format = 0x38385247; /* DRM_FORMAT_GR88 */
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = 0;
        desc->layers[1].offset[0] = (uint32_t)(hstride * vstride);
        desc->layers[1].pitch[0] = (uint32_t)hstride;
    }
    rk_object_unref(&surface->base);
    return VA_STATUS_SUCCESS;
}
