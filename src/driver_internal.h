#ifndef RK_VAAPI_DRIVER_INTERNAL_H
#define RK_VAAPI_DRIVER_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/rk_mpi.h>
#include <va/va_backend.h>

#include "log.h"
#include "object_heap.h"

typedef struct {
    RKObjectBase base;
    VAProfile profile;
    VAEntrypoint entrypoint;
} RKConfig;

typedef struct RKSurface RKSurface;
typedef struct RKDriver RKDriver;

typedef struct RKDecodeJob {
    struct RKDecodeJob *next;
    uint8_t *data;
    size_t size;
    VASurfaceID target;
    RKSurface *surface;
    uint64_t fence;
    uint64_t token;
    bool h264_field;
    bool is_hidden;
    uint8_t repeat_slot;
} RKDecodeJob;

typedef struct RKFrameRoute {
    struct RKFrameRoute *next;
    VASurfaceID target;
    RKSurface *surface;
    uint64_t fence;
    uint64_t token;
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
    RKDriver *driver;
    VAProfile profile;
    int width;
    int height;

    MppCtx mpp;
    MppApi *mpi;
    MppCodingType coding;

    RKSurface **targets;
    int n_targets;
    RKDecodePool *decode_pool;

    pthread_mutex_t picture_lock;
    pthread_mutex_t work_lock;
    pthread_cond_t work_cond;
    pthread_t worker;
    bool sync_initialized;
    bool worker_started;
    bool worker_stop;
    RKDecodeJob *job_head;
    RKDecodeJob *job_tail;
    RKFrameRoute *h264_routes;
    RKFrameRoute *generic_head;
    RKFrameRoute *generic_tail;
    unsigned int outstanding_frames;
    uint64_t next_token;

    VABufferID pending[64];
    int n_pending;

    VASurfaceID render_target;
    RKSurface *render_surface;
    uint64_t render_fence;

    VAPictureParameterBufferH264 last_pp;
    VAIQMatrixBufferH264 last_iq;
    bool has_iq;
    bool sps_sent;
} RKContext;

struct RKSurface {
    RKObjectBase base;
    int width;
    int height;

    MppFrame frame;
    MppBuffer backing_buf;
    RKDecodePool *decode_pool;
    int hstride;
    int vstride;

    MppBufferGroup priv_group;
    MppBuffer priv_buf;

    MppFrameFormat fmt;
    bool decoded;
    bool decode_failed;
    bool h264_field_pending;
    VAContextID ctx_id;
    uint64_t fence;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

typedef struct {
    RKObjectBase base;
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    size_t capacity;
    void *data;
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

static inline RKDriver *drv_from_ctx(VADriverContextP context)
{
    return (RKDriver *)context->pDriverData;
}

static inline RKConfig *config_acquire(RKDriver *driver, VAConfigID id)
{
    pthread_mutex_lock(&driver->object_lock);
    RKConfig *config = (RKConfig *)rk_object_heap_acquire(
        &driver->config_heap, (uint32_t)id);
    pthread_mutex_unlock(&driver->object_lock);
    return config;
}

static inline RKContext *context_acquire(RKDriver *driver, VAContextID id)
{
    pthread_mutex_lock(&driver->object_lock);
    RKContext *context = (RKContext *)rk_object_heap_acquire(
        &driver->context_heap, (uint32_t)id);
    pthread_mutex_unlock(&driver->object_lock);
    return context;
}

static inline RKSurface *surface_acquire(RKDriver *driver, VASurfaceID id)
{
    pthread_mutex_lock(&driver->object_lock);
    RKSurface *surface = (RKSurface *)rk_object_heap_acquire(
        &driver->surface_heap, (uint32_t)id);
    pthread_mutex_unlock(&driver->object_lock);
    return surface;
}

static inline RKBuffer *buffer_acquire(RKDriver *driver, VABufferID id)
{
    pthread_mutex_lock(&driver->object_lock);
    RKBuffer *buffer = (RKBuffer *)rk_object_heap_acquire(
        &driver->buffer_heap, (uint32_t)id);
    pthread_mutex_unlock(&driver->object_lock);
    return buffer;
}

static inline RKImage *image_acquire(RKDriver *driver, VAImageID id)
{
    pthread_mutex_lock(&driver->object_lock);
    RKImage *image = (RKImage *)rk_object_heap_acquire(
        &driver->image_heap, (uint32_t)id);
    pthread_mutex_unlock(&driver->object_lock);
    return image;
}

#endif
