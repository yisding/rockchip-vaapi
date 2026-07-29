#include "surface.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <va/va_drmcommon.h>

#include "convert.h"
#include "driver_internal.h"
#include "frame_layout.h"

static bool dmabuf_cpu_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    int ret;
    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && errno == EINTR);
    return ret == 0;
}

static void surface_destroy(void *opaque) {
    RKSurface *surface = opaque;
    if (surface->frame)
        mpp_frame_deinit(&surface->frame);
    if (surface->backing_buf)
        mpp_buffer_put(surface->backing_buf);
    if (surface->import_buf)
        mpp_buffer_put(surface->import_buf);
    if (surface->import_fd >= 0)
        close(surface->import_fd);
    if (surface->decode_pool)
        rk_object_unref(&surface->decode_pool->base);
    if (surface->priv_buf)
        mpp_buffer_put(surface->priv_buf);
    if (surface->priv_group)
        mpp_buffer_group_put(surface->priv_group);
    pthread_cond_destroy(&surface->cond);
    pthread_mutex_destroy(&surface->lock);
    free(surface);
}

static uint32_t rgb_drm_format(uint32_t fourcc)
{
    switch (fourcc) {
    case VA_FOURCC_RGBA: return DRM_FORMAT_ABGR8888;
    case VA_FOURCC_RGBX: return DRM_FORMAT_XBGR8888;
    case VA_FOURCC_BGRA: return DRM_FORMAT_ARGB8888;
    case VA_FOURCC_BGRX: return DRM_FORMAT_XRGB8888;
    default:             return 0;
    }
}

static bool rgb_fourcc(uint32_t fourcc)
{
    return rgb_drm_format(fourcc) != 0;
}

