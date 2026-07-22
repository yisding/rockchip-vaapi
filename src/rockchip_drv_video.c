/*
 * rockchip_drv_video.c — VA-API driver for Rockchip RK3588 via MPP
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "frame_layout.h"
#include "h264.h"
#include "object_heap.h"
#include "vp9.h"

/* ── logging ─────────────────────────────────────────────────── */
static FILE *g_log_fp = NULL;
static void log_init(void) {
    const char *p = getenv("RK_VAAPI_LOG");
    if (p && *p) g_log_fp = fopen(p, "a");
}
#define LOG(fmt, ...) do { if (g_log_fp) \
    fprintf(g_log_fp, "[rk-vaapi pid=%d] " fmt "\n", getpid(), ##__VA_ARGS__); \
} while(0)

/* ── data structures ─────────────────────────────────────────── */

typedef struct {
    RKObjectBase  base;
    VAProfile     profile;
    VAEntrypoint  entrypoint;
} RKConfig;

typedef struct RKSurface RKSurface;
typedef struct RKDriver RKDriver;

typedef struct RKDecodeJob {
    struct RKDecodeJob *next;
    uint8_t            *data;
    size_t              size;
    VASurfaceID         target;
    RKSurface          *surface;
    uint64_t            fence;
    uint64_t            token;
    bool                h264_field;
    bool                is_hidden;
    uint8_t             repeat_slot;
} RKDecodeJob;

typedef struct RKFrameRoute {
    struct RKFrameRoute *next;
    VASurfaceID          target;
    RKSurface           *surface;
    uint64_t             fence;
    uint64_t             token;
} RKFrameRoute;

typedef struct {
    RKObjectBase base;
    MppBufferGroup frame_group;
    MppBufferGroup backing_group;
    MppBuffer *buffers;
    int count;
} RKDecodePool;

typedef struct {
    RKObjectBase base;
    RKDriver     *driver;
    VAProfile    profile;
    int          width, height;

    MppCtx       mpp;
    MppApi      *mpi;
    MppCodingType coding;

    /* VA render-target hints remain alive for the context lifetime. MPP's
     * pure-external decode pool is context-owned and works without hints. */
    RKSurface    **targets;
    int          n_targets;
    RKDecodePool *decode_pool;

    /* Picture assembly is caller-side; an owned packet is handed to the
     * worker, which is the only thread that calls the MPP data/control API. */
    pthread_mutex_t picture_lock;
    pthread_mutex_t work_lock;
    pthread_cond_t  work_cond;
    pthread_t       worker;
    bool            sync_initialized;
    bool            worker_started;
    bool            worker_stop;
    RKDecodeJob    *job_head;
    RKDecodeJob    *job_tail;
    RKFrameRoute   *h264_routes;
    RKFrameRoute   *generic_head;
    RKFrameRoute   *generic_tail;
    unsigned int    outstanding_frames;
    uint64_t        next_token;

    /* buffers collected between BeginPicture / EndPicture */
    VABufferID   pending[64];
    int          n_pending;

    VASurfaceID  render_target;
    RKSurface   *render_surface;
    uint64_t     render_fence;

    /* H.264 state for SPS/PPS reconstruction */
    VAPictureParameterBufferH264 last_pp;
    VAIQMatrixBufferH264 last_iq;
    bool         has_iq;
    bool         sps_sent;
} RKContext;

struct RKSurface {
    RKObjectBase base;
    int          width, height;

    /* filled after decode */
    MppFrame     frame;          /* owns the zero-copy MPP output reference */
    MppBuffer    backing_buf;    /* keeps the borrowed external fd alive */
    RKDecodePool *decode_pool;   /* keeps both MPP groups alive */
    int          hstride;
    int          vstride;

    /* Pre-decode DMA-BUF used by ExportSurfaceHandle capability probes. Once
     * decoded, export selects frame/backing_buf from the context pool. */
    MppBufferGroup priv_group;
    MppBuffer      priv_buf;

    MppFrameFormat fmt;     /* pixel format of last decoded frame (0 = NV12 default) */
    bool         decoded;
    bool         decode_failed;
    bool         h264_field_pending;
    VAContextID  ctx_id;   /* context currently decoding into this surface */
    uint64_t     fence;
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
};

typedef struct {
    RKObjectBase   base;
    VABufferType   type;
    unsigned int   size;
    unsigned int   num_elements;
    size_t         capacity;
    void          *data;
} RKBuffer;

typedef struct {
    RKObjectBase base;
    VABufferID buffer_id;
    RKBuffer *buffer;
    uint32_t fourcc;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
} RKImage;

struct RKDriver {
    pthread_mutex_t object_lock;
    RKObjectHeap config_heap;
    RKObjectHeap context_heap;
    RKObjectHeap surface_heap;
    RKObjectHeap buffer_heap;
    RKObjectHeap image_heap;
};

/* ── helpers ─────────────────────────────────────────────────── */

static RKDriver *drv_from_ctx(VADriverContextP ctx) {
    return (RKDriver *)ctx->pDriverData;
}

