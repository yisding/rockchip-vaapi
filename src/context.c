#include "context.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/rk_mpi.h>

#include "convert.h"
#include "driver_internal.h"
#include "mpp_dec.h"
#include "mpp_enc.h"

static void context_destroy(void *opaque) {
    RKContext *context = opaque;
    if (!context->is_encoder)
        rk_mpp_dec_stop(context);
    if (context->enc_cfg)
        mpp_enc_cfg_deinit(context->enc_cfg);
    if (context->mpp)
        mpp_destroy(context->mpp);
    if (context->decode_pool)
        rk_object_unref(&context->decode_pool->base);
    for (int i = 0; i < context->n_targets; i++) {
        if (context->targets[i])
            rk_object_unref(&context->targets[i]->base);
    }
    free(context->targets);
    free(context->hevc_sequence_headers);
    if (context->render_surface)
        rk_object_unref(&context->render_surface->base);
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

    if (width <= 0 || height <= 0 ||
        width > RK_MAX_WIDTH || height > RK_MAX_HEIGHT || n_targets < 0 ||
        (n_targets > 0 && !targets) || !out_id)
        return width > RK_MAX_WIDTH || height > RK_MAX_HEIGHT
             ? VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED
             : VA_STATUS_ERROR_INVALID_PARAMETER;

    RKConfig *cfg = config_acquire(d, config_id);
    if (!cfg) return VA_STATUS_ERROR_INVALID_CONFIG;

    MppCodingType coding = profile_to_coding(cfg->profile);
    VAProfile profile = cfg->profile;
    VAEntrypoint entrypoint = cfg->entrypoint;
    uint32_t rate_control = cfg->rate_control;
    rk_object_unref(&cfg->base);
    if (coding == MPP_VIDEO_CodingUnused)
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    RKContext *c = calloc(1, sizeof(*c));
    if (!c)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    rk_object_init(&c->base, context_destroy);
    c->driver = d;
    c->profile = profile;
    c->entrypoint = entrypoint;
    c->rate_control = rate_control;
    c->is_encoder = entrypoint == VAEntrypointEncSlice;
    c->width = width;
    c->height = height;
    c->coding = coding;
    c->render_target = VA_INVALID_SURFACE;

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

    ret = mpp_init(c->mpp, c->is_encoder ? MPP_CTX_ENC : MPP_CTX_DEC,
                   coding);
    if (ret != MPP_OK) {
        LOG("mpp_init FAILED: %d (coding=%d)", ret, (int)coding);
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    LOG("CreateContext: mpp_init OK");

    if (c->is_encoder) {
        VAStatus enc_status = rk_mpp_enc_init(c);
        if (enc_status != VA_STATUS_SUCCESS) {
            rk_object_unref(&c->base);
            return enc_status;
        }
    } else {
        bool output_10bit = profile == VAProfileHEVCMain10 ||
                            profile == VAProfileVP9Profile2;
        if (output_10bit) {
            RK_U32 output_format = MPP_FRAME_FBC_AFBC_V2;
            if (!rk_rga_available() ||
                c->mpi->control(c->mpp, MPP_DEC_SET_OUTPUT_FORMAT,
                                &output_format) != MPP_OK) {
                LOG("CreateContext: 10-bit AFBC output configuration failed "
                    "profile=%d", profile);
                rk_object_unref(&c->base);
                return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
            }
            LOG("CreateContext: 10-bit output mode=AFBC_V2 profile=%d", profile);
        }

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

        RK_S64 input_timeout_ms = 0;
        RK_S64 output_timeout_ms = 20;
        if (c->mpi->control(c->mpp, MPP_SET_INPUT_TIMEOUT,
                            &input_timeout_ms) != MPP_OK ||
            c->mpi->control(c->mpp, MPP_SET_OUTPUT_TIMEOUT,
                            &output_timeout_ms) != MPP_OK) {
            LOG("CreateContext: decoder timeout configuration failed");
            rk_object_unref(&c->base);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        if (coding == MPP_VIDEO_CodingHEVC) {
            RK_U32 immediate_out = 1;
            if (c->mpi->control(c->mpp, MPP_DEC_SET_IMMEDIATE_OUT,
                                &immediate_out) != MPP_OK) {
                LOG("CreateContext: HEVC immediate-output configuration failed");
                rk_object_unref(&c->base);
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
        }
        c->sps_sent = false;
        if (pthread_create(&c->worker, NULL, rk_mpp_dec_worker_main, c) != 0) {
            LOG("CreateContext: decode worker creation failed");
            rk_object_unref(&c->base);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        c->worker_started = true;
    }

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

    if (c->is_encoder) {
        bool exact_dimensions = s->width == c->width &&
                                s->height == c->height;
        bool aligned_hevc_dimensions =
            c->coding == MPP_VIDEO_CodingHEVC &&
            ((s->width + 15) & ~15) == c->width &&
            ((s->height + 15) & ~15) == c->height;
        if ((!exact_dimensions && !aligned_hevc_dimensions) ||
            MPP_FRAME_FMT_IS_YUV_10BIT(s->fmt)) {
            rk_object_unref(&s->base);
            rk_object_unref(&c->base);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        pthread_mutex_lock(&c->picture_lock);
        if (c->render_surface) {
            pthread_mutex_unlock(&c->picture_lock);
            rk_object_unref(&s->base);
            rk_object_unref(&c->base);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        c->render_target = render_target;
        c->render_surface = s;
        c->has_enc_pic = false;
        c->has_enc_slice = false;
        pthread_mutex_unlock(&c->picture_lock);
        rk_object_unref(&c->base);
        return VA_STATUS_SUCCESS;
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
    c->has_hevc_pp   = false;
    c->has_hevc_iq   = false;
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

    if (c->is_encoder) {
        VAStatus status = VA_STATUS_SUCCESS;
        pthread_mutex_lock(&c->picture_lock);
        if (!c->render_surface)
            status = VA_STATUS_ERROR_OPERATION_FAILED;
        for (int i = 0; status == VA_STATUS_SUCCESS && i < n; i++) {
            RKBuffer *buffer = buffer_acquire(d, buffers[i]);
            if (!buffer) {
                status = VA_STATUS_ERROR_INVALID_BUFFER;
                break;
            }
            status = rk_mpp_enc_render_buffer(c, buffer);
            rk_object_unref(&buffer->base);
        }
        pthread_mutex_unlock(&c->picture_lock);
        rk_object_unref(&c->base);
        return status;
    }

    pthread_mutex_lock(&c->picture_lock);
    if (n > 64 - c->n_pending) {
        pthread_mutex_unlock(&c->picture_lock);
        rk_object_unref(&c->base);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    for (int i = 0; i < n; i++) {
        c->pending[c->n_pending++] = buffers[i];
        /* Snapshot picture-wide state immediately: applications may destroy
         * the VA buffers as soon as RenderPicture returns. */
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
        } else if (b && b->type == VAPictureParameterBufferType &&
                   c->coding == MPP_VIDEO_CodingHEVC &&
                   (size_t)b->size * b->num_elements >=
                       sizeof(VAPictureParameterBufferHEVC)) {
            memcpy(&c->last_hevc_pp, b->data,
                   sizeof(VAPictureParameterBufferHEVC));
            c->has_hevc_pp = true;
        } else if (b && b->type == VAIQMatrixBufferType &&
                   c->coding == MPP_VIDEO_CodingHEVC &&
                   (size_t)b->size * b->num_elements >=
                       sizeof(VAIQMatrixBufferHEVC)) {
            memcpy(&c->last_hevc_iq, b->data,
                   sizeof(VAIQMatrixBufferHEVC));
            c->has_hevc_iq = true;
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

    if (c->is_encoder) {
        pthread_mutex_lock(&c->picture_lock);
        if (c->render_target == VA_INVALID_SURFACE || !c->render_surface) {
            pthread_mutex_unlock(&c->picture_lock);
            rk_object_unref(&c->base);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        VAStatus status = rk_mpp_enc_encode(c);
        RKSurface *render_surface = c->render_surface;
        c->render_target = VA_INVALID_SURFACE;
        c->render_surface = NULL;
        c->has_enc_pic = false;
        c->has_enc_slice = false;
        pthread_mutex_unlock(&c->picture_lock);
        rk_object_unref(&render_surface->base);
        rk_object_unref(&c->base);
        return status;
    }

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