static VAStatus import_surface_descriptor(
    RKSurface *surface, const VADRMPRIMESurfaceDescriptor *descriptor)
{
    uint32_t expected_drm_format = rgb_drm_format(surface->fourcc);
    bool rgb = expected_drm_format != 0;
    bool p010 = surface->fourcc == VA_FOURCC_P010;
    if (!rgb && surface->fourcc != VA_FOURCC_NV12 && !p010)
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
    if (!rgb)
        expected_drm_format = p010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
    if (rgb && !rk_rga_available())
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
    if (!descriptor || descriptor->fourcc != surface->fourcc ||
        descriptor->width != (uint32_t)surface->width ||
        descriptor->height != (uint32_t)surface->height ||
        descriptor->num_objects != 1 || descriptor->objects[0].fd < 0 ||
        descriptor->objects[0].size == 0 ||
        descriptor->objects[0].drm_format_modifier !=
            DRM_FORMAT_MOD_LINEAR ||
        descriptor->num_layers != 1)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    const uint32_t pitch = descriptor->layers[0].pitch[0];
    size_t required_size = 0;
    if (rgb) {
        required_size = (size_t)pitch * (uint32_t)surface->height;
        if (descriptor->layers[0].drm_format != expected_drm_format ||
            descriptor->layers[0].num_planes != 1 ||
            descriptor->layers[0].object_index[0] != 0 ||
            descriptor->layers[0].offset[0] != 0 || pitch % 4 != 0 ||
            pitch / 4 < (uint32_t)surface->width ||
            (surface->width & 1) || (surface->height & 1))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    } else {
        uint32_t bytes_per_sample = p010 ? 2u : 1u;
        size_t chroma_offset =
            (size_t)pitch * (uint32_t)surface->height;
        required_size =
            chroma_offset + (size_t)pitch *
                                ((uint32_t)surface->height / 2);
        if (descriptor->layers[0].drm_format != expected_drm_format ||
            descriptor->layers[0].num_planes != 2 ||
            descriptor->layers[0].object_index[0] != 0 ||
            descriptor->layers[0].object_index[1] != 0 ||
            descriptor->layers[0].offset[0] != 0 ||
            descriptor->layers[0].offset[1] != chroma_offset ||
            descriptor->layers[0].pitch[1] != pitch ||
            pitch % bytes_per_sample != 0 ||
            pitch / bytes_per_sample < (uint32_t)surface->width)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (required_size > descriptor->objects[0].size)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    int imported_fd = dup(descriptor->objects[0].fd);
    if (imported_fd < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (!dmabuf_cpu_sync(imported_fd, DMA_BUF_SYNC_START |
                                      DMA_BUF_SYNC_READ) ||
        !dmabuf_cpu_sync(imported_fd, DMA_BUF_SYNC_END |
                                      DMA_BUF_SYNC_READ)) {
        close(imported_fd);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    MppBufferInfo info = {
        .type = MPP_BUFFER_TYPE_EXT_DMA,
        .size = descriptor->objects[0].size,
        .fd = imported_fd,
    };
    MppBuffer imported_buffer = NULL;
    if (mpp_buffer_import(&imported_buffer, &info) != MPP_OK ||
        !imported_buffer) {
        close(imported_fd);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    surface->import_buf = imported_buffer;
    surface->import_fd = imported_fd;
    surface->import_size = descriptor->objects[0].size;
    surface->import_pitch = pitch;
    surface->import_drm_format = descriptor->layers[0].drm_format;
    surface->imported_rgb = rgb;
    if (!rgb) {
        surface->hstride = (int)(pitch / (p010 ? 2u : 1u));
        surface->vstride = surface->height;
    }
    LOG("CreateSurfaces: imported %s %dx%d fd=%d size=%zu pitch=%u "
        "drm_format=0x%x",
        rgb ? "RGB" : p010 ? "P010" : "NV12",
        surface->width, surface->height, imported_fd,
        surface->import_size, pitch, surface->import_drm_format);
    return VA_STATUS_SUCCESS;
}

static VAStatus create_surfaces(VADriverContextP ctx, int width, int height,
                                int n, VASurfaceID *ids, uint32_t fourcc,
                                const VADRMPRIMESurfaceDescriptor *imports,
                                bool encoder_input)
{
    RKDriver *d = drv_from_ctx(ctx);
    bool is_10bit = fourcc == VA_FOURCC_P010;

    if (width <= 0 || height <= 0 || n < 0 || (n > 0 && !ids))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width > RK_MAX_WIDTH || height > RK_MAX_HEIGHT)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    if ((fourcc == VA_FOURCC_I420 || fourcc == VA_FOURCC_YV12 ||
         fourcc == VA_FOURCC_P010) &&
        ((width & 1) || (height & 1)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    int allocated = 0;
    VAStatus failure_status = VA_STATUS_ERROR_ALLOCATION_FAILED;
    for (int s = 0; s < n; s++) {
        RKSurface *surf = calloc(1, sizeof(*surf));
        if (!surf)
            goto rollback;
        rk_object_init(&surf->base, surface_destroy);
        surf->import_fd = -1;
        surf->width    = width;
        surf->height   = height;
        surf->fmt      = is_10bit ? MPP_FMT_YUV420SP_10BIT
                                  : MPP_FMT_YUV420SP;
        surf->fourcc   = fourcc;

        if (pthread_mutex_init(&surf->lock, NULL) != 0) {
            free(surf);
            goto rollback;
        }
        if (pthread_cond_init(&surf->cond, NULL) != 0) {
            pthread_mutex_destroy(&surf->lock);
            free(surf);
            goto rollback;
        }

        if (imports) {
            VAStatus import_status =
                import_surface_descriptor(surf, &imports[s]);
            if (import_status != VA_STATUS_SUCCESS) {
                failure_status = import_status;
                rk_object_unref(&surf->base);
                goto rollback;
            }
        }

        /* Pre-allocate placeholder DMA-BUF so ExportSurfaceHandle succeeds
         * before any decode (e.g. Firefox's DMABUF capability probe). */
        {
            size_t alloc_size = 0;
            MppBufferGroup grp = NULL;
            MppBuffer      buf = NULL;
            if (rk_surface_placeholder_size((unsigned)width,
                                            (unsigned)height, is_10bit,
                                            &alloc_size) &&
                mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM) == MPP_OK &&
                mpp_buffer_get(grp, &buf, alloc_size) == MPP_OK) {
                int raw_fd = mpp_buffer_get_fd(buf);
                if (raw_fd >= 0) {
                    surf->priv_group = grp;
                    surf->priv_buf   = buf;
                    if (!imports || surf->imported_rgb) {
                        surf->hstride = (int)((width  + 15) & ~15);
                        surf->vstride = (int)((height + 15) & ~15);
                    }
                    surf->encoder_input = encoder_input;
                    LOG("CreateSurfaces: surface %ux%u placeholder fd=%d "
                        "size=%zu format=%s",
                        (unsigned)width, (unsigned)height, raw_fd,
                        mpp_buffer_get_size(buf), is_10bit ? "P010" : "NV12");
                } else {
                    LOG("CreateSurfaces: mpp_buffer_get_fd failed (raw_fd=%d), no placeholder", raw_fd);
                    mpp_buffer_put(buf);
                    mpp_buffer_group_put(grp);
                }
            } else {
                if (buf) mpp_buffer_put(buf);
                if (grp) mpp_buffer_group_put(grp);
                LOG("CreateSurfaces: placeholder allocation failed");
            }
        }
        if (!surf->priv_buf) {
            rk_object_unref(&surf->base);
            goto rollback;
        }

        uint32_t id;
        pthread_mutex_lock(&d->object_lock);
        bool inserted = rk_object_heap_insert(&d->surface_heap, &surf->base,
                                              &id);
        pthread_mutex_unlock(&d->object_lock);
        if (!inserted) {
            rk_object_unref(&surf->base);
            goto rollback;
        }
        ids[s] = (VASurfaceID)id;
        allocated++;
    }
    return VA_STATUS_SUCCESS;

rollback:
    for (int j = 0; j < allocated; j++) {
        pthread_mutex_lock(&d->object_lock);
        RKSurface *surface = (RKSurface *)rk_object_heap_remove(
            &d->surface_heap, (uint32_t)ids[j]);
        pthread_mutex_unlock(&d->object_lock);
        if (surface)
            rk_object_unref(&surface->base);
    }
    return failure_status;
}

/* vaCreateSurfaces (old API, redirected) */
VAStatus rk_CreateSurfaces(VADriverContextP ctx,
                           int width, int height, int format,
                           int n, VASurfaceID *ids)
{
    if (format != VA_RT_FORMAT_YUV420 &&
        format != VA_RT_FORMAT_YUV420_10)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    return create_surfaces(ctx, width, height, n, ids,
                           format == VA_RT_FORMAT_YUV420_10
                               ? VA_FOURCC_P010 : VA_FOURCC_NV12,
                           NULL, false);
}

VAStatus rk_DestroySurfaces(VADriverContextP ctx,
                                    VASurfaceID *list, int n) {
    LOG("DestroySurfaces: n=%d", n);
    RKDriver *d = drv_from_ctx(ctx);
    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&d->object_lock);
        RKSurface *surface = (RKSurface *)rk_object_heap_remove(
            &d->surface_heap, (uint32_t)list[i]);
        pthread_mutex_unlock(&d->object_lock);
        if (surface)
            rk_object_unref(&surface->base);
    }
    return VA_STATUS_SUCCESS;
}

/* vaCreateSurfaces2 (new API with attributes) */
VAStatus rk_CreateSurfaces2(VADriverContextP ctx,
                            unsigned int format,
                            unsigned int width, unsigned int height,
                            VASurfaceID *ids, unsigned int n,
                            VASurfaceAttrib *attribs,
                            unsigned int n_attribs)
{
    LOG("CreateSurfaces2: %ux%u fmt=0x%x n=%u n_attribs=%u",
        width, height, format, n, n_attribs);
    if ((n_attribs > 0 && !attribs) || width > INT_MAX ||
        height > INT_MAX || n > INT_MAX)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    uint32_t fourcc;
    if (format == VA_RT_FORMAT_YUV420)
        fourcc = VA_FOURCC_NV12;
    else if (format == VA_RT_FORMAT_YUV420_10)
        fourcc = VA_FOURCC_P010;
    else if (format == VA_RT_FORMAT_RGB32)
        fourcc = VA_FOURCC_BGRA;
    else
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    bool pixel_format_seen = false;
    bool memory_type_seen = false;
    bool descriptor_seen = false;
    bool encoder_input = false;
    uint32_t memory_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    const VADRMPRIMESurfaceDescriptor *external_descriptors = NULL;
    for (unsigned i = 0; i < n_attribs; i++) {
        LOG("  attrib[%u] type=%d flags=%d value=0x%x",
            i, attribs[i].type, attribs[i].flags,
            attribs[i].value.type == VAGenericValueTypeInteger
                ? (unsigned)attribs[i].value.value.i : 0u);
        if (attribs[i].type == VASurfaceAttribUsageHint) {
            if (attribs[i].value.type == VAGenericValueTypeInteger &&
                (attribs[i].value.value.i &
                 VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER))
                encoder_input = true;
            continue;
        }
        if (attribs[i].type == VASurfaceAttribMemoryType) {
            if (attribs[i].value.type != VAGenericValueTypeInteger)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            uint32_t requested = (uint32_t)attribs[i].value.value.i;
            if (requested != VA_SURFACE_ATTRIB_MEM_TYPE_VA &&
                requested != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            if (memory_type_seen && requested != memory_type)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            memory_type = requested;
            memory_type_seen = true;
            continue;
        }
        if (attribs[i].type == VASurfaceAttribExternalBufferDescriptor) {
            if (attribs[i].value.type != VAGenericValueTypePointer ||
                !attribs[i].value.value.p)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            const VADRMPRIMESurfaceDescriptor *requested =
                attribs[i].value.value.p;
            if (descriptor_seen && requested != external_descriptors)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            external_descriptors = requested;
            descriptor_seen = true;
            continue;
        }
        if (attribs[i].type != VASurfaceAttribPixelFormat)
            continue;
        if (attribs[i].value.type != VAGenericValueTypeInteger)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        uint32_t requested = (uint32_t)attribs[i].value.value.i;
        if (pixel_format_seen && requested != fourcc)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (requested != VA_FOURCC_NV12 && requested != VA_FOURCC_P010 &&
            requested != VA_FOURCC_I420 && requested != VA_FOURCC_YV12 &&
            !rgb_fourcc(requested))
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        if ((format == VA_RT_FORMAT_YUV420_10 &&
             requested != VA_FOURCC_P010) ||
            (format == VA_RT_FORMAT_YUV420 &&
             requested != VA_FOURCC_NV12 &&
             requested != VA_FOURCC_I420 &&
             requested != VA_FOURCC_YV12) ||
            (format == VA_RT_FORMAT_RGB32 && !rgb_fourcc(requested)))
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        fourcc = requested;
        pixel_format_seen = true;
    }

    bool imported = memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    if (imported != descriptor_seen)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (rgb_fourcc(fourcc) && !imported)
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    if (imported && fourcc != VA_FOURCC_NV12 &&
        fourcc != VA_FOURCC_P010 && !rgb_fourcc(fourcc))
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;

    return create_surfaces(ctx, (int)width, (int)height, (int)n, ids,
                           fourcc, imported ? external_descriptors : NULL,
                           encoder_input);
}

static VAStatus sync_surface_timeout(VADriverContextP ctx, VASurfaceID id,
                                     uint64_t timeout_ns) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_acquire(d, id);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;

    struct timespec deadline = {0};
    bool timed = timeout_ns != VA_TIMEOUT_INFINITE;
    if (timed && timeout_ns != 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        uint64_t seconds = timeout_ns / 1000000000u;
        uint64_t nanoseconds = timeout_ns % 1000000000u;
        deadline.tv_sec += (time_t)seconds;
        deadline.tv_nsec += (long)nanoseconds;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&s->lock);
    for (;;) {
        if (s->decode_failed) {
            pthread_mutex_unlock(&s->lock);
            LOG("SyncSurface: decode failed surface=0x%x", id);
            rk_object_unref(&s->base);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        if (s->decoded || s->ctx_id == 0) {
            pthread_mutex_unlock(&s->lock);
            LOG("SyncSurface: surface=0x%x ready", id);
            rk_object_unref(&s->base);
            return VA_STATUS_SUCCESS;
        }
        if (timeout_ns == 0) {
            pthread_mutex_unlock(&s->lock);
            rk_object_unref(&s->base);
            return VA_STATUS_ERROR_TIMEDOUT;
        }

        int wait_status = timed
                        ? pthread_cond_timedwait(&s->cond, &s->lock,
                                                 &deadline)
                        : pthread_cond_wait(&s->cond, &s->lock);
        if (wait_status == ETIMEDOUT) {
            pthread_mutex_unlock(&s->lock);
            LOG("SyncSurface: timeout surface=0x%x", id);
            rk_object_unref(&s->base);
            return VA_STATUS_ERROR_TIMEDOUT;
        }
        if (wait_status != 0) {
            pthread_mutex_unlock(&s->lock);
            rk_object_unref(&s->base);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
}

VAStatus rk_SyncSurface(VADriverContextP ctx, VASurfaceID id) {
    return sync_surface_timeout(ctx, id, VA_TIMEOUT_INFINITE);
}

VAStatus rk_SyncSurface2(VADriverContextP ctx,
                         VASurfaceID id, uint64_t timeout_ns) {
    return sync_surface_timeout(ctx, id, timeout_ns);
}

VAStatus rk_QuerySurfaceStatus(VADriverContextP ctx,
                                       VASurfaceID id,
                                       VASurfaceStatus *status) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_acquire(d, id);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
    pthread_mutex_lock(&s->lock);
    *status = (s->decoded || s->ctx_id == 0)
            ? VASurfaceReady : VASurfaceRendering;
    pthread_mutex_unlock(&s->lock);
    LOG("QuerySurfaceStatus: surface=0x%x status=%s", id,
        (*status == VASurfaceReady) ? "Ready" : "Rendering");
    rk_object_unref(&s->base);
    return VA_STATUS_SUCCESS;
}

static bool image_plane_fits(const RKImage *image, unsigned int plane,
                             size_t row_bytes, unsigned int rows,
                             size_t capacity)
{
    if (plane >= image->num_planes || image->pitches[plane] < row_bytes ||
        image->offsets[plane] > capacity)
        return false;
    if (!rows)
        return true;
    size_t last_row = (size_t)(rows - 1) * image->pitches[plane];
    return last_row <= capacity - image->offsets[plane] &&
           row_bytes <= capacity - image->offsets[plane] - last_row;
}

VAStatus rk_GetImage(VADriverContextP ctx, VASurfaceID surface_id,
                             int x, int y, unsigned int w,
                             unsigned int h, VAImageID image_id)
{
    RKDriver  *d  = drv_from_ctx(ctx);
    RKSurface *s = surface_acquire(d, surface_id);
    RKImage *image = image_acquire(d, image_id);
    RKBuffer *ib = image ? image->buffer : NULL;
    if (!s) {
        if (image)
            rk_object_unref(&image->base);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!image || !ib || !ib->data) {
        rk_object_unref(&s->base);
        if (image)
            rk_object_unref(&image->base);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    pthread_mutex_lock(&s->lock);
    MppBuffer source_buffer = s->import_buf ? s->import_buf
                            : s->backing_buf ? s->backing_buf
                            : s->frame ? mpp_frame_get_buffer(s->frame)
                            : s->priv_buf;
    bool full_frame = x == 0 && y == 0 && w == (unsigned int)s->width &&
                      h == (unsigned int)s->height &&
                      image->width >= w && image->height >= h;
    if (!source_buffer || !full_frame) {
        pthread_mutex_unlock(&s->lock);
        rk_object_unref(&image->base);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int hs  = s->hstride ? s->hstride : s->width;
    int vs  = s->vstride ? s->vstride : s->height;
    bool i10 = MPP_FRAME_FMT_IS_YUV_10BIT(s->fmt);
    int bpp  = i10 ? 2 : 1;
    bool planar = image->fourcc == VA_FOURCC_I420 ||
                  image->fourcc == VA_FOURCC_YV12;
    size_t source_size;
    size_t source_buffer_size = mpp_buffer_get_size(source_buffer);
    size_t source_pitch = (size_t)hs * (size_t)bpp;
    size_t row_bytes = (size_t)(unsigned int)s->width * (size_t)bpp;
    size_t chroma_bytes = (size_t)(unsigned int)s->width / 2;
    bool destination_layout_valid =
        image_plane_fits(image, 0, planar ? (size_t)(unsigned int)s->width
                                         : row_bytes,
                         (unsigned int)s->height, ib->capacity) &&
        image_plane_fits(image, 1, planar ? chroma_bytes : row_bytes,
                         (unsigned int)s->height / 2, ib->capacity) &&
        (!planar ||
         image_plane_fits(image, 2, chroma_bytes,
                          (unsigned int)s->height / 2, ib->capacity));
    if (s->imported_rgb || image->fourcc != s->fourcc || (planar && i10) ||
        source_pitch < row_bytes ||
        !rk_nv12_layout_size(source_pitch, (size_t)vs, &source_size) ||
        source_size > source_buffer_size || !destination_layout_valid) {
        pthread_mutex_unlock(&s->lock);
        rk_object_unref(&image->base);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    int source_fd = mpp_buffer_get_fd(source_buffer);
    if (source_fd < 0 ||
        !dmabuf_cpu_sync(source_fd, DMA_BUF_SYNC_START |
                                    DMA_BUF_SYNC_READ)) {
        pthread_mutex_unlock(&s->lock);
        rk_object_unref(&image->base);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const uint8_t *sp = (const uint8_t *)mpp_buffer_get_ptr(source_buffer);
    if (!sp) {
        (void)dmabuf_cpu_sync(source_fd, DMA_BUF_SYNC_END |
                                        DMA_BUF_SYNC_READ);
        pthread_mutex_unlock(&s->lock);
        rk_object_unref(&image->base);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    uint8_t       *dp = (uint8_t *)ib->data;
    memset(dp, 0, ib->capacity);
    for (int r = 0; r < s->height; r++)
        memcpy(dp + image->offsets[0] +
                   (size_t)r * image->pitches[0],
               sp + (size_t)r * source_pitch,
               planar ? (size_t)(unsigned int)s->width : row_bytes);
    const uint8_t *su = sp + source_pitch * (size_t)vs;
    if (planar) {
        unsigned int u_plane = image->fourcc == VA_FOURCC_I420 ? 1 : 2;
        unsigned int v_plane = image->fourcc == VA_FOURCC_I420 ? 2 : 1;
        for (int r = 0; r < s->height / 2; r++) {
            uint8_t *du = dp + image->offsets[u_plane] +
                          (size_t)r * image->pitches[u_plane];
            uint8_t *dv = dp + image->offsets[v_plane] +
                          (size_t)r * image->pitches[v_plane];
            const uint8_t *row_uv = su + (size_t)r * source_pitch;
            for (size_t column = 0; column < chroma_bytes; column++) {
                du[column] = row_uv[2 * column];
                dv[column] = row_uv[2 * column + 1];
            }
        }
    } else {
        uint8_t *du = dp + image->offsets[1];
        for (int r = 0; r < s->height / 2; r++)
            memcpy(du + (size_t)r * image->pitches[1],
                   su + (size_t)r * source_pitch, row_bytes);
    }
    bool sync_ok = dmabuf_cpu_sync(source_fd, DMA_BUF_SYNC_END |
                                              DMA_BUF_SYNC_READ);
    pthread_mutex_unlock(&s->lock);
    rk_object_unref(&image->base);
    rk_object_unref(&s->base);
    return sync_ok ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
}
