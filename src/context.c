#include "context.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/rk_mpi.h>

#include "driver_internal.h"
#include "mpp_dec.h"

static void context_destroy(void *opaque) {
    RKContext *context = opaque;
    rk_mpp_dec_stop(context);
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

    if (pthread_create(&c->worker, NULL, rk_mpp_dec_worker_main, c) != 0) {
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
        rk_mpp_dec_fail_surface(previous_render_surface,
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
    st = rk_mpp_dec_build_job(c, d, &job);
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
        rk_mpp_dec_fail_surface(render_surface, render_fence);
    rk_object_unref(&render_surface->base);

    if (st != VA_STATUS_SUCCESS) {
        rk_mpp_dec_job_destroy(job);
        job = NULL;
    }

    if (st == VA_STATUS_SUCCESS && !rk_mpp_dec_enqueue_job(c, job)) {
        rk_mpp_dec_reject_job(job);
        st = VA_STATUS_ERROR_OPERATION_FAILED;
    }

    rk_object_unref(&c->base);
    return st;
}
