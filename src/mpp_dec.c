#include "mpp_dec.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>

#include "convert.h"
#include "frame_layout.h"
#include "h264.h"
#include "hevc.h"
#include "vp9.h"

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

static bool write_all(int fd, const void *data, size_t size)
{
    const uint8_t *cursor = data;

    while (size > 0) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        cursor += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

/* Opt-in reducer hook. Each record is one complete Annex-B access unit in
 * submission order; PATH.sizes records the corresponding byte lengths. The
 * data-file lock preserves the same append order across frame threads and
 * processes. The replay parser rejects an incomplete or mismatched pair after
 * an I/O failure. Dump failures are diagnostic only and never change decode
 * behavior. */
static void dump_hevc_packet(const uint8_t *packet, size_t packet_size)
{
    const char *path = getenv("RK_VAAPI_HEVC_DUMP");
    if (!path || !*path || !packet || packet_size == 0)
        return;

    int data_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (data_fd < 0) {
        LOG_WARNING("HEVC dump: cannot open %s: %s", path, strerror(errno));
        return;
    }
    if (flock(data_fd, LOCK_EX) != 0) {
        LOG_WARNING("HEVC dump: cannot lock %s: %s", path, strerror(errno));
        close(data_fd);
        return;
    }

    bool data_ok = write_all(data_fd, packet, packet_size);
    size_t path_length = strlen(path);
    char *sizes_path = NULL;
    if (path_length <= SIZE_MAX - sizeof(".sizes")) {
        sizes_path = malloc(path_length + sizeof(".sizes"));
        if (sizes_path)
            snprintf(sizes_path, path_length + sizeof(".sizes"),
                     "%s.sizes", path);
    }

    bool sizes_ok = false;
    if (sizes_path) {
        int sizes_fd = open(sizes_path,
                            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (sizes_fd >= 0) {
            char line[64];
            int length = snprintf(line, sizeof(line), "%zu\n", packet_size);
            sizes_ok = length > 0 && (size_t)length < sizeof(line) &&
                       write_all(sizes_fd, line, (size_t)length);
            close(sizes_fd);
        }
    }
    if (!data_ok || !sizes_ok)
        LOG_WARNING("HEVC dump: stream/manifest append failed for %s", path);
    flock(data_fd, LOCK_UN);
    close(data_fd);
    free(sizes_path);
}

static int h264_profile_idc(VAProfile p) {
    switch (p) {
    case VAProfileH264ConstrainedBaseline: return 66;
    case VAProfileH264Main:                return 77;
    case VAProfileH264High:                return 100;
    case VAProfileH264High10:              return 110;
    default:                               return 100;
    }
}

static bool codec_uses_token_routes(const RKContext *c)
{
    return c->coding == MPP_VIDEO_CodingAVC ||
           c->coding == MPP_VIDEO_CodingHEVC;
}

/* The pool starts at the depth a single-threaded consumer needs and grows on
 * demand. It cannot be sized correctly up front: VA surfaces are created
 * independently of the context, FFmpeg passes no render targets to
 * vaCreateContext, and its surface count scales with decoder thread count.
 * A frame-threaded HEVC decode of a large DPB holds ~29 surfaces, which
 * deadlocked against a fixed 24-buffer pool -- MPP had no free output buffer,
 * every buffer was bound to a surface the application had not released, and
 * the application was waiting on the decode. Growing costs memory only when a
 * consumer actually holds that many frames. */
enum { EXTERNAL_POOL_COUNT = 24, EXTERNAL_POOL_MAX = 64 };

/* Allocate and commit one more backing buffer. Caller runs on the worker. */
static bool commit_pool_buffer(RKDecodePool *pool, int index)
{
    if (mpp_buffer_get(pool->backing_group, &pool->buffers[index],
                       pool->buffer_size) != MPP_OK) {
        LOG("external_group: backing buffer %d allocation failed", index);
        return false;
    }
    MppBufferInfo commit = {
        .type = MPP_BUFFER_TYPE_EXT_DMA,
        .size = mpp_buffer_get_size(pool->buffers[index]),
        .ptr = NULL,
        .hnd = NULL,
        .fd = mpp_buffer_get_fd(pool->buffers[index]),
        .index = index,
    };
    if (commit.fd < 0 ||
        mpp_buffer_commit(pool->frame_group, &commit) != MPP_OK) {
        LOG("external_group: commit buffer[%d] fd=%d size=%zu failed",
            index, commit.fd, commit.size);
        mpp_buffer_put(pool->buffers[index]);
        pool->buffers[index] = NULL;
        return false;
    }
    return true;
}

/* Add buffers to a live external group. Returns false when the pool is
 * already at its ceiling or the allocation failed, so the caller can report a
 * real error instead of stalling forever. */
static bool grow_external_group(RKContext *c)
{
    enum { EXTERNAL_POOL_GROW = 8 };
    RKDecodePool *pool = c->decode_pool;

    if (!pool || pool->count >= pool->capacity)
        return false;

    int target = pool->count + EXTERNAL_POOL_GROW;
    if (target > pool->capacity)
        target = pool->capacity;

    int added = 0;
    for (int i = pool->count; i < target; i++) {
        if (!commit_pool_buffer(pool, i))
            break;
        added++;
    }
    if (!added)
        return false;

    pool->count += added;
    LOG("external_group: grew to %d buffers (+%d)", pool->count, added);
    return true;
}

static bool configure_external_group(RKContext *c, MppFrame info_frame)
{
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
    pool->capacity = EXTERNAL_POOL_MAX;
    pool->buffer_size = allocation_size;

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

    pool->buffers = calloc((size_t)pool->capacity, sizeof(*pool->buffers));
    if (!pool->buffers)
        goto fail;

    for (int i = 0; i < EXTERNAL_POOL_COUNT; i++) {
        if (!commit_pool_buffer(pool, i))
            goto fail;
        pool->count++;
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

    if (codec_uses_token_routes(c)) {
        /* H.264 and HEVC can reorder display output. The worker assigns a unique PTS
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
    if (!codec_uses_token_routes(c) &&
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

    uint32_t errinfo = mpp_frame_get_errinfo(frame);
    uint32_t discard = mpp_frame_get_discard(frame);
    if (errinfo || discard) {
        LOG("assign_mpp_frame: surface=0x%x MPP reported err=0x%x discard=0x%x; decode failed",
            (unsigned)sid, errinfo, discard);
        mpp_frame_deinit(&frame);
        complete_surface_ref(s, route->fence, false);
        frame_route_destroy(route);
        return true;
    }

    MppBuffer      buf    = mpp_frame_get_buffer(frame);
    int            fwidth = (int)mpp_frame_get_width(frame);
    int            fheight= (int)mpp_frame_get_height(frame);
    int            fhs    = (int)mpp_frame_get_hor_stride(frame);
    int            fvs    = (int)mpp_frame_get_ver_stride(frame);
    int            fhs_pix= (int)mpp_frame_get_hor_stride_pixel(frame);
    int            fbc_hs = (int)mpp_frame_get_fbc_hdr_stride(frame);
    int            offset_x = (int)mpp_frame_get_offset_x(frame);
    int            offset_y = (int)mpp_frame_get_offset_y(frame);
    MppFrameFormat ffmt   = mpp_frame_get_fmt(frame);

    int  frame_w = fwidth  > 0 ? fwidth  : surface_width;
    int  frame_h = fheight > 0 ? fheight : surface_height;
    int  src_hs = fhs > 0 ? fhs : frame_w;
    int  src_vs = fvs > 0 ? fvs : frame_h;
    size_t src_size = buf ? mpp_buffer_get_size(buf) : 0;
    size_t layout_size = 0;
    bool is_afbc = MPP_FRAME_FMT_IS_AFBC(ffmt);
    bool nv12 = (ffmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP;
    bool nv15 = (ffmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV420SP_10BIT;
    bool linear_nv12 = nv12 && !is_afbc;
    bool linear_nv15 = nv15 && !is_afbc;
    bool afbc_nv15 = nv15 && is_afbc;
    int src_hs_pixels = afbc_nv15 && fbc_hs > 0 ? fbc_hs : fhs_pix;
    if (linear_nv15 && src_hs > 0 && (src_hs % 5) == 0)
        src_hs_pixels = src_hs / 5 * 4;

    bool linear_layout_valid = (linear_nv12 || linear_nv15) &&
        src_hs > 0 && src_vs >= frame_h &&
        rk_nv12_layout_size((size_t)src_hs, (size_t)src_vs, &layout_size) &&
        src_size >= layout_size;
    if (linear_nv15)
        linear_layout_valid = linear_layout_valid &&
            src_hs_pixels >= frame_w && (src_hs_pixels % 64) == 0 &&
            (fhs_pix <= 0 || fhs_pix == src_hs_pixels);
    bool afbc_layout_valid = afbc_nv15 && src_hs_pixels >= frame_w &&
        (src_hs_pixels % 64) == 0 && src_vs >= frame_h &&
        (src_vs % 16) == 0 && offset_x >= 0 && offset_y >= 0 &&
        offset_x <= src_hs_pixels && frame_w <= src_hs_pixels - offset_x &&
        offset_y <= src_vs && frame_h <= src_vs - offset_y && src_size > 0;
    bool layout_valid = linear_layout_valid || afbc_layout_valid;
    int pool_index = -1;
    bool pool_match = external_buffer_matches_pool(c, buf, &pool_index);
    RKDecodePool *pool = c->decode_pool;
    bool external_ready = pool != NULL;
    bool usable = layout_valid && (!external_ready || pool_match);
    bool converted_10bit = false;
    MppBuffer backing = NULL;
    MppFrame stored_frame = frame;
    int output_hstride = src_hs;

    if (usable && nv15) {
        MppBuffer converted = NULL;
        usable = external_ready &&
            rk_convert_nv15_to_p010(pool->backing_group, buf,
                                    (uint32_t)frame_w, (uint32_t)frame_h,
                                    (uint32_t)src_hs,
                                    (uint32_t)src_hs_pixels,
                                    (uint32_t)src_vs, (uint32_t)offset_x,
                                    (uint32_t)offset_y, afbc_nv15, &converted);
        if (usable) {
            converted_10bit = true;
            backing = converted;
            stored_frame = NULL;
            output_hstride = src_hs_pixels;
        }
    } else if (usable && external_ready) {
        backing = pool->buffers[pool_index];
        if (backing && mpp_buffer_inc_ref(backing) != MPP_OK)
            usable = false;
    }
    if (usable && pool && !rk_object_ref(&pool->base)) {
        if (backing)
            mpp_buffer_put(backing);
        backing = NULL;
        usable = false;
    }

    if (usable) {
        pthread_mutex_lock(&s->lock);
        uint64_t current_fence = s->fence;
        if (current_fence != route->fence) {
            pthread_mutex_unlock(&s->lock);
            LOG("assign_mpp_frame: output canceled surface=0x%x "
                "route_fence=%llu current_fence=%llu converted_10bit=%d "
                "external=%d",
                (unsigned)sid, (unsigned long long)route->fence,
                (unsigned long long)current_fence, converted_10bit,
                external_ready);
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
        s->frame = stored_frame;
        s->backing_buf = backing;
        s->decode_pool = pool;
        s->fmt = ffmt;
        if (fwidth  > 0) s->width   = fwidth;
        if (fheight > 0) s->height  = fheight;
        if (output_hstride > 0) s->hstride = output_hstride;
        if (fvs     > 0) s->vstride = fvs;
        s->decoded = true;
        s->decode_failed = false;
        s->h264_field_pending = false;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        if (old_frame)
            mpp_frame_deinit(&old_frame);
        if (converted_10bit)
            mpp_frame_deinit(&frame);
        if (old_backing)
            mpp_buffer_put(old_backing);
        if (old_pool)
            rk_object_unref(&old_pool->base);
        LOG("assign_mpp_frame: surface=0x%x MPP %dx%d stride=%dx%d "
            "fmt=0x%x zero_copy=%d converted_10bit=%d external=%d "
            "pool_index=%d fd=%d fence=%llu",
            (unsigned)sid, fwidth, fheight, output_hstride, fvs,
            (unsigned)ffmt, !converted_10bit, converted_10bit,
            external_ready, pool_index,
            backing ? mpp_buffer_get_fd(backing) : mpp_buffer_get_fd(buf),
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
static int64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/* True once the worker must stop pushing work at MPP: either a hard stop, or
 * a teardown drain that has run past its deadline. The deadline keeps
 * vaDestroyContext bounded when the backend has stopped making progress. */
static bool decode_worker_stopping(RKContext *c)
{
    pthread_mutex_lock(&c->work_lock);
    bool stopping = c->worker_stop ||
                    (c->worker_drain && monotonic_ns() > c->drain_deadline_ns);
    pthread_mutex_unlock(&c->work_lock);
    return stopping;
}

static bool drain_output_frame(RKContext *c)
{
    MppFrame frame = NULL;
    MPP_RET ret = c->mpi->decode_get_frame(c->mpp, &frame);
    if (ret == MPP_OK && frame) {
        /* MPP marks the last frame after an end-of-stream packet. Reading it
         * is the only positive signal that nothing more is coming; without it
         * a teardown drain can only wait out its deadline. */
        if (mpp_frame_get_eos(frame))
            c->saw_eos = true;
        if (assign_mpp_frame(frame, c) && c->outstanding_frames > 0)
            c->outstanding_frames--;
        return true;
    }
    if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT)
        LOG("decode worker: output wait failed: %d", ret);
    return false;
}

static MPP_RET put_packet_draining(RKContext *c, MppPacket pkt)
{
    /* Consecutive rejections with nothing to drain that mean MPP is starved of
     * output buffers rather than merely busy. At the 20 ms output timeout this
     * is ~0.5 s of no progress before the pool is grown. */
    enum { STARVED_TRIES = 25 };
    MPP_RET ret = MPP_OK;
    int starved = 0;

    for (int tries = 0; tries < 500; tries++) {
        if (decode_worker_stopping(c))
            return MPP_NOK;
        ret = c->mpi->decode_put_packet(c->mpp, pkt);
        if (ret == MPP_OK) return MPP_OK;
        if (drain_output_frame(c)) {
            starved = 0;
            continue;
        }
        /* Every pool buffer is bound to a surface the application still holds,
         * so no drain can free one. Only more buffers break the deadlock. */
        if (++starved >= STARVED_TRIES) {
            starved = 0;
            if (!grow_external_group(c)) {
                LOG_WARNING("decode worker: output pool exhausted at %d "
                            "buffers; consumer holds every frame",
                            c->decode_pool ? c->decode_pool->count : 0);
                return ret;
            }
        }
    }
    return ret;
}

static void drain_hevc_after_submit(RKContext *c)
{
    if (c->coding == MPP_VIDEO_CodingHEVC && c->outstanding_frames > 0)
        (void)drain_output_frame(c);
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
                               h264_profile_idc(c->profile));
            if (n <= 0) {
                LOG("do_h264_decode: SPS reconstruction failed");
                free(pkt_data);
                return VA_STATUS_ERROR_DECODING_ERROR;
            }
            PKT_APPEND(hdr, (size_t)n);
            c->sps_sent = true;
        }
        n = h264_write_pps(hdr, sizeof(hdr), &c->last_pp,
                           c->has_iq && h264_profile_idc(c->profile) >= 100
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

static bool has_annex_b_prefix(const uint8_t *data, size_t size)
{
    return data &&
           ((size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
            (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
             data[3] == 1));
}

/* MPP consumes complete Annex B access units, while VA-API supplies the
 * active HEVC parameter state and bare slice NAL units independently. */
static VAStatus build_hevc_job(RKContext *c, RKDriver *d,
                               RKDecodeJob **job_out)
{
    enum {
        HEVC_HEADER_CAPACITY = 65536,
        HEVC_SEQUENCE_HEADER_CAPACITY = 4096,
    };
    static const uint8_t start_code[4] = { 0, 0, 0, 1 };
    uint8_t sequence_headers[HEVC_SEQUENCE_HEADER_CAPACITY];
    RKHEVCSliceInfo slice_info;
    bool found_slice = false;

    if (!c->has_hevc_pp) {
        LOG("build_hevc_job: missing picture parameters");
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_acquire(d, c->pending[i]);
        if (!b)
            continue;
        if (b->type == VASliceDataBufferType && b->capacity > 0) {
            found_slice = rk_hevc_parse_slice_info(b->data, b->capacity,
                                                   &slice_info);
            rk_object_unref(&b->base);
            break;
        }
        rk_object_unref(&b->base);
    }
    if (!found_slice) {
        LOG("build_hevc_job: missing or malformed slice NAL");
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    uint8_t *packet = NULL;
    size_t packet_size = 0;
    size_t packet_capacity = 0;
    VAStatus status = VA_STATUS_SUCCESS;

    uint8_t *headers = malloc(HEVC_HEADER_CAPACITY);
    if (!headers)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    int profile_idc = c->profile == VAProfileHEVCMain10 ? 2 : 1;
    const VAIQMatrixBufferHEVC *iq =
        c->has_hevc_iq ? &c->last_hevc_iq : NULL;
    int sequence_size = rk_hevc_write_sequence_parameter_sets(
        sequence_headers, sizeof(sequence_headers), &c->last_hevc_pp,
        iq, profile_idc);
    if (sequence_size <= 0) {
        LOG("build_hevc_job: sequence parameter-set reconstruction failed");
        free(headers);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    pthread_mutex_lock(&c->work_lock);
    bool sequence_changed =
        c->hevc_sequence_headers_size != (size_t)sequence_size ||
        !c->hevc_sequence_headers ||
        memcmp(c->hevc_sequence_headers, sequence_headers,
               (size_t)sequence_size) != 0;
    pthread_mutex_unlock(&c->work_lock);
    size_t header_size = 0;
    if (sequence_changed) {
        memcpy(headers, sequence_headers, (size_t)sequence_size);
        header_size = (size_t)sequence_size;
    }
    int pps_size = rk_hevc_write_picture_parameter_set(
        headers + header_size, HEVC_HEADER_CAPACITY - header_size,
        &c->last_hevc_pp, iq, slice_info.pps_id, profile_idc);
    if (pps_size <= 0) {
        LOG("build_hevc_job: picture parameter-set reconstruction failed");
        free(headers);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    header_size += (size_t)pps_size;

    status = packet_append(&packet, &packet_size, &packet_capacity,
                           headers, header_size);
    free(headers);
    if (status != VA_STATUS_SUCCESS) {
        free(packet);
        return status;
    }
    unsigned int slice_count = 0;
    VASliceParameterBufferHEVC last_slice_param;
    bool have_slice_param = false;
    for (int i = 0; i < c->n_pending; i++) {
        RKBuffer *b = buffer_acquire(d, c->pending[i]);
        if (!b)
            continue;
        if (b->type == VASliceParameterBufferType &&
            b->capacity >= sizeof(last_slice_param)) {
            memcpy(&last_slice_param, b->data, sizeof(last_slice_param));
            have_slice_param = true;
            rk_object_unref(&b->base);
            continue;
        }
        if (b->type != VASliceDataBufferType || b->capacity == 0) {
            rk_object_unref(&b->base);
            continue;
        }
        uint8_t *rewritten = malloc(b->capacity + 4096);
        if (!rewritten) {
            rk_object_unref(&b->base);
            free(packet);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        int rewritten_size = rk_hevc_rewrite_slice_nal(
            rewritten, b->capacity + 4096, b->data, b->capacity,
            &c->last_hevc_pp, have_slice_param ? &last_slice_param : NULL);
        if (rewritten_size < 0) {
            LOG("build_hevc_job: slice RPS rewrite failed");
            free(rewritten);
            rk_object_unref(&b->base);
            free(packet);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        const uint8_t *data = rewritten;
        size_t data_size = (size_t)rewritten_size;
        if (!has_annex_b_prefix(data, data_size))
            status = packet_append(&packet, &packet_size, &packet_capacity,
                                   start_code, sizeof(start_code));
        if (status == VA_STATUS_SUCCESS)
            status = packet_append(&packet, &packet_size, &packet_capacity,
                                   data, data_size);
        free(rewritten);
        rk_object_unref(&b->base);
        if (status != VA_STATUS_SUCCESS) {
            free(packet);
            return status;
        }
        slice_count++;
    }
    if (slice_count == 0) {
        free(packet);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    RKDecodeJob *job = decode_job_create(c, packet, packet_size);
    if (!job) {
        free(packet);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (sequence_changed) {
        uint8_t *cached = malloc((size_t)sequence_size);
        if (!cached) {
            rk_mpp_dec_job_destroy(job);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        memcpy(cached, sequence_headers, (size_t)sequence_size);
        job->hevc_sequence_headers = cached;
        job->hevc_sequence_headers_size = (size_t)sequence_size;
    }
    LOG("build_hevc_job: queued %zu bytes in %u slice(s) target=0x%x fence=%llu",
        packet_size, slice_count, (unsigned)job->target,
        (unsigned long long)job->fence);
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
    uint8_t vp9_profile = c->profile == VAProfileVP9Profile2 ? 2 : 0;
    if (pkt_sz && is_vp9 &&
        !rk_vp9_parse_frame(pkt_data, pkt_sz, vp9_profile, &vp9_info)) {
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

void rk_mpp_dec_job_destroy(RKDecodeJob *job)
{
    if (!job)
        return;
    free(job->data);
    free(job->hevc_sequence_headers);
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

    if (c->coding == MPP_VIDEO_CodingHEVC)
        dump_hevc_packet(job->data, job->size);

    if (codec_uses_token_routes(c)) {
        bool paired_field = c->coding == MPP_VIDEO_CodingAVC &&
                            job->h264_field;
        RKFrameRoute *route = paired_field
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
            LOG("decode worker: token-routed packet submission failed: %d", ret);
            complete_surface_ref(job->surface, job->fence, false);
            return;
        }
        if (new_route) {
            c->outstanding_frames++;
            drain_hevc_after_submit(c);
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
            drain_hevc_after_submit(c);
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
        uint8_t profile = c->profile == VAProfileVP9Profile2 ? 2 : 0;
        if (!route ||
            !rk_vp9_make_show_existing(profile, job->repeat_slot,
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
        drain_hevc_after_submit(c);
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
    drain_hevc_after_submit(c);
}

static bool worker_drain_one(RKContext *c)
{
    return drain_output_frame(c);
}

/* Tell MPP no more input is coming. Codecs without immediate-output -- H.264
 * and VP9 here -- hold decoded pictures for display reordering and will not
 * release the last few without this, so a teardown drain would wait out its
 * deadline and fail fences whose frames were already decoded. */
static void worker_signal_eos(RKContext *c)
{
    uint8_t stub = 0;
    MppPacket packet = NULL;

    if (mpp_packet_init(&packet, &stub, sizeof(stub)) != MPP_OK)
        return;
    mpp_packet_set_pos(packet, &stub);
    mpp_packet_set_length(packet, 0);
    mpp_packet_set_eos(packet);
    MPP_RET ret = MPP_NOK;
    for (int tries = 0; tries < 50; tries++) {
        ret = c->mpi->decode_put_packet(c->mpp, packet);
        if (ret == MPP_OK)
            break;
        if (!drain_output_frame(c) && monotonic_ns() > c->drain_deadline_ns)
            break;
    }
    LOG("decode worker: end-of-stream marker put ret=%d outstanding=%u",
        ret, c->outstanding_frames);
    mpp_packet_deinit(&packet);
}

/* Wall time a teardown drain may spend waiting on MPP before the remaining
 * fences are failed. With immediate output a healthy backend returns finished
 * pictures in milliseconds, so this only has to cover a hiccup -- it must not
 * become a second of latency on every vaDestroyContext. */
#define WORKER_DRAIN_TIMEOUT_NS ((int64_t)1000000000)

/* Wall time the worker tolerates with frames outstanding and no output at all
 * before it declares every pending decode failed. vaSyncSurface has no
 * timeout, so a backend that stops responding would otherwise hang the calling
 * media process forever. Failing the fences turns that into a real VAStatus
 * the application can fall back from. Measured on the FATE conformance
 * candidates MPP cannot decode -- NUT_A, OPFLAG, NoOutPrior and friends --
 * where the process previously had to be killed. */
#define WORKER_STALL_TIMEOUT_NS ((int64_t)10000000000)

/* Fail every route the worker still owns. Worker-thread context only: routes
 * are created and consumed there, and rk_mpp_dec_stop touches them after the
 * join. */
static unsigned int fail_pending_routes(RKContext *c)
{
    unsigned int failed = 0;

    RKFrameRoute *route = c->h264_routes;
    c->h264_routes = NULL;
    while (route) {
        RKFrameRoute *next = route->next;
        rk_mpp_dec_fail_surface(route->surface, route->fence);
        frame_route_destroy(route);
        route = next;
        failed++;
    }

    route = c->generic_head;
    c->generic_head = NULL;
    c->generic_tail = NULL;
    while (route) {
        RKFrameRoute *next = route->next;
        rk_mpp_dec_fail_surface(route->surface, route->fence);
        frame_route_destroy(route);
        route = next;
        failed++;
    }

    c->outstanding_frames = 0;
    return failed;
}

void *rk_mpp_dec_worker_main(void *opaque)
{
    RKContext *c = opaque;
    int64_t stalled_since_ns = 0;
    bool signalled_eos = false;
    LOG("decode worker: started coding=%d", (int)c->coding);

    for (;;) {
        pthread_mutex_lock(&c->work_lock);
        while (!c->worker_stop && !c->worker_drain && !c->job_head &&
               c->outstanding_frames == 0)
            pthread_cond_wait(&c->work_cond, &c->work_lock);

        if (c->worker_stop) {
            pthread_mutex_unlock(&c->work_lock);
            break;
        }

        /* VA surfaces outlive the context that decoded into them, so a
         * teardown drains submitted work instead of cancelling it. Applications
         * legitimately destroy a decode context on a sequence change and then
         * sync surfaces the old context was still filling. */
        bool draining = c->worker_drain;
        bool drain_expired = draining &&
                             monotonic_ns() > c->drain_deadline_ns;
        if (draining && (drain_expired || c->saw_eos ||
                         (!c->job_head && c->outstanding_frames == 0))) {
            unsigned int stranded = c->outstanding_frames;
            bool ended = c->saw_eos;
            pthread_mutex_unlock(&c->work_lock);
            /* Not necessarily a fault: a player that stops mid-stream
             * destroys its context with pictures it never intends to consume,
             * and MPP will not produce them without more input. Their fences
             * are failed below, so a caller that does still want them gets a
             * real VAStatus rather than a wait that never ends. */
            if (drain_expired)
                LOG("decode worker: teardown drain ended with %u frame(s) the "
                    "backend had not produced", stranded);
            else if (ended && stranded)
                LOG("decode worker: backend ended the stream with %u frames "
                    "it never produced", stranded);
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

        /* Every queued packet has now been submitted, so this is the first
         * point at which an end-of-stream marker is correct. */
        if (draining && !job && !signalled_eos) {
            signalled_eos = true;
            worker_signal_eos(c);
        }

        if (job) {
            worker_submit_job(c, job);
            rk_mpp_dec_job_destroy(job);
            stalled_since_ns = 0;
        } else if (worker_drain_one(c)) {
            stalled_since_ns = 0;
        } else if (c->outstanding_frames > 0 && !draining) {
            int64_t now = monotonic_ns();
            if (!stalled_since_ns) {
                stalled_since_ns = now;
            } else if (now - stalled_since_ns > WORKER_STALL_TIMEOUT_NS) {
                unsigned int failed = fail_pending_routes(c);
                LOG_WARNING("decode worker: backend produced no output for "
                            "%llds; failed %u outstanding decode(s)",
                            (long long)(WORKER_STALL_TIMEOUT_NS / 1000000000),
                            failed);
                stalled_since_ns = 0;
            }
        }
    }

    LOG("decode worker: stopped coding=%d", (int)c->coding);
    return NULL;
}

bool rk_mpp_dec_enqueue_job(RKContext *c, RKDecodeJob *job)
{
    pthread_mutex_lock(&c->work_lock);
    if (c->worker_stop) {
        pthread_mutex_unlock(&c->work_lock);
        return false;
    }
    if (job->hevc_sequence_headers) {
        free(c->hevc_sequence_headers);
        c->hevc_sequence_headers = job->hevc_sequence_headers;
        c->hevc_sequence_headers_size = job->hevc_sequence_headers_size;
        job->hevc_sequence_headers = NULL;
        job->hevc_sequence_headers_size = 0;
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

void rk_mpp_dec_fail_surface(RKSurface *surface, uint64_t fence)
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

void rk_mpp_dec_stop(RKContext *c)
{
    if (!c->sync_initialized)
        return;

    bool had_worker = c->worker_started;
    if (c->worker_started) {
        /* Let the worker finish what the application already submitted; only
         * then stop it. Anything still queued after the bounded drain is
         * failed below, so no fence is left unsignalled either way. */
        pthread_mutex_lock(&c->work_lock);
        c->drain_deadline_ns = monotonic_ns() + WORKER_DRAIN_TIMEOUT_NS;
        c->worker_drain = true;
        pthread_cond_broadcast(&c->work_cond);
        pthread_mutex_unlock(&c->work_lock);
        pthread_join(c->worker, NULL);
        pthread_mutex_lock(&c->work_lock);
        c->worker_stop = true;
        pthread_mutex_unlock(&c->work_lock);
        c->worker_started = false;
    }

    RKDecodeJob *job = c->job_head;
    c->job_head = NULL;
    c->job_tail = NULL;
    while (job) {
        RKDecodeJob *next = job->next;
        rk_mpp_dec_fail_surface(job->surface, job->fence);
        rk_mpp_dec_job_destroy(job);
        job = next;
    }

    (void)fail_pending_routes(c);

    pthread_mutex_lock(&c->picture_lock);
    RKSurface *render_surface = c->render_surface;
    uint64_t fence = c->render_fence;
    c->render_target = VA_INVALID_SURFACE;
    c->render_surface = NULL;
    c->render_fence = 0;
    pthread_mutex_unlock(&c->picture_lock);
    if (render_surface) {
        rk_mpp_dec_fail_surface(render_surface, fence);
        rk_object_unref(&render_surface->base);
    }

    if (had_worker && c->mpp && c->mpi && c->mpi->reset) {
        MPP_RET ret = c->mpi->reset(c->mpp);
        if (ret != MPP_OK)
            LOG("decode worker: mpp reset during stop failed: %d", ret);
    }
}

VAStatus rk_mpp_dec_build_job(RKContext *context, RKDriver *driver,
                              RKDecodeJob **job)
{
    if (context->coding == MPP_VIDEO_CodingAVC)
        return build_h264_job(context, driver, job);
    if (context->coding == MPP_VIDEO_CodingHEVC)
        return build_hevc_job(context, driver, job);
    return build_generic_job(context, driver, job);
}

void rk_mpp_dec_reject_job(RKDecodeJob *job)
{
    if (!job)
        return;
    complete_surface_ref(job->surface, job->fence, false);
    rk_mpp_dec_job_destroy(job);
}