static RKConfig *config_acquire(RKDriver *d, VAConfigID id) {
    pthread_mutex_lock(&d->object_lock);
    RKConfig *config = (RKConfig *)rk_object_heap_acquire(
        &d->config_heap, (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    return config;
}

static RKContext *context_acquire(RKDriver *d, VAContextID id) {
    pthread_mutex_lock(&d->object_lock);
    RKContext *context = (RKContext *)rk_object_heap_acquire(
        &d->context_heap, (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    return context;
}

static RKSurface *surface_acquire(RKDriver *d, VASurfaceID id) {
    pthread_mutex_lock(&d->object_lock);
    RKSurface *surface = (RKSurface *)rk_object_heap_acquire(
        &d->surface_heap, (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    return surface;
}

static RKBuffer *buffer_acquire(RKDriver *d, VABufferID id) {
    pthread_mutex_lock(&d->object_lock);
    RKBuffer *buffer = (RKBuffer *)rk_object_heap_acquire(
        &d->buffer_heap, (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    return buffer;
}

static RKImage *image_acquire(RKDriver *d, VAImageID id) {
    pthread_mutex_lock(&d->object_lock);
    RKImage *image = (RKImage *)rk_object_heap_acquire(
        &d->image_heap, (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    return image;
}

static void stop_decode_worker(RKContext *context);
static void *decode_worker_main(void *opaque);
static void fail_pending_surface_ref(RKSurface *surface, uint64_t fence);

static bool dmabuf_cpu_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    int ret;
    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && errno == EINTR);
    return ret == 0;
}

static void buffer_destroy(void *opaque) {
    RKBuffer *buffer = opaque;
    free(buffer->data);
    free(buffer);
}

static void image_destroy(void *opaque) {
    RKImage *image = opaque;
    if (image->buffer)
        rk_object_unref(&image->buffer->base);
    free(image);
}

static void decode_pool_destroy(void *opaque) {
    RKDecodePool *pool = opaque;
    int count = pool->count;
    if (pool->frame_group)
        mpp_buffer_group_put(pool->frame_group);
    for (int i = 0; i < pool->count; i++) {
        if (pool->buffers[i])
            mpp_buffer_put(pool->buffers[i]);
    }
    free(pool->buffers);
    if (pool->backing_group)
        mpp_buffer_group_put(pool->backing_group);
    LOG("external_group: destroyed buffers=%d", count);
    free(pool);
}

static void context_destroy(void *opaque) {
    RKContext *context = opaque;
    stop_decode_worker(context);
    if (context->mpp)
        mpp_destroy(context->mpp);
    if (context->decode_pool)
        rk_object_unref(&context->decode_pool->base);
    for (int i = 0; i < context->n_targets; i++) {
        if (context->targets[i])
            rk_object_unref(&context->targets[i]->base);
    }
    free(context->targets);
    if (context->sync_initialized) {
        pthread_cond_destroy(&context->work_cond);
        pthread_mutex_destroy(&context->work_lock);
        pthread_mutex_destroy(&context->picture_lock);
    }
    free(context);
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

static VAStatus packet_append(uint8_t **packet, size_t *packet_size,
                              size_t *packet_capacity, const void *data,
                              size_t data_size)
{
    if (data_size == 0)
        return VA_STATUS_SUCCESS;
    if (!data || *packet_size > SIZE_MAX - data_size)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    size_t needed = *packet_size + data_size;
    if (needed > *packet_capacity) {
        size_t new_capacity = needed;
        if (needed <= (SIZE_MAX - 4096) / 2)
            new_capacity = needed * 2 + 4096;

        uint8_t *grown = realloc(*packet, new_capacity);
        if (!grown)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        *packet = grown;
        *packet_capacity = new_capacity;
    }

    memcpy(*packet + *packet_size, data, data_size);
    *packet_size = needed;
    return VA_STATUS_SUCCESS;
}

static MppCodingType profile_to_coding(VAProfile p) {
    switch (p) {
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264High10:    return MPP_VIDEO_CodingAVC;
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:    return MPP_VIDEO_CodingHEVC;
    case VAProfileVP8Version0_3: return MPP_VIDEO_CodingVP8;
    case VAProfileVP9Profile0:
    case VAProfileVP9Profile2:   return MPP_VIDEO_CodingVP9;
    case VAProfileAV1Profile0:
    case VAProfileAV1Profile1:   return MPP_VIDEO_CodingAV1;
    default:                     return MPP_VIDEO_CodingUnused;
    }
}

static int profile_idc(VAProfile p) {
    switch (p) {
    case VAProfileH264ConstrainedBaseline: return 66;
    case VAProfileH264Main:                return 77;
    case VAProfileH264High:                return 100;
    case VAProfileH264High10:              return 110;
    default:                               return 100;
    }
}

/* ── VADriverVTable implementations ──────────────────────────── */

static VAStatus rk_Terminate(VADriverContextP ctx) {
    LOG("Terminate: cleaning up driver");
    RKDriver *d = drv_from_ctx(ctx);
    if (!d) return VA_STATUS_SUCCESS;

    /* destroy any leftover objects */
    rk_object_heap_finish(&d->context_heap);
    rk_object_heap_finish(&d->surface_heap);
    rk_object_heap_finish(&d->image_heap);
    rk_object_heap_finish(&d->buffer_heap);
    rk_object_heap_finish(&d->config_heap);
    pthread_mutex_destroy(&d->object_lock);
    free(d);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

/* Only profiles with validated decode paths are advertised (or accepted by
 * CreateConfig). Board-validated 2026-07-21 on RK3588 / kernel 6.18 via
 * software-vs-VAAPI framemd5 comparison:
 *   - H.264 Main/High: bit-exact on pinned conformance vectors
 *   - VP9 Profile 0: bit-exact
 * Deliberately not offered:
 *   - H.264 Constrained Baseline: MPP decodes the pinned SVA_Base_B stream
 *     incorrectly even though the reconstructed Annex B stream is
 *     software-exact. Fall back instead of returning corrupt frames.
 *   - HEVC: no VPS/SPS/PPS reconstruction exists; MPP receives headerless
 *     slice data and decodes nothing (verified failure). Advertising it made
 *     apps pick VAAPI and break instead of falling back to software.
 *   - VP8: verified segfault in the generic path.
 *   - H.264 High10 / VP9 Profile 2 (10-bit): MPP outputs compact NV15, but
 *     the export/readback path treats 10-bit as 2-byte-per-sample P010 — layout
 *     mismatch, would render garbage.
 *   - AV1: MPP needs a full OBU bytestream but VA-API hands us only
 *     headerless tile data, so MPP can never parse it. Firefox falls back
 *     to VP9 (hardware-decoded) for AV1-capable content. */
static bool profile_supported(VAProfile p) {
    switch (p) {
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileVP9Profile0:
        return true;
    default:
        return false;
    }
}

static VAStatus rk_QueryConfigProfiles(VADriverContextP ctx,
                                       VAProfile *list, int *n) {
    (void)ctx;
    int i = 0;
    list[i++] = VAProfileH264Main;
    list[i++] = VAProfileH264High;
    list[i++] = VAProfileVP9Profile0;
    *n = i;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QueryConfigEntrypoints(VADriverContextP ctx,
                                          VAProfile profile,
                                          VAEntrypoint *list, int *n) {
    (void)ctx;
    if (!profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    list[0] = VAEntrypointVLD;
    *n = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_GetConfigAttributes(VADriverContextP ctx,
                                       VAProfile profile,
                                       VAEntrypoint entrypoint,
                                       VAConfigAttrib *list, int n) {
    (void)ctx; (void)profile; (void)entrypoint;
    for (int i = 0; i < n; i++) {
        LOG("GetConfigAttributes: type=%d", list[i].type);
        switch (list[i].type) {
        case VAConfigAttribRTFormat:
            /* 10-bit intentionally not offered — see profile_supported() */
            list[i].value = VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribDecSliceMode:
            list[i].value = VA_DEC_SLICE_MODE_NORMAL;
            break;
        case VAConfigAttribEncryption:
            list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        default:
            LOG("GetConfigAttributes: unsupported type=%d", list[i].type);
            list[i].value = VA_ATTRIB_NOT_SUPPORTED;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateConfig(VADriverContextP ctx,
                                 VAProfile profile, VAEntrypoint entrypoint,
                                 VAConfigAttrib *attribs, int n_attribs,
                                 VAConfigID *out_id) {
    RKDriver *d = drv_from_ctx(ctx);
    LOG("CreateConfig: profile=%d entrypoint=%d n_attribs=%d",
        profile, entrypoint, n_attribs);

    if (!profile_supported(profile) ||
        profile_to_coding(profile) == MPP_VIDEO_CodingUnused) {
        LOG("CreateConfig: unsupported profile %d", profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    if (entrypoint != VAEntrypointVLD) {
        LOG("CreateConfig: unsupported entrypoint %d", entrypoint);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    (void)attribs; (void)n_attribs;

    RKConfig *config = calloc(1, sizeof(*config));
    if (!config)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    rk_object_init(&config->base, free);
    config->profile = profile;
    config->entrypoint = entrypoint;

    uint32_t id;
    pthread_mutex_lock(&d->object_lock);
    bool inserted = rk_object_heap_insert(&d->config_heap, &config->base,
                                          &id);
    pthread_mutex_unlock(&d->object_lock);
    if (!inserted) {
        rk_object_unref(&config->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    *out_id = (VAConfigID)id;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DestroyConfig(VADriverContextP ctx, VAConfigID id) {
    RKDriver *d = drv_from_ctx(ctx);
    pthread_mutex_lock(&d->object_lock);
    RKConfig *c = (RKConfig *)rk_object_heap_remove(&d->config_heap,
                                                    (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    rk_object_unref(&c->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QueryConfigAttributes(VADriverContextP ctx,
                                          VAConfigID id,
                                          VAProfile *profile,
                                          VAEntrypoint *entrypoint,
                                          VAConfigAttrib *attribs, int *n) {
    RKDriver *d = drv_from_ctx(ctx);
    RKConfig *c = config_acquire(d, id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    (void)attribs;
    *profile = c->profile;
    *entrypoint = c->entrypoint;
    *n = 0;
    rk_object_unref(&c->base);
    return VA_STATUS_SUCCESS;
}

/* vaCreateSurfaces (old API, redirected) */
static VAStatus rk_CreateSurfaces(VADriverContextP ctx,
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

static VAStatus rk_DestroySurfaces(VADriverContextP ctx,
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
static VAStatus rk_CreateSurfaces2(VADriverContextP ctx,
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

static VAStatus rk_CreateContext(VADriverContextP ctx,
                                  VAConfigID config_id,
                                  int width, int height,
                                  int flag,
                                  VASurfaceID *targets, int n_targets,
                                  VAContextID *out_id) {
    RKDriver *d = drv_from_ctx(ctx);
    (void)flag;

    if (width <= 0 || height <= 0 || n_targets < 0 ||
        (n_targets > 0 && !targets) || !out_id)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    RKConfig *cfg = config_acquire(d, config_id);
    if (!cfg) return VA_STATUS_ERROR_INVALID_CONFIG;

    MppCodingType coding = profile_to_coding(cfg->profile);
    VAProfile profile = cfg->profile;
    rk_object_unref(&cfg->base);
    if (coding == MPP_VIDEO_CodingUnused)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    RKContext *c = calloc(1, sizeof(*c));
    if (!c)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    rk_object_init(&c->base, context_destroy);
    c->driver = d;

    if (pthread_mutex_init(&c->picture_lock, NULL) != 0) {
        free(c);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (pthread_mutex_init(&c->work_lock, NULL) != 0) {
        pthread_mutex_destroy(&c->picture_lock);
        free(c);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (pthread_cond_init(&c->work_cond, NULL) != 0) {
        pthread_mutex_destroy(&c->work_lock);
        pthread_mutex_destroy(&c->picture_lock);
        free(c);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    c->sync_initialized = true;

    LOG("CreateContext: config=0x%x %dx%d coding=%d targets=%d",
        config_id, width, height, (int)coding, n_targets);

    if (n_targets > 0) {
        // NOLINTNEXTLINE(bugprone-sizeof-expression) -- pointer array.
        c->targets = calloc((size_t)n_targets, sizeof(*c->targets));
        if (!c->targets) {
            rk_object_unref(&c->base);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        c->n_targets = n_targets;
        for (int i = 0; i < n_targets; i++) {
            c->targets[i] = surface_acquire(d, targets[i]);
            if (!c->targets[i]) {
                rk_object_unref(&c->base);
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }
        }
    }

    MPP_RET ret = mpp_create(&c->mpp, &c->mpi);
    if (ret != MPP_OK) {
        LOG("mpp_create FAILED: %d", ret);
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    LOG("CreateContext: mpp_create OK");

    ret = mpp_init(c->mpp, MPP_CTX_DEC, coding);
    if (ret != MPP_OK) {
        LOG("mpp_init FAILED: %d (coding=%d)", ret, (int)coding);
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    LOG("CreateContext: mpp_init OK");

    /* Must be set after mpp_init: split_parse=0 means we send complete
     * access units. The worker uses a bounded blocking output wait so it can
     * drain efficiently while still observing shutdown and newly queued work. */
    MppDecCfg dec_cfg = NULL;
    if (mpp_dec_cfg_init(&dec_cfg) != MPP_OK ||
        mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", 0) != MPP_OK ||
        c->mpi->control(c->mpp, MPP_DEC_SET_CFG, dec_cfg) != MPP_OK) {
        if (dec_cfg)
            mpp_dec_cfg_deinit(dec_cfg);
        LOG("CreateContext: decoder configuration failed");
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    mpp_dec_cfg_deinit(dec_cfg);

    RK_S64 output_timeout_ms = 20;
    if (c->mpi->control(c->mpp, MPP_SET_OUTPUT_TIMEOUT,
                        (MppParam)&output_timeout_ms) != MPP_OK) {
        LOG("CreateContext: output timeout configuration failed");
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    c->profile  = profile;
    c->width    = width;
    c->height   = height;
    c->coding   = coding;
    c->sps_sent = false;
    c->render_target = VA_INVALID_SURFACE;

    if (pthread_create(&c->worker, NULL, decode_worker_main, c) != 0) {
        LOG("CreateContext: decode worker creation failed");
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    c->worker_started = true;

    uint32_t id;
    pthread_mutex_lock(&d->object_lock);
    bool inserted = rk_object_heap_insert(&d->context_heap, &c->base, &id);
    pthread_mutex_unlock(&d->object_lock);
    if (!inserted) {
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    *out_id = (VAContextID)id;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DestroyContext(VADriverContextP ctx, VAContextID id) {
    LOG("DestroyContext: ctx=0x%x", id);
    RKDriver *d = drv_from_ctx(ctx);
    pthread_mutex_lock(&d->object_lock);
    RKContext *c = (RKContext *)rk_object_heap_remove(&d->context_heap,
                                                      (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
    rk_object_unref(&c->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateBuffer(VADriverContextP ctx,
                                 VAContextID context,
                                 VABufferType type,
                                 unsigned int size,
                                 unsigned int num_elements,
                                 void *data,
                                 VABufferID *out_id) {
    RKDriver *d = drv_from_ctx(ctx);
    (void)context;

    if (size != 0 && num_elements > SIZE_MAX / size)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t bytes = (size_t)size * num_elements;

    RKBuffer *b = calloc(1, sizeof(*b));
    if (!b)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    rk_object_init(&b->base, buffer_destroy);
    b->type         = type;
    b->size         = size;
    b->num_elements = num_elements;
    b->capacity     = bytes;
    b->data         = malloc(bytes ? bytes : 1u);
    if (!b->data) {
        rk_object_unref(&b->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (data) memcpy(b->data, data, bytes);
    else      memset(b->data, 0, bytes);

    uint32_t id;
    pthread_mutex_lock(&d->object_lock);
    bool inserted = rk_object_heap_insert(&d->buffer_heap, &b->base, &id);
    pthread_mutex_unlock(&d->object_lock);
    if (!inserted) {
        rk_object_unref(&b->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    *out_id = (VABufferID)id;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_BufferSetNumElements(VADriverContextP ctx,
                                         VABufferID id, unsigned int n) {
    RKDriver *d = drv_from_ctx(ctx);
    RKBuffer *b = buffer_acquire(d, id);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;

    if (b->size != 0 && n > SIZE_MAX / b->size) {
        rk_object_unref(&b->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    size_t bytes = (size_t)b->size * n;
    void *resized = realloc(b->data, bytes ? bytes : 1u);
    if (!resized) {
        rk_object_unref(&b->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (bytes > b->capacity)
        memset((uint8_t *)resized + b->capacity, 0, bytes - b->capacity);
    b->data = resized;
    b->capacity = bytes;
    b->num_elements = n;
    rk_object_unref(&b->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_MapBuffer(VADriverContextP ctx,
                              VABufferID id, void **ptr) {
    RKDriver *d = drv_from_ctx(ctx);
    RKBuffer *b = buffer_acquire(d, id);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    *ptr = b->data;
    rk_object_unref(&b->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_UnmapBuffer(VADriverContextP ctx, VABufferID id) {
    (void)ctx; (void)id;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DestroyBuffer(VADriverContextP ctx, VABufferID id) {
    RKDriver *d = drv_from_ctx(ctx);
    pthread_mutex_lock(&d->object_lock);
    RKBuffer *b = (RKBuffer *)rk_object_heap_remove(&d->buffer_heap,
                                                    (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    rk_object_unref(&b->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_BeginPicture(VADriverContextP ctx,
                                 VAContextID ctx_id,
                                 VASurfaceID render_target) {
    LOG("BeginPicture: ctx=0x%x surface=0x%x", ctx_id, render_target);
    RKDriver  *d = drv_from_ctx(ctx);
    RKContext *c = context_acquire(d, ctx_id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;

    /* Reusing the VA surface releases its previous output frame, returning
     * that external-group buffer to MPP once codec references are also gone.
     * priv_buf remains available as the pre-decode placeholder. */
    RKSurface *s = surface_acquire(d, render_target);
    if (!s) {
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    pthread_mutex_lock(&c->picture_lock);
    RKSurface *previous_render_surface = c->render_surface;
    uint64_t previous_render_fence = c->render_fence;
    pthread_mutex_lock(&s->lock);
    bool continuation = c->coding == MPP_VIDEO_CodingAVC &&
                        s->h264_field_pending && s->ctx_id == ctx_id &&
                        !s->decoded && !s->decode_failed && s->fence != 0;
    MppFrame old_frame = continuation ? NULL : s->frame;
    MppBuffer old_backing = continuation ? NULL : s->backing_buf;
    RKDecodePool *old_pool = continuation ? NULL : s->decode_pool;
    if (!continuation) {
        s->frame = NULL;
        s->backing_buf = NULL;
        s->decode_pool = NULL;
        s->h264_field_pending = false;
        s->fence++;
        if (s->fence == 0)
            s->fence++;
    }
    s->decoded = false;
    s->decode_failed = false;
    s->ctx_id  = ctx_id;
    c->render_target = render_target;
    c->render_surface = s;
    c->render_fence  = s->fence;
    c->n_pending     = 0;
    c->has_iq        = false;
    pthread_mutex_unlock(&s->lock);
    LOG("BeginPicture: surface=0x%x fence=%llu%s", render_target,
        (unsigned long long)c->render_fence,
        continuation ? " continuation" : "");
    if (old_frame)
        mpp_frame_deinit(&old_frame);
    if (old_backing)
        mpp_buffer_put(old_backing);
    if (old_pool)
        rk_object_unref(&old_pool->base);
    pthread_mutex_unlock(&c->picture_lock);
    if (previous_render_surface) {
        fail_pending_surface_ref(previous_render_surface,
                                 previous_render_fence);
        rk_object_unref(&previous_render_surface->base);
    }
    rk_object_unref(&c->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_RenderPicture(VADriverContextP ctx,
                                  VAContextID ctx_id,
                                  VABufferID *buffers, int n) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKContext *c = context_acquire(d, ctx_id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;

    if (n < 0 || (n > 0 && !buffers)) {
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    pthread_mutex_lock(&c->picture_lock);
    if (n > 64 - c->n_pending) {
        pthread_mutex_unlock(&c->picture_lock);
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    for (int i = 0; i < n; i++) {
        c->pending[c->n_pending++] = buffers[i];
        /* Snapshot VAPictureParameterBufferH264 immediately */
        RKBuffer *b = buffer_acquire(d, buffers[i]);
        if (b && b->type == VAPictureParameterBufferType &&
            c->coding == MPP_VIDEO_CodingAVC &&
            (size_t)b->size * b->num_elements >=
                sizeof(VAPictureParameterBufferH264)) {
            memcpy(&c->last_pp, b->data,
                   sizeof(VAPictureParameterBufferH264));
        } else if (b && b->type == VAIQMatrixBufferType &&
                   c->coding == MPP_VIDEO_CodingAVC &&
                   (size_t)b->size * b->num_elements >=
                       sizeof(VAIQMatrixBufferH264)) {
            memcpy(&c->last_iq, b->data, sizeof(VAIQMatrixBufferH264));
            c->has_iq = true;
        }
        if (b)
            rk_object_unref(&b->base);
    }
    pthread_mutex_unlock(&c->picture_lock);
    rk_object_unref(&c->base);
    return VA_STATUS_SUCCESS;
}

static bool configure_external_group(RKContext *c, MppFrame info_frame)
{
    enum { EXTERNAL_POOL_COUNT = 24 };
    size_t required_size = mpp_frame_get_buf_size(info_frame);
    size_t allocation_size = required_size;
    unsigned int hs = mpp_frame_get_hor_stride(info_frame);
    unsigned int vs = mpp_frame_get_ver_stride(info_frame);

    LOG("external_group: info-change required=%zu stride=%ux%u target_hints=%d",
        required_size, hs, vs, c->n_targets);
    if (!required_size)
        return false;
    size_t conservative_size = 0;
    if (rk_surface_buffer_size((unsigned)c->width, (unsigned)c->height,
                               &conservative_size) &&
        conservative_size > allocation_size)
        allocation_size = conservative_size;

    if (c->decode_pool) {
        if (c->decode_pool->count <= 0 ||
            mpp_buffer_get_size(c->decode_pool->buffers[0]) < required_size) {
            LOG("external_group: resolution change exceeds existing pool");
            return false;
        }
        return true;
    }

    RKDecodePool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return false;
    rk_object_init(&pool->base, decode_pool_destroy);
    pool->count = EXTERNAL_POOL_COUNT;

    if (mpp_buffer_group_get_external(&pool->frame_group,
                                      MPP_BUFFER_TYPE_EXT_DMA) !=
        MPP_OK) {
        LOG("external_group: mpp_buffer_group_get_external failed");
        goto fail;
    }
    if (mpp_buffer_group_get_internal(&pool->backing_group,
                                      MPP_BUFFER_TYPE_DRM) !=
        MPP_OK) {
        LOG("external_group: backing DRM group allocation failed");
        goto fail;
    }

    pool->buffers = calloc(EXTERNAL_POOL_COUNT, sizeof(*pool->buffers));
    if (!pool->buffers)
        goto fail;

    for (int i = 0; i < EXTERNAL_POOL_COUNT; i++) {
        if (mpp_buffer_get(pool->backing_group, &pool->buffers[i],
                           allocation_size) !=
            MPP_OK) {
            LOG("external_group: backing buffer %d/%d allocation failed",
                i, EXTERNAL_POOL_COUNT);
            goto fail;
        }
        MppBufferInfo commit = {
            .type = MPP_BUFFER_TYPE_EXT_DMA,
            .size = mpp_buffer_get_size(pool->buffers[i]),
            .ptr = NULL,
            .hnd = NULL,
            .fd = mpp_buffer_get_fd(pool->buffers[i]),
            .index = i,
        };
        if (commit.fd < 0 ||
            mpp_buffer_commit(pool->frame_group, &commit) != MPP_OK) {
            LOG("external_group: commit buffer[%d] fd=%d size=%zu failed",
                i, commit.fd, commit.size);
            goto fail;
        }
    }

    if (c->mpi->control(c->mpp, MPP_DEC_SET_EXT_BUF_GROUP,
                        pool->frame_group) != MPP_OK) {
        LOG("external_group: MPP_DEC_SET_EXT_BUF_GROUP failed");
        goto fail;
    }

    c->decode_pool = pool;
    LOG("external_group: ready buffers=%d required=%zu allocated=%zu",
        pool->count, required_size, allocation_size);
    return true;

fail:
    rk_object_unref(&pool->base);
    return false;
}

static bool external_buffer_matches_pool(RKContext *c, MppBuffer buffer,
                                         int *index_out)
{
    int index = buffer ? mpp_buffer_get_index(buffer) : -1;
    RKDecodePool *pool = c->decode_pool;
    if (!pool || index < 0 || index >= pool->count)
        return false;

    MppBuffer backing = pool->buffers[index];
    int expected_fd = backing ? mpp_buffer_get_fd(backing) : -1;
    int actual_fd = mpp_buffer_get_fd(buffer);
    if (expected_fd < 0 || actual_fd != expected_fd)
        return false;

    if (index_out)
        *index_out = index;
    return true;
}

static void complete_surface_ref(RKSurface *surface, uint64_t fence,
                                 bool success)
{
    if (!surface)
        return;

    pthread_mutex_lock(&surface->lock);
    if (surface->fence == fence) {
        surface->decoded = success;
        surface->decode_failed = !success;
        surface->h264_field_pending = false;
        pthread_cond_broadcast(&surface->cond);
    }
    pthread_mutex_unlock(&surface->lock);
}

static RKFrameRoute *frame_route_create(const RKDecodeJob *job)
{
    RKFrameRoute *route = calloc(1, sizeof(*route));
    if (!route)
        return NULL;
    route->target = job->target;
    route->fence = job->fence;
    route->token = job->token;
    if (!rk_object_ref(&job->surface->base)) {
        free(route);
        return NULL;
    }
    route->surface = job->surface;
    return route;
}

static void frame_route_destroy(RKFrameRoute *route)
{
    if (!route)
        return;
    if (route->surface)
        rk_object_unref(&route->surface->base);
    free(route);
}

static void h264_route_add(RKContext *c, RKFrameRoute *route)
{
    route->next = c->h264_routes;
    c->h264_routes = route;
}

static RKFrameRoute *h264_route_find_surface(RKContext *c,
                                             VASurfaceID target,
                                             uint64_t fence)
{
    for (RKFrameRoute *route = c->h264_routes; route; route = route->next) {
        if (route->target == target && route->fence == fence)
            return route;
    }
    return NULL;
}

static RKFrameRoute *h264_route_take(RKContext *c, uint64_t token)
{
    RKFrameRoute **cursor = &c->h264_routes;
    while (*cursor) {
        if ((*cursor)->token == token) {
            RKFrameRoute *route = *cursor;
            *cursor = route->next;
            route->next = NULL;
            return route;
        }
        cursor = &(*cursor)->next;
    }
    return NULL;
}

static void generic_route_add(RKContext *c, RKFrameRoute *route)
{
    route->next = NULL;
    if (c->generic_tail)
        c->generic_tail->next = route;
    else
        c->generic_head = route;
    c->generic_tail = route;
}

static RKFrameRoute *generic_route_take(RKContext *c)
{
    RKFrameRoute *route = c->generic_head;
    if (!route)
        return NULL;
    c->generic_head = route->next;
    if (!c->generic_head)
        c->generic_tail = NULL;
    route->next = NULL;
    return route;
}

static void generic_route_remove(RKContext *c, RKFrameRoute *route)
{
    RKFrameRoute **cursor = &c->generic_head;
    RKFrameRoute *previous = NULL;
    while (*cursor) {
        if (*cursor == route) {
            *cursor = route->next;
            if (c->generic_tail == route)
                c->generic_tail = previous;
            route->next = NULL;
            return;
        }
        previous = *cursor;
        cursor = &(*cursor)->next;
    }
}

/* Route one MPP output frame to the right surface and mark it decoded.
 * Returns true when one pending output route was consumed. */
static bool assign_mpp_frame(MppFrame frame, RKContext *c)
{
    if (mpp_frame_get_info_change(frame)) {
        bool external = configure_external_group(c, frame);
        LOG("assign_mpp_frame: info_change → acknowledged mode=%s",
            external ? "external" : "internal-fallback");
        c->mpi->control(c->mpp, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        mpp_frame_deinit(&frame);
        return false;
    }

    RK_S64      raw_pts = mpp_frame_get_pts(frame);
    RKFrameRoute *route;
    RKSurface  *s;

    if (c->coding == MPP_VIDEO_CodingAVC) {
        /* H.264 can reorder display output. The worker assigns a unique PTS
         * token and resolves it back to the exact surface fence. */
        route = h264_route_take(c, (uint64_t)raw_pts);
    } else {
        /* VP9: shown frames come out in submission order (no display
         * reordering), but PTS is NOT reliable — a show_existing_frame
         * repeat of a hidden altref surfaces with the *altref packet's*
         * PTS, which routes to a surface we already marked decoded and
         * desyncs everything after it (measured: nondeterministic 60-95%
         * frame corruption before this change). The FIFO of submitted
         * shown-frame surfaces is the reliable identity, so route by it
         * unconditionally and ignore PTS. */
        route = generic_route_take(c);
    }

    if (!route) {
        LOG("assign_mpp_frame: PTS=0x%llx has no pending route, dropped",
            (unsigned long long)raw_pts);
        mpp_frame_deinit(&frame);
        return false;
    }

    VASurfaceID sid = route->target;
    if (c->coding != MPP_VIDEO_CodingAVC &&
        (uint64_t)raw_pts != route->token)
        LOG("assign_mpp_frame: PTS=0x%llx overridden by FIFO → surface=0x%x fence=%llu",
            (unsigned long long)raw_pts, (unsigned)sid,
            (unsigned long long)route->fence);

    s = route->surface;

    pthread_mutex_lock(&s->lock);
    bool current_fence = s->fence == route->fence;
    int surface_width = s->width;
    int surface_height = s->height;
    pthread_mutex_unlock(&s->lock);
    if (!current_fence) {
        LOG("assign_mpp_frame: stale surface=0x%x fence=%llu dropped",
            (unsigned)sid, (unsigned long long)route->fence);
        frame_route_destroy(route);
        mpp_frame_deinit(&frame);
        return true;
    }

    MppBuffer      buf    = mpp_frame_get_buffer(frame);
    int            fwidth = (int)mpp_frame_get_width(frame);
    int            fheight= (int)mpp_frame_get_height(frame);
    int            fhs    = (int)mpp_frame_get_hor_stride(frame);
    int            fvs    = (int)mpp_frame_get_ver_stride(frame);
    MppFrameFormat ffmt   = mpp_frame_get_fmt(frame);

    int  frame_w = fwidth  > 0 ? fwidth  : surface_width;
    int  frame_h = fheight > 0 ? fheight : surface_height;
    int  src_hs = fhs > 0 ? fhs : frame_w;
    int  src_vs = fvs > 0 ? fvs : frame_h;
    size_t src_size = buf ? mpp_buffer_get_size(buf) : 0;
    size_t layout_size = 0;
    bool linear_nv12 = (ffmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP;
    bool layout_valid = linear_nv12 && src_hs > 0 && src_vs > 0 &&
        rk_nv12_layout_size((size_t)src_hs, (size_t)src_vs, &layout_size) &&
        src_size >= layout_size;
    int pool_index = -1;
    bool pool_match = external_buffer_matches_pool(c, buf, &pool_index);
    RKDecodePool *pool = c->decode_pool;
    bool external_ready = pool != NULL;
    bool zero_copy = layout_valid &&
        (!external_ready || pool_match);
    MppBuffer backing = (zero_copy && external_ready)
                      ? pool->buffers[pool_index] : NULL;
    if (zero_copy && backing && mpp_buffer_inc_ref(backing) != MPP_OK)
        zero_copy = false;
    if (zero_copy && pool && !rk_object_ref(&pool->base)) {
        mpp_buffer_put(backing);
        backing = NULL;
        zero_copy = false;
    }

    if (zero_copy) {
        pthread_mutex_lock(&s->lock);
        if (s->fence != route->fence) {
            pthread_mutex_unlock(&s->lock);
            if (backing)
                mpp_buffer_put(backing);
            if (pool)
                rk_object_unref(&pool->base);
            frame_route_destroy(route);
            mpp_frame_deinit(&frame);
            return true;
        }
        MppFrame old_frame = s->frame;
        MppBuffer old_backing = s->backing_buf;
        RKDecodePool *old_pool = s->decode_pool;
        s->frame = frame;
        s->backing_buf = backing;
        s->decode_pool = pool;
        s->fmt = ffmt;
        if (fwidth  > 0) s->width   = fwidth;
        if (fheight > 0) s->height  = fheight;
        if (fhs     > 0) s->hstride = fhs;
        if (fvs     > 0) s->vstride = fvs;
        s->decoded = true;
        s->decode_failed = false;
        s->h264_field_pending = false;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        if (old_frame)
            mpp_frame_deinit(&old_frame);
        if (old_backing)
            mpp_buffer_put(old_backing);
        if (old_pool)
            rk_object_unref(&old_pool->base);
        LOG("assign_mpp_frame: surface=0x%x MPP %dx%d stride=%dx%d "
            "fmt=0x%x zero_copy=1 external=%d pool_index=%d fd=%d fence=%llu",
            (unsigned)sid, fwidth, fheight, fhs, fvs, (unsigned)ffmt,
            external_ready, pool_index, mpp_buffer_get_fd(buf),
            (unsigned long long)route->fence);
        frame_route_destroy(route);
        return true;
    }

    if (external_ready) {
        LOG("assign_mpp_frame: external buffer mismatch surface=0x%x "
            "index=%d fd=%d fmt=0x%x stride=%dx%d layout=%zu size=%zu",
            (unsigned)sid, buf ? mpp_buffer_get_index(buf) : -1,
            buf ? mpp_buffer_get_fd(buf) : -1, (unsigned)ffmt,
            src_hs, src_vs, layout_size, src_size);
        mpp_frame_deinit(&frame);
        complete_surface_ref(s, route->fence, false);
        frame_route_destroy(route);
        return true;
    }
    mpp_frame_deinit(&frame);
    LOG("assign_mpp_frame: unsafe internal layout surface=0x%x fmt=0x%x "
        "stride=%dx%d layout=%zu src=%zu; decode failed",
        (unsigned)sid, (unsigned)ffmt, src_hs, src_vs, layout_size,
        src_size);
    complete_surface_ref(s, route->fence, false);
    frame_route_destroy(route);
    return true;
}

/* decode_put_packet with backpressure handling. Input remains non-blocking,
 * so when MPP's queue is full the worker gives it its output consumer and
 * retries. decode_get_frame itself uses the configured bounded timeout; no
 * caller thread polls and no sleep loop is needed.
 * Without this, fast submission silently drops frames (measured on VP9:
 * 38 of 120 packets rejected, nondeterministically, before this fix). */
static bool decode_worker_stopping(RKContext *c)
{
    pthread_mutex_lock(&c->work_lock);
    bool stopping = c->worker_stop;
    pthread_mutex_unlock(&c->work_lock);
    return stopping;
}

static MPP_RET put_packet_draining(RKContext *c, MppPacket pkt)
{
    MPP_RET ret = MPP_OK;
    for (int tries = 0; tries < 500; tries++) {
        if (decode_worker_stopping(c))
            return MPP_NOK;
        ret = c->mpi->decode_put_packet(c->mpp, pkt);
        if (ret == MPP_OK) return MPP_OK;
        MppFrame f = NULL;
        if (c->mpi->decode_get_frame(c->mpp, &f) == MPP_OK && f &&
            assign_mpp_frame(f, c) && c->outstanding_frames > 0)
            c->outstanding_frames--;
    }
    return ret;
}

static RKDecodeJob *decode_job_create(RKContext *c, uint8_t *data,
                                      size_t size)
{
    RKDecodeJob *job = calloc(1, sizeof(*job));
    if (!job)
        return NULL;
    job->data = data;
    job->size = size;
    job->target = c->render_target;
    job->fence = c->render_fence;
    job->repeat_slot = 8;
    if (!c->render_surface ||
        !rk_object_ref(&c->render_surface->base)) {
        free(job);
        return NULL;
    }
    job->surface = c->render_surface;
    return job;
}

/* Build an owned Annex B access unit for the decode worker. */
static VAStatus build_h264_job(RKContext *c, RKDriver *d,
                               RKDecodeJob **job_out)
{
    /* gather slice data */
    uint8_t *pkt_data = NULL;
    size_t   pkt_cap  = 0;
    size_t   pkt_sz   = 0;

#define PKT_APPEND(ptr, len) do {                                      \
    VAStatus _append_status = packet_append(&pkt_data, &pkt_sz,        \
                                             &pkt_cap, (ptr), (len));   \
    if (_append_status != VA_STATUS_SUCCESS) {                         \
        free(pkt_data);                                                \
        return _append_status;                                         \
    }                                                                  \
} while (0)

    /* Pull the current frame's active reference counts from the first slice
     * parameter buffer. VA-API never gives us the original PPS defaults;
     * slices that don't carry num_ref_idx_active_override_flag rely on the
     * PPS default matching their active count, so we re-emit a PPS whose
     * "defaults" are this frame's values before every frame. Multi-slice
     * frames with differing per-slice counts are still correct as long as
     * the non-matching slices carry the override flag (they must, since one
     * original default couldn't have matched both either). */
    int ref_l0_minus1 = 0, ref_l1_minus1 = 0;
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_acquire(d, c->pending[i]);
        if (!b)
            continue;
        if (b->type != VASliceParameterBufferType ||
            b->capacity < sizeof(VASliceParameterBufferH264)) {
            rk_object_unref(&b->base);
            continue;
        }
        const VASliceParameterBufferH264 *sp = b->data;
        ref_l0_minus1 = sp->num_ref_idx_l0_active_minus1;
        ref_l1_minus1 = sp->num_ref_idx_l1_active_minus1;
        rk_object_unref(&b->base);
        break;
    }

    /* SPS at IDR/first frame; PPS before every frame (see above). Repeating
     * parameter sets mid-stream is legal Annex B and MPP handles it. */
    bool is_idr = false;
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_acquire(d, c->pending[i]);
        if (!b)
            continue;
        if (b->type != VASliceDataBufferType || b->capacity == 0) {
            rk_object_unref(&b->base);
            continue;
        }
        uint8_t nal_type = ((const uint8_t *)b->data)[0] & 0x1F;
        is_idr = (nal_type == 5);
        rk_object_unref(&b->base);
        break;
    }

    {
        uint8_t hdr[2048];
        int n;

        if (is_idr || !c->sps_sent) {
            n = h264_write_sps(hdr, sizeof(hdr), &c->last_pp,
                               profile_idc(c->profile));
            if (n <= 0) {
                LOG("do_h264_decode: SPS reconstruction failed");
                free(pkt_data);
                return VA_STATUS_ERROR_DECODING_ERROR;
            }
            PKT_APPEND(hdr, (size_t)n);
            c->sps_sent = true;
        }
        n = h264_write_pps(hdr, sizeof(hdr), &c->last_pp,
                           c->has_iq && profile_idc(c->profile) >= 100
                               ? &c->last_iq : NULL,
                           ref_l0_minus1, ref_l1_minus1);
        if (n <= 0) {
            LOG("do_h264_decode: PPS reconstruction failed");
            free(pkt_data);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        PKT_APPEND(hdr, (size_t)n);
    }

    /* append each slice with Annex B start code */
    static const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_acquire(d, c->pending[i]);
        if (!b)
            continue;
        if (b->type != VASliceDataBufferType) {
            rk_object_unref(&b->base);
            continue;
        }
        VAStatus append_status = packet_append(&pkt_data, &pkt_sz, &pkt_cap,
                                               sc, sizeof(sc));
        if (append_status == VA_STATUS_SUCCESS)
            append_status = packet_append(&pkt_data, &pkt_sz, &pkt_cap,
                                          b->data, b->capacity);
        rk_object_unref(&b->base);
        if (append_status != VA_STATUS_SUCCESS) {
            free(pkt_data);
            return append_status;
        }
    }
#undef PKT_APPEND

    RKDecodeJob *job = decode_job_create(c, pkt_data, pkt_sz);
    if (!job) {
        free(pkt_data);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    job->h264_field = c->last_pp.pic_fields.bits.field_pic_flag != 0;
    LOG("build_h264_job: queued %zu bytes target=0x%x fence=%llu",
        pkt_sz, (unsigned)job->target, (unsigned long long)job->fence);
    *job_out = job;
    return VA_STATUS_SUCCESS;
}

static uint8_t first_refresh_slot(uint8_t refresh_frame_flags)
{
    for (uint8_t slot = 0; slot < 8; slot++) {
        if (refresh_frame_flags & (1u << slot))
            return slot;
    }
    return 8;
}

/* For VP9, slice data is already a complete coded picture. The worker owns
 * the MPP submission and the hidden-reference repeat transaction. */
static VAStatus build_generic_job(RKContext *c, RKDriver *d,
                                  RKDecodeJob **job_out)
{
    uint8_t *pkt_data = NULL;
    size_t   pkt_sz   = 0;
    size_t   pkt_cap  = 0;

    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_acquire(d, c->pending[i]);
        if (!b)
            continue;
        if (b->type != VASliceDataBufferType) {
            rk_object_unref(&b->base);
            continue;
        }
        VAStatus append_status = packet_append(
            &pkt_data, &pkt_sz, &pkt_cap, b->data, b->capacity);
        rk_object_unref(&b->base);
        if (append_status != VA_STATUS_SUCCESS) {
            free(pkt_data);
            return append_status;
        }
    }
    RKVP9FrameInfo vp9_info = {0};
    bool is_vp9 = c->coding == MPP_VIDEO_CodingVP9;
    if (pkt_sz && is_vp9 &&
        !rk_vp9_parse_profile0_frame(pkt_data, pkt_sz, &vp9_info)) {
        LOG("build_generic_job: malformed or unsupported VP9 header");
        free(pkt_data);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    bool is_hidden = pkt_sz && is_vp9 && !vp9_info.show_existing_frame &&
                     !vp9_info.show_frame;

    RKDecodeJob *job = decode_job_create(c, pkt_data, pkt_sz);
    if (!job) {
        free(pkt_data);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    job->is_hidden = is_hidden;
    if (is_hidden)
        job->repeat_slot = first_refresh_slot(vp9_info.refresh_frame_flags);
    LOG("build_generic_job: queued %zu bytes target=0x%x fence=%llu%s",
        pkt_sz, (unsigned)job->target, (unsigned long long)job->fence,
        is_hidden ? " [altref]" : "");
    *job_out = job;
    return VA_STATUS_SUCCESS;
}

static void decode_job_destroy(RKDecodeJob *job)
{
    if (!job)
        return;
    free(job->data);
    if (job->surface)
        rk_object_unref(&job->surface->base);
    free(job);
}

static MPP_RET submit_decode_packet(RKContext *c, const uint8_t *data,
                                    size_t size, uint64_t token)
{
    MppPacket packet = NULL;
    MPP_RET ret = mpp_packet_init(&packet, (void *)data, size);
    if (ret != MPP_OK)
        return ret;
    mpp_packet_set_length(packet, size);
    mpp_packet_set_pts(packet, (RK_S64)token);
    ret = put_packet_draining(c, packet);
    mpp_packet_deinit(&packet);
    return ret;
}

static void worker_submit_job(RKContext *c, RKDecodeJob *job)
{
    if (job->size == 0) {
        complete_surface_ref(job->surface, job->fence, true);
        return;
    }

    if (c->coding == MPP_VIDEO_CodingAVC) {
        RKFrameRoute *route = job->h264_field
                            ? h264_route_find_surface(c, job->target,
                                                      job->fence)
                            : NULL;
        bool new_route = route == NULL;
        if (new_route) {
            route = frame_route_create(job);
            if (!route) {
                complete_surface_ref(job->surface, job->fence, false);
                return;
            }
            h264_route_add(c, route);
        }
        uint64_t route_token = route->token;
        MPP_RET ret = submit_decode_packet(c, job->data, job->size,
                                           route_token);
        if (ret != MPP_OK) {
            RKFrameRoute *failed = h264_route_take(c, route_token);
            if (!new_route && failed && c->outstanding_frames > 0)
                c->outstanding_frames--;
            frame_route_destroy(failed);
            LOG("decode worker: H.264 packet submission failed: %d", ret);
            complete_surface_ref(job->surface, job->fence, false);
            return;
        }
        if (new_route) {
            c->outstanding_frames++;
        } else if (!h264_route_find_surface(c, job->target, job->fence)) {
            /* Backpressure draining consumed the prior field route while the
             * continuation packet was being accepted. Track its later output
             * with the same token instead of leaving it unroutable. */
            RKFrameRoute *replacement = frame_route_create(job);
            if (!replacement) {
                complete_surface_ref(job->surface, job->fence, false);
                return;
            }
            replacement->token = route_token;
            h264_route_add(c, replacement);
            c->outstanding_frames++;
        } else {
            LOG("decode worker: paired H.264 field surface=0x%x fence=%llu",
                (unsigned)job->target, (unsigned long long)job->fence);
        }
        return;
    }

    /* A VP9 hidden frame is first submitted without a display route. When it
     * refreshes a reference slot, a synthetic show_existing_frame packet is
     * then routed to the logical VA surface as one atomic worker transaction. */
    if (job->is_hidden) {
        MPP_RET ret = submit_decode_packet(c, job->data, job->size,
                                           job->token);
        if (ret != MPP_OK) {
            LOG("decode worker: hidden VP9 packet submission failed: %d", ret);
            complete_surface_ref(job->surface, job->fence, false);
            return;
        }
        if (job->repeat_slot >= 8) {
            complete_surface_ref(job->surface, job->fence, true);
            return;
        }

        uint8_t repeat_data;
        RKFrameRoute *route = frame_route_create(job);
        if (!route ||
            !rk_vp9_make_profile0_show_existing(job->repeat_slot,
                                                 &repeat_data)) {
            frame_route_destroy(route);
            complete_surface_ref(job->surface, job->fence, false);
            return;
        }
        generic_route_add(c, route);
        ret = submit_decode_packet(c, &repeat_data, sizeof(repeat_data),
                                   job->token);
        if (ret != MPP_OK) {
            generic_route_remove(c, route);
            frame_route_destroy(route);
            LOG("decode worker: hidden VP9 repeat submission failed: %d", ret);
            complete_surface_ref(job->surface, job->fence, false);
            return;
        }
        c->outstanding_frames++;
        LOG("decode worker: hidden VP9 target=0x%x fence=%llu via ref slot %u",
            (unsigned)job->target, (unsigned long long)job->fence,
            (unsigned)job->repeat_slot);
        return;
    }

    RKFrameRoute *route = frame_route_create(job);
    if (!route) {
        complete_surface_ref(job->surface, job->fence, false);
        return;
    }
    generic_route_add(c, route);
    MPP_RET ret = submit_decode_packet(c, job->data, job->size, job->token);
    if (ret != MPP_OK) {
        generic_route_remove(c, route);
        frame_route_destroy(route);
        LOG("decode worker: packet submission failed: %d", ret);
        complete_surface_ref(job->surface, job->fence, false);
        return;
    }
    c->outstanding_frames++;
}

static void worker_drain_one(RKContext *c)
{
    MppFrame frame = NULL;
    MPP_RET ret = c->mpi->decode_get_frame(c->mpp, &frame);
    if (ret == MPP_OK && frame) {
        if (assign_mpp_frame(frame, c) &&
            c->outstanding_frames > 0)
            c->outstanding_frames--;
    } else if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
        LOG("decode worker: output wait failed: %d", ret);
    }
}

static void *decode_worker_main(void *opaque)
{
    RKContext *c = opaque;
    LOG("decode worker: started coding=%d", (int)c->coding);

    for (;;) {
        pthread_mutex_lock(&c->work_lock);
        while (!c->worker_stop && !c->job_head &&
               c->outstanding_frames == 0)
            pthread_cond_wait(&c->work_cond, &c->work_lock);

        if (c->worker_stop) {
            pthread_mutex_unlock(&c->work_lock);
            break;
        }

        RKDecodeJob *job = c->job_head;
        if (job) {
            c->job_head = job->next;
            if (!c->job_head)
                c->job_tail = NULL;
            job->next = NULL;
        }
        pthread_mutex_unlock(&c->work_lock);

        if (job) {
            worker_submit_job(c, job);
            decode_job_destroy(job);
        } else {
            worker_drain_one(c);
        }
    }

    LOG("decode worker: stopped coding=%d", (int)c->coding);
    return NULL;
}

static bool enqueue_decode_job(RKContext *c, RKDecodeJob *job)
{
    pthread_mutex_lock(&c->work_lock);
    if (c->worker_stop) {
        pthread_mutex_unlock(&c->work_lock);
        return false;
    }
    c->next_token++;
    if (c->next_token == 0 || c->next_token > INT64_MAX)
        c->next_token = 1;
    job->token = c->next_token;
    job->next = NULL;
    if (c->job_tail)
        c->job_tail->next = job;
    else
        c->job_head = job;
    c->job_tail = job;
    pthread_cond_signal(&c->work_cond);
    pthread_mutex_unlock(&c->work_lock);
    return true;
}

static void fail_pending_surface_ref(RKSurface *surface, uint64_t fence)
{
    if (!surface)
        return;
    pthread_mutex_lock(&surface->lock);
    if (surface->fence == fence && !surface->decoded) {
        surface->decode_failed = true;
        pthread_cond_broadcast(&surface->cond);
    }
    pthread_mutex_unlock(&surface->lock);
}

static void stop_decode_worker(RKContext *c)
{
    if (!c->sync_initialized)
        return;

    if (c->worker_started) {
        pthread_mutex_lock(&c->work_lock);
        c->worker_stop = true;
        pthread_cond_broadcast(&c->work_cond);
        pthread_mutex_unlock(&c->work_lock);
        pthread_join(c->worker, NULL);
        c->worker_started = false;
    }

    RKDecodeJob *job = c->job_head;
    c->job_head = NULL;
    c->job_tail = NULL;
    while (job) {
        RKDecodeJob *next = job->next;
        fail_pending_surface_ref(job->surface, job->fence);
        decode_job_destroy(job);
        job = next;
    }

    RKFrameRoute *route = c->h264_routes;
    c->h264_routes = NULL;
    while (route) {
        RKFrameRoute *next = route->next;
        fail_pending_surface_ref(route->surface, route->fence);
        frame_route_destroy(route);
        route = next;
    }
    route = c->generic_head;
    c->generic_head = NULL;
    c->generic_tail = NULL;
    while (route) {
        RKFrameRoute *next = route->next;
        fail_pending_surface_ref(route->surface, route->fence);
        frame_route_destroy(route);
        route = next;
    }
    c->outstanding_frames = 0;

    pthread_mutex_lock(&c->picture_lock);
    RKSurface *render_surface = c->render_surface;
    uint64_t fence = c->render_fence;
    c->render_target = VA_INVALID_SURFACE;
    c->render_surface = NULL;
    c->render_fence = 0;
    pthread_mutex_unlock(&c->picture_lock);
    if (render_surface) {
        fail_pending_surface_ref(render_surface, fence);
        rk_object_unref(&render_surface->base);
    }
}

static VAStatus rk_EndPicture(VADriverContextP ctx, VAContextID ctx_id) {
    LOG("EndPicture: ctx=0x%x", ctx_id);
    RKDriver  *d = drv_from_ctx(ctx);
    RKContext *c = context_acquire(d, ctx_id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;

    RKDecodeJob *job = NULL;
    VAStatus st;
    pthread_mutex_lock(&c->picture_lock);
    if (c->render_target == VA_INVALID_SURFACE || !c->render_surface ||
        c->render_fence == 0) {
        pthread_mutex_unlock(&c->picture_lock);
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (c->coding == MPP_VIDEO_CodingAVC)
        st = build_h264_job(c, d, &job);
    else
        st = build_generic_job(c, d, &job);
    RKSurface *render_surface = c->render_surface;
    uint64_t render_fence = c->render_fence;
    if (st == VA_STATUS_SUCCESS && job) {
        pthread_mutex_lock(&render_surface->lock);
        if (render_surface->fence == render_fence)
            render_surface->h264_field_pending = job->h264_field;
        pthread_mutex_unlock(&render_surface->lock);
    }
    c->n_pending = 0;
    c->render_target = VA_INVALID_SURFACE;
    c->render_surface = NULL;
    c->render_fence = 0;
    pthread_mutex_unlock(&c->picture_lock);
    if (st == VA_STATUS_SUCCESS && !job)
        st = VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (st != VA_STATUS_SUCCESS)
        fail_pending_surface_ref(render_surface, render_fence);
    rk_object_unref(&render_surface->base);

    if (st != VA_STATUS_SUCCESS) {
        decode_job_destroy(job);
        job = NULL;
    }

    if (st == VA_STATUS_SUCCESS && !enqueue_decode_job(c, job)) {
        complete_surface_ref(job->surface, job->fence, false);
        decode_job_destroy(job);
        st = VA_STATUS_ERROR_OPERATION_FAILED;
    }

    rk_object_unref(&c->base);
    return st;
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

static VAStatus rk_SyncSurface(VADriverContextP ctx, VASurfaceID id) {
    return sync_surface_timeout(ctx, id, VA_TIMEOUT_INFINITE);
}

static VAStatus rk_SyncSurface2(VADriverContextP ctx,
                                  VASurfaceID id, uint64_t timeout_ns) {
    return sync_surface_timeout(ctx, id, timeout_ns);
}

static VAStatus rk_QuerySurfaceStatus(VADriverContextP ctx,
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

static VAStatus rk_ExportSurfaceHandle(VADriverContextP ctx,
                                        VASurfaceID id,
                                        uint32_t mem_type,
                                        uint32_t flags,
                                        void *descriptor) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_acquire(d, id);
    LOG("ExportSurfaceHandle: surface=0x%x mem_type=0x%x flags=0x%x",
        id, mem_type, flags);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        LOG("ExportSurfaceHandle: unsupported mem_type 0x%x", mem_type);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (!descriptor) {
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    /* If decode is in progress, sync now so the exported DMA-BUF contains the
     * correct frame. Firefox calls ExportSurfaceHandle before SyncSurface when
     * EndPicture is async; without this the EGLImage gets stale data. */
    pthread_mutex_lock(&s->lock);
    bool needs_sync = !s->decoded && (s->ctx_id != 0);
    pthread_mutex_unlock(&s->lock);
    if (needs_sync) {
        VAStatus sync_status = rk_SyncSurface(ctx, id);
        if (sync_status != VA_STATUS_SUCCESS) {
            rk_object_unref(&s->base);
            return sync_status;
        }
    }

    pthread_mutex_lock(&s->lock);
    MppBuffer active_buffer = s->frame
                            ? mpp_frame_get_buffer(s->frame)
                            : s->priv_buf;
    int fd       = active_buffer ? mpp_buffer_get_fd(active_buffer) : -1;
    size_t object_size = active_buffer ? mpp_buffer_get_size(active_buffer) : 0;
    int hs       = s->hstride ? s->hstride : s->width;
    int vs       = s->vstride ? s->vstride : s->height;
    int width    = s->width;
    int height   = s->height;
    bool decoded = s->decoded;
    bool is_placeholder = (s->frame == NULL);
    bool is_10bit = MPP_FRAME_FMT_IS_YUV_10BIT(s->fmt);
    int export_fd = fd >= 0 ? dup(fd) : -1;
    int dup_errno = export_fd < 0 ? errno : 0;
    pthread_mutex_unlock(&s->lock);

    if (fd < 0 || object_size == 0 || object_size > UINT32_MAX) {
        if (export_fd >= 0)
            close(export_fd);
        LOG("ExportSurfaceHandle: buffer not exportable (fd=%d size=%zu decoded=%d)",
            fd, object_size, decoded);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (export_fd < 0) {
        LOG("ExportSurfaceHandle: dup(%d) failed errno=%d, ERROR_ALLOCATION_FAILED",
            fd, dup_errno);
        rk_object_unref(&s->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    LOG("ExportSurfaceHandle: surface=0x%x %dx%d stride=%dx%d export_fd=%d decoded=%d placeholder=%d 10bit=%d",
        id, width, height, hs, vs, export_fd, decoded, is_placeholder, is_10bit);

    VADRMPRIMESurfaceDescriptor *desc = descriptor;
    memset(desc, 0, sizeof(*desc));
    desc->width       = (uint32_t)width;
    desc->height      = (uint32_t)height;
    desc->num_objects = 1;
    desc->objects[0].fd                  = export_fd;
    desc->objects[0].size                = (uint32_t)object_size;
    desc->objects[0].drm_format_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */

    bool composed = (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS) != 0;

    /* COMPOSED_LAYERS: single NV12/P010 layer with 2 planes (mpv, GStreamer).
     * SEPARATE_LAYERS (default): R8/GR88 split planes (Firefox DMABufSurfaceYUV). */
    if (!is_10bit && composed) {
        desc->fourcc     = VA_FOURCC_NV12;
        desc->num_layers = 1;
        desc->layers[0].drm_format      = 0x3231564e; /* DRM_FORMAT_NV12 */
        desc->layers[0].num_planes      = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0]       = 0;
        desc->layers[0].pitch[0]        = (uint32_t)hs;
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1]       = (uint32_t)(hs * vs);
        desc->layers[0].pitch[1]        = (uint32_t)hs;
        rk_object_unref(&s->base);
        return VA_STATUS_SUCCESS;
    }
    if (is_10bit && composed) {
        desc->fourcc     = VA_FOURCC_P010;
        desc->num_layers = 1;
        desc->layers[0].drm_format      = 0x30313050; /* DRM_FORMAT_P010 */
        desc->layers[0].num_planes      = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0]       = 0;
        desc->layers[0].pitch[0]        = (uint32_t)(hs * 2);
        desc->layers[0].object_index[1] = 0;
        desc->layers[0].offset[1]       = (uint32_t)(hs * vs * 2);
        desc->layers[0].pitch[1]        = (uint32_t)(hs * 2);
        rk_object_unref(&s->base);
        return VA_STATUS_SUCCESS;
    }

    desc->num_layers = 2;

    if (is_10bit) {
        /*
         * P010: 10-bit NV12 semi-planar.  Each luma/chroma sample is uint16.
         * Two-layer layout matching Firefox DMABufSurfaceYUV P010 import:
         *   layer 0 → DRM_FORMAT_R16    (Y,  16-bit luma, 10 significant bits)
         *   layer 1 → DRM_FORMAT_GR1616 (UV, 2×16-bit interleaved chroma)
         * Both layers share pitch = hs*2 (bytes per luma row).
         * UV offset = hs*vs*2 (Y plane size in bytes).
         */
        desc->fourcc                         = VA_FOURCC_P010;
        /* Y plane */
        desc->layers[0].drm_format           = 0x20363152; /* DRM_FORMAT_R16    */
        desc->layers[0].num_planes           = 1;
        desc->layers[0].object_index[0]      = 0;
        desc->layers[0].offset[0]            = 0;
        desc->layers[0].pitch[0]             = (uint32_t)(hs * 2);
        /* UV plane */
        desc->layers[1].drm_format           = 0x36315247; /* DRM_FORMAT_GR1616 */
        desc->layers[1].num_planes           = 1;
        desc->layers[1].object_index[0]      = 0;
        desc->layers[1].offset[0]            = (uint32_t)(hs * vs * 2);
        desc->layers[1].pitch[0]             = (uint32_t)(hs * 2);
    } else {
        /*
         * NV12: 8-bit YUV 4:2:0 semi-planar.
         * Firefox (DMABufSurfaceYUV) expects num_layers == plane count, each
         * layer being a single-plane view of the buffer:
         *   layer 0 → DRM_FORMAT_R8   (Y,  8-bit luma)
         *   layer 1 → DRM_FORMAT_GR88 (UV, interleaved chroma, 2 bytes/px)
         */
        desc->fourcc                         = VA_FOURCC_NV12;
        /* Y plane */
        desc->layers[0].drm_format           = 0x20203852; /* DRM_FORMAT_R8   */
        desc->layers[0].num_planes           = 1;
        desc->layers[0].object_index[0]      = 0;
        desc->layers[0].offset[0]            = 0;
        desc->layers[0].pitch[0]             = (uint32_t)hs;
        /* UV plane */
        desc->layers[1].drm_format           = 0x38385247; /* DRM_FORMAT_GR88 */
        desc->layers[1].num_planes           = 1;
        desc->layers[1].object_index[0]      = 0;
        desc->layers[1].offset[0]            = (uint32_t)(hs * vs);
        desc->layers[1].pitch[0]             = (uint32_t)hs;
    }
    rk_object_unref(&s->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QueryImageFormats(VADriverContextP ctx,
                                      VAImageFormat *list, int *n) {
    (void)ctx;
    list[0].fourcc         = VA_FOURCC_NV12;
    list[0].byte_order     = VA_LSB_FIRST;
    list[0].bits_per_pixel = 12;
    list[1].fourcc         = VA_FOURCC_P010;
    list[1].byte_order     = VA_LSB_FIRST;
    list[1].bits_per_pixel = 24;
    *n = 2;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateImage(VADriverContextP ctx,
                                VAImageFormat *format,
                                int width, int height,
                                VAImage *image) {
    RKDriver *d = drv_from_ctx(ctx);
    if (!format || !image || width <= 0 || height <= 0 ||
        width > USHRT_MAX || height > USHRT_MAX)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    unsigned int bytes_per_sample;
    if (format->fourcc == VA_FOURCC_NV12)
        bytes_per_sample = 1;
    else if (format->fourcc == VA_FOURCC_P010)
        bytes_per_sample = 2;
    else
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    size_t aligned_width = ((size_t)(unsigned int)width + 15u) & ~15u;
    if (aligned_width > UINT_MAX / bytes_per_sample)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    unsigned int pitch = (unsigned int)aligned_width * bytes_per_sample;
    size_t allocation_size;
    if (!rk_nv12_layout_size(pitch, (size_t)(unsigned int)height,
                             &allocation_size) || allocation_size > UINT_MAX)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    VABufferID buf_id;
    unsigned int size = (unsigned int)allocation_size;
    VAStatus st = rk_CreateBuffer(ctx, 0, VAImageBufferType, size, 1,
                                  NULL, &buf_id);
    if (st != VA_STATUS_SUCCESS) return st;

    RKBuffer *buffer = buffer_acquire(d, buf_id);
    if (!buffer) {
        rk_DestroyBuffer(ctx, buf_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    RKImage *image_object = calloc(1, sizeof(*image_object));
    if (!image_object) {
        rk_object_unref(&buffer->base);
        rk_DestroyBuffer(ctx, buf_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    rk_object_init(&image_object->base, image_destroy);
    image_object->buffer_id = buf_id;
    image_object->buffer = buffer;
    image_object->fourcc = format->fourcc;
    image_object->width = (unsigned int)width;
    image_object->height = (unsigned int)height;
    image_object->pitch = pitch;

    uint32_t image_id;
    pthread_mutex_lock(&d->object_lock);
    bool inserted = rk_object_heap_insert(&d->image_heap,
                                          &image_object->base, &image_id);
    pthread_mutex_unlock(&d->object_lock);
    if (!inserted) {
        rk_object_unref(&image_object->base);
        rk_DestroyBuffer(ctx, buf_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    memset(image, 0, sizeof(*image));
    image->image_id    = (VAImageID)image_id;
    image->buf         = buf_id;
    image->format      = *format;
    image->width       = (unsigned short)width;
    image->height      = (unsigned short)height;
    image->num_planes  = 2;
    image->pitches[0]  = pitch;
    image->pitches[1]  = pitch;
    image->offsets[0]  = 0;
    image->offsets[1]  = pitch * (unsigned int)height;
    image->data_size   = size;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DeriveImage(VADriverContextP ctx,
                                VASurfaceID sid, VAImage *image) {
    RKDriver  *d = drv_from_ctx(ctx);
    RKSurface *s = surface_acquire(d, sid);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;

    /* priv_buf is not CPU-mappable as a shared VAImage; force callers to use
     * vaGetImage instead (copies pixels, but always works). */
    (void)image;
    rk_object_unref(&s->base);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus rk_DestroyImage(VADriverContextP ctx, VAImageID id) {
    RKDriver *d = drv_from_ctx(ctx);
    pthread_mutex_lock(&d->object_lock);
    RKImage *image = (RKImage *)rk_object_heap_remove(&d->image_heap,
                                                      (uint32_t)id);
    RKBuffer *buffer = image ? (RKBuffer *)rk_object_heap_remove(
        &d->buffer_heap, (uint32_t)image->buffer_id) : NULL;
    pthread_mutex_unlock(&d->object_lock);
    if (!image)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if (buffer)
        rk_object_unref(&buffer->base);
    rk_object_unref(&image->base);
    return VA_STATUS_SUCCESS;
}

/* ── stub implementations ────────────────────────────────────── */

/* Suppress -Wunused-parameter for pure stub functions */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static VAStatus rk_SetImagePalette(VADriverContextP ctx, VAImageID image,
                                    unsigned char *palette)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_GetImage(VADriverContextP ctx, VASurfaceID surface_id,
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

static VAStatus rk_PutImage(VADriverContextP ctx, VASurfaceID surface,
                             VAImageID image, int src_x, int src_y,
                             unsigned int src_width, unsigned int src_height,
                             int dest_x, int dest_y,
                             unsigned int dest_width, unsigned int dest_height)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QuerySubpicFmts(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats)
{ *num_formats = 0; return VA_STATUS_SUCCESS; }

static VAStatus rk_CreateSubpicture(VADriverContextP ctx,
                                     VAImageID image,
                                     VASubpictureID *subpicture)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_DestroySubpicture(VADriverContextP ctx,
                                      VASubpictureID subpicture)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicImage(VADriverContextP ctx,
                                   VASubpictureID subpicture, VAImageID image)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicChromakey(VADriverContextP ctx,
                                       VASubpictureID subpicture,
                                       unsigned int chromakey_min,
                                       unsigned int chromakey_max,
                                       unsigned int chromakey_mask)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicAlpha(VADriverContextP ctx,
                                   VASubpictureID subpicture, float global_alpha)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_AssociateSubpic(VADriverContextP ctx,
                                    VASubpictureID subpicture,
                                    VASurfaceID *target_surfaces, int num_surfaces,
                                    short src_x, short src_y,
                                    unsigned short src_width, unsigned short src_height,
                                    short dest_x, short dest_y,
                                    unsigned short dest_width, unsigned short dest_height,
                                    unsigned int flags)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_DeassociateSubpic(VADriverContextP ctx,
                                      VASubpictureID subpicture,
                                      VASurfaceID *target_surfaces, int num_surfaces)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QueryDisplayAttrs(VADriverContextP ctx,
                                      VADisplayAttribute *attr_list,
                                      int *num_attributes)
{ *num_attributes = 0; return VA_STATUS_SUCCESS; }

static VAStatus rk_GetDisplayAttrs(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetDisplayAttrs(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_BufferInfo(VADriverContextP ctx, VABufferID buf_id,
                               VABufferType *type, unsigned int *size,
                               unsigned int *num_elements)
{
    RKDriver *d = drv_from_ctx(ctx);
    RKBuffer *b = buffer_acquire(d, buf_id);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (type)         *type         = b->type;
    if (size)         *size         = b->size;
    if (num_elements) *num_elements = b->num_elements;
    rk_object_unref(&b->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_LockSurface(VADriverContextP ctx, VASurfaceID surface,
                                unsigned int *fourcc,
                                unsigned int *luma_stride,
                                unsigned int *chroma_u_stride,
                                unsigned int *chroma_v_stride,
                                unsigned int *luma_offset,
                                unsigned int *chroma_u_offset,
                                unsigned int *chroma_v_offset,
                                unsigned int *buffer_name, void **buffer)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_UnlockSurface(VADriverContextP ctx, VASurfaceID surface)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QuerySurfaceAttrs(VADriverContextP ctx, VAConfigID config,
                                      VASurfaceAttrib *attrib_list,
                                      unsigned int *num_attribs)
{
    LOG("QuerySurfaceAttributes: config=0x%x list=%s",
        config, attrib_list ? "provided" : "NULL (query count)");

    /* Firefox calls this twice: first with NULL to get count, then with buffer */
    const unsigned int n = 4;
    if (!attrib_list) {
        *num_attribs = n;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < n) {
        *num_attribs = n;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    /* Pixel format: NV12 (8-bit) */
    attrib_list[0].type              = VASurfaceAttribPixelFormat;
    attrib_list[0].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[0].value.type        = VAGenericValueTypeInteger;
    attrib_list[0].value.value.i     = VA_FOURCC_NV12;

    /* Pixel format: P010 (10-bit) */
    attrib_list[1].type              = VASurfaceAttribPixelFormat;
    attrib_list[1].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[1].value.type        = VAGenericValueTypeInteger;
    attrib_list[1].value.value.i     = VA_FOURCC_P010;

    /* Memory type: VA-managed + DRM PRIME 2 */
    attrib_list[2].type              = VASurfaceAttribMemoryType;
    attrib_list[2].flags             = VA_SURFACE_ATTRIB_GETTABLE |
                                       VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[2].value.type        = VAGenericValueTypeInteger;
    attrib_list[2].value.value.i     = (int)(VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                                       VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);

    /* Max resolution */
    attrib_list[3].type              = VASurfaceAttribMaxWidth;
    attrib_list[3].flags             = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[3].value.type        = VAGenericValueTypeInteger;
    attrib_list[3].value.value.i     = 7680;

    *num_attribs = n;
    LOG("QuerySurfaceAttributes: returned %u attribs (NV12, P010, DRM_PRIME_2)", n);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_AcquireBufferHandle(VADriverContextP ctx,
                                        VABufferID buf_id, VABufferInfo *buf_info)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_CreateMFContext(VADriverContextP ctx,
                                    VAMFContextID *mfe_context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFAddContext(VADriverContextP ctx,
                                 VAMFContextID mf_context, VAContextID context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFReleaseContext(VADriverContextP ctx,
                                     VAMFContextID mf_context, VAContextID context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFSubmit(VADriverContextP ctx, VAMFContextID mf_context,
                             VAContextID *contexts, int num_contexts)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_QueryProcessingRate(VADriverContextP ctx,
                                        VAConfigID config_id,
                                        VAProcessingRateParameter *proc_buf,
                                        unsigned int *processing_rate)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_SyncBuffer(VADriverContextP ctx, VABufferID buf_id,
                               uint64_t timeout_ns)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_Copy(VADriverContextP ctx, VACopyObject *dst,
                         VACopyObject *src, VACopyOption option)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

#pragma GCC diagnostic pop

static VAStatus rk_PutSurface(VADriverContextP ctx, VASurfaceID s,
                               void *draw, short sx, short sy,
                               unsigned short sw, unsigned short sh,
                               short dx, short dy,
                               unsigned short dw, unsigned short dh,
                               VARectangle *clips, unsigned int nc,
                               unsigned int flags) {
    (void)ctx;(void)s;(void)draw;(void)sx;(void)sy;(void)sw;(void)sh;
    (void)dx;(void)dy;(void)dw;(void)dh;(void)clips;(void)nc;(void)flags;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QuerySurfaceError(VADriverContextP ctx, VASurfaceID s,
                                      VAStatus err, void **info) {
    (void)ctx;(void)s;(void)err; *info = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateBuffer2(VADriverContextP ctx, VAContextID context,
                                  VABufferType type,
                                  unsigned int w, unsigned int h,
                                  unsigned int *unit_size,
                                  unsigned int *pitch,
                                  VABufferID *id) {
    unsigned int stride = (w + 15) & ~15;
    unsigned int size   = stride * h;
    if (unit_size) *unit_size = size;
    if (pitch)     *pitch     = stride;
    return rk_CreateBuffer(ctx, context, type, size, 1, NULL, id);
}

static VAStatus rk_GetSurfaceAttributes(VADriverContextP ctx,
                                         VAConfigID config,
                                         VASurfaceAttrib *list,
                                         unsigned int n) {
    (void)ctx;(void)config;(void)list;(void)n;
    return VA_STATUS_SUCCESS;
}

/* ── driver init ─────────────────────────────────────────────── */

VAStatus __vaDriverInit_1_20(VADriverContextP ctx)  /* NOLINT */
{
    log_init();
    LOG("__vaDriverInit_1_20: entry");
    RKDriver *d = calloc(1, sizeof(*d));
    if (!d) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (pthread_mutex_init(&d->object_lock, NULL) != 0) {
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->config_heap, RK_OBJECT_CONFIG)) {
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->context_heap, RK_OBJECT_CONTEXT)) {
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->surface_heap, RK_OBJECT_SURFACE)) {
        rk_object_heap_finish(&d->context_heap);
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->buffer_heap, RK_OBJECT_BUFFER)) {
        rk_object_heap_finish(&d->surface_heap);
        rk_object_heap_finish(&d->context_heap);
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->image_heap, RK_OBJECT_IMAGE)) {
        rk_object_heap_finish(&d->buffer_heap);
        rk_object_heap_finish(&d->surface_heap);
        rk_object_heap_finish(&d->context_heap);
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    ctx->pDriverData = d;

    ctx->version_major        = VA_MAJOR_VERSION;
    ctx->version_minor        = VA_MINOR_VERSION;
    ctx->max_profiles         = 16;
    ctx->max_entrypoints      = 4;
    ctx->max_attributes       = 8;
    ctx->max_image_formats    = 4;
    ctx->max_subpic_formats   = 4;
    ctx->max_display_attributes = 4;
    ctx->str_vendor           = "Rockchip MPP VA-API Driver 0.1";

    struct VADriverVTable *v = ctx->vtable;
    v->vaTerminate                = rk_Terminate;
    v->vaQueryConfigProfiles      = rk_QueryConfigProfiles;
    v->vaQueryConfigEntrypoints   = rk_QueryConfigEntrypoints;
    v->vaGetConfigAttributes      = rk_GetConfigAttributes;
    v->vaCreateConfig             = rk_CreateConfig;
    v->vaDestroyConfig            = rk_DestroyConfig;
    v->vaQueryConfigAttributes    = rk_QueryConfigAttributes;
    v->vaCreateSurfaces           = rk_CreateSurfaces;
    v->vaDestroySurfaces          = rk_DestroySurfaces;
    v->vaCreateContext            = rk_CreateContext;
    v->vaDestroyContext           = rk_DestroyContext;
    v->vaCreateBuffer             = rk_CreateBuffer;
    v->vaBufferSetNumElements     = rk_BufferSetNumElements;
    v->vaMapBuffer                = rk_MapBuffer;
    v->vaUnmapBuffer              = rk_UnmapBuffer;
    v->vaDestroyBuffer            = rk_DestroyBuffer;
    v->vaBeginPicture             = rk_BeginPicture;
    v->vaRenderPicture            = rk_RenderPicture;
    v->vaEndPicture               = rk_EndPicture;
    v->vaSyncSurface              = rk_SyncSurface;
    v->vaQuerySurfaceStatus       = rk_QuerySurfaceStatus;
    v->vaQuerySurfaceError        = rk_QuerySurfaceError;
    v->vaPutSurface               = rk_PutSurface;
    v->vaQueryImageFormats        = rk_QueryImageFormats;
    v->vaCreateImage              = rk_CreateImage;
    v->vaDeriveImage              = rk_DeriveImage;
    v->vaDestroyImage             = rk_DestroyImage;
    v->vaSetImagePalette          = rk_SetImagePalette;
    v->vaGetImage                 = rk_GetImage;
    v->vaPutImage                 = rk_PutImage;
    v->vaQuerySubpictureFormats   = rk_QuerySubpicFmts;
    v->vaCreateSubpicture         = rk_CreateSubpicture;
    v->vaDestroySubpicture        = rk_DestroySubpicture;
    v->vaSetSubpictureImage       = rk_SetSubpicImage;
    v->vaSetSubpictureChromakey   = rk_SetSubpicChromakey;
    v->vaSetSubpictureGlobalAlpha = rk_SetSubpicAlpha;
    v->vaAssociateSubpicture      = rk_AssociateSubpic;
    v->vaDeassociateSubpicture    = rk_DeassociateSubpic;
    v->vaQueryDisplayAttributes   = rk_QueryDisplayAttrs;
    v->vaGetDisplayAttributes     = rk_GetDisplayAttrs;
    v->vaSetDisplayAttributes     = rk_SetDisplayAttrs;
    v->vaBufferInfo               = rk_BufferInfo;
    v->vaLockSurface              = rk_LockSurface;
    v->vaUnlockSurface            = rk_UnlockSurface;
    v->vaGetSurfaceAttributes     = rk_GetSurfaceAttributes;
    v->vaCreateSurfaces2          = rk_CreateSurfaces2;
    v->vaQuerySurfaceAttributes   = rk_QuerySurfaceAttrs;
    v->vaAcquireBufferHandle      = rk_AcquireBufferHandle;
    v->vaReleaseBufferHandle      = rk_ReleaseBufferHandle;
    v->vaCreateMFContext          = rk_CreateMFContext;
    v->vaMFAddContext             = rk_MFAddContext;
    v->vaMFReleaseContext         = rk_MFReleaseContext;
    v->vaMFSubmit                 = rk_MFSubmit;
    v->vaCreateBuffer2            = rk_CreateBuffer2;
    v->vaQueryProcessingRate      = rk_QueryProcessingRate;
    v->vaExportSurfaceHandle      = rk_ExportSurfaceHandle;
    v->vaSyncSurface2             = rk_SyncSurface2;
    v->vaSyncBuffer               = rk_SyncBuffer;
    v->vaCopy                     = rk_Copy;

    LOG("driver init OK — Rockchip RK3588 MPP");
    return VA_STATUS_SUCCESS;
}
