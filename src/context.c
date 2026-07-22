#include "context.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>

#include "driver_internal.h"
#include "frame_layout.h"
#include "h264.h"
#include "vp9.h"

static void stop_decode_worker(RKContext *context);
static void *decode_worker_main(void *opaque);
static void fail_pending_surface_ref(RKSurface *surface, uint64_t fence);

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

VAStatus rk_CreateContext(VADriverContextP ctx,
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

VAStatus rk_DestroyContext(VADriverContextP ctx, VAContextID id) {
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

VAStatus rk_BeginPicture(VADriverContextP ctx,
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

VAStatus rk_RenderPicture(VADriverContextP ctx,
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

VAStatus rk_EndPicture(VADriverContextP ctx, VAContextID ctx_id) {
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

