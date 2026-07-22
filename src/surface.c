#include "surface.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>

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

/* vaCreateSurfaces (old API, redirected) */
VAStatus rk_CreateSurfaces(VADriverContextP ctx,
                                   int width, int height, int format,
                                   int n, VASurfaceID *ids) {
    RKDriver *d = drv_from_ctx(ctx);
    (void)format;

    if (width <= 0 || height <= 0 || n < 0 || (n > 0 && !ids))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width > 7680 || height > 4320)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    int allocated = 0;
    for (int s = 0; s < n; s++) {
        RKSurface *surf = calloc(1, sizeof(*surf));
        if (!surf)
            goto rollback;
        rk_object_init(&surf->base, surface_destroy);
        surf->width    = width;
        surf->height   = height;

        if (pthread_mutex_init(&surf->lock, NULL) != 0) {
            free(surf);
            goto rollback;
        }
        if (pthread_cond_init(&surf->cond, NULL) != 0) {
            pthread_mutex_destroy(&surf->lock);
            free(surf);
            goto rollback;
        }

        /* Pre-allocate placeholder DMA-BUF so ExportSurfaceHandle succeeds
         * before any decode (e.g. Firefox's DMABUF capability probe). */
        {
            size_t alloc_size = 0;
            MppBufferGroup grp = NULL;
            MppBuffer      buf = NULL;
            if (rk_surface_buffer_size((unsigned)width, (unsigned)height,
                                       &alloc_size) &&
                mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM) == MPP_OK &&
                mpp_buffer_get(grp, &buf, alloc_size) == MPP_OK) {
                int raw_fd = mpp_buffer_get_fd(buf);
                if (raw_fd >= 0) {
                    surf->priv_group = grp;
                    surf->priv_buf   = buf;
                    surf->hstride    = (int)((width  + 15) & ~15);
                    surf->vstride    = (int)((height + 15) & ~15);
                    LOG("CreateSurfaces: surface %ux%u placeholder fd=%d size=%zu",
                        (unsigned)width, (unsigned)height, raw_fd,
                        mpp_buffer_get_size(buf));
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
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
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
                                    unsigned int n_attribs) {
    LOG("CreateSurfaces2: %ux%u fmt=0x%x n=%u n_attribs=%u",
        width, height, format, n, n_attribs);
    for (unsigned i = 0; i < n_attribs; i++) {
        LOG("  attrib[%u] type=%d flags=%d value=0x%x",
            i, attribs[i].type, attribs[i].flags,
            attribs[i].value.type == VAGenericValueTypeInteger
                ? (unsigned)attribs[i].value.value.i : 0u);
    }
    return rk_CreateSurfaces(ctx, (int)width, (int)height, (int)format,
                              (int)n, ids);
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
    *status = s->decoded ? VASurfaceReady : VASurfaceRendering;
    pthread_mutex_unlock(&s->lock);
    LOG("QuerySurfaceStatus: surface=0x%x status=%s", id,
        (*status == VASurfaceReady) ? "Ready" : "Rendering");
    rk_object_unref(&s->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_GetImage(VADriverContextP ctx, VASurfaceID surface_id,
                             int x, int y, unsigned int w,
                             unsigned int h, VAImageID image_id)
{
    (void)x; (void)y; (void)w; (void)h;
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
    MppBuffer source_buffer = s->backing_buf ? s->backing_buf
                            : s->frame ? mpp_frame_get_buffer(s->frame)
                            : s->priv_buf;
    if (!source_buffer || image->width < (unsigned int)s->width ||
        image->height < (unsigned int)s->height) {
        pthread_mutex_unlock(&s->lock);
        rk_object_unref(&image->base);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int hs  = s->hstride ? s->hstride : s->width;
    int vs  = s->vstride ? s->vstride : s->height;
    bool i10 = MPP_FRAME_FMT_IS_YUV_10BIT(s->fmt);
    int bpp  = i10 ? 2 : 1;
    uint32_t expected_fourcc = i10 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    size_t source_size;
    size_t destination_size;
    size_t source_buffer_size = mpp_buffer_get_size(source_buffer);
    size_t source_pitch = (size_t)hs * (size_t)bpp;
    size_t copy_bytes = (((size_t)(unsigned int)s->width + 15u) & ~15u) *
                        (size_t)bpp;
    if (image->fourcc != expected_fourcc || source_pitch < copy_bytes ||
        !rk_nv12_layout_size(source_pitch, (size_t)vs, &source_size) ||
        !rk_nv12_layout_size(image->pitch, image->height,
                             &destination_size) ||
        source_size > source_buffer_size || destination_size > ib->capacity) {
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
    /* Y plane */
    for (int r = 0; r < s->height; r++)
        memcpy(dp + (size_t)r * image->pitch,
               sp + (size_t)r * source_pitch, copy_bytes);
    /* UV plane: src at source_pitch*vs, dst at pitch*image height. */
    const uint8_t *su = sp + source_pitch * (size_t)vs;
    uint8_t *du = dp + (size_t)image->pitch * image->height;
    for (int r = 0; r < s->height / 2; r++)
        memcpy(du + (size_t)r * image->pitch,
               su + (size_t)r * source_pitch, copy_bytes);
    bool sync_ok = dmabuf_cpu_sync(source_fd, DMA_BUF_SYNC_END |
                                              DMA_BUF_SYNC_READ);
    pthread_mutex_unlock(&s->lock);
    rk_object_unref(&image->base);
    rk_object_unref(&s->base);
    return sync_ok ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
}

