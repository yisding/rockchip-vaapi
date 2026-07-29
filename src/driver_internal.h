#ifndef RK_VAAPI_DRIVER_INTERNAL_H
#define RK_VAAPI_DRIVER_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <va/va_backend.h>
#include <va/va_dec_hevc.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include "log.h"
#include "object_heap.h"

#define RK_MAX_WIDTH 7680
#define RK_MAX_HEIGHT 4320

typedef struct {
    RKObjectBase base;
    VAProfile profile;
    VAEntrypoint entrypoint;
    uint32_t rt_format;
    uint32_t rate_control;
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
    uint8_t *hevc_sequence_headers;
    size_t hevc_sequence_headers_size;
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
    int count;                  /* committed to MPP so far */
    int capacity;               /* entries in buffers[] */
    size_t buffer_size;
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
    VAEntrypoint entrypoint;
    uint32_t rate_control;
    bool is_encoder;

    RKSurface **targets;
    int n_targets;
    RKDecodePool *decode_pool;

    pthread_mutex_t picture_lock;
    pthread_mutex_t work_lock;
    pthread_cond_t work_cond;
    pthread_t worker;
    RKDecodeJob *job_head;
    RKDecodeJob *job_tail;
    RKFrameRoute *h264_routes;
    RKFrameRoute *generic_head;
    RKFrameRoute *generic_tail;
    uint64_t next_token;
    int64_t drain_deadline_ns;
    unsigned int outstanding_frames;
    bool sync_initialized;
    bool worker_started;
    bool worker_stop;
    bool worker_drain;
    bool saw_eos;

    /* Grown on demand: HEVC conformance streams such as CAINIT_G carry more
     * than a hundred slices in one picture, and a fixed ceiling would turn a
     * legal stream into a decode failure. */
    VABufferID *pending;
    RKSurface *render_surface;
    uint64_t render_fence;
    int n_pending;
    int pending_capacity;
    VASurfaceID render_target;

    VAPictureParameterBufferH264 last_pp;
    VAIQMatrixBufferH264 last_iq;
    bool has_iq;
    bool sps_sent;

    VAPictureParameterBufferHEVC last_hevc_pp;
    VAIQMatrixBufferHEVC last_hevc_iq;
    bool has_hevc_pp;
    bool has_hevc_iq;
    uint8_t *hevc_sequence_headers;
    size_t hevc_sequence_headers_size;

    MppEncCfg enc_cfg;
    VAEncSequenceParameterBufferH264 enc_seq;
    VAEncPictureParameterBufferH264 enc_pic;
    VAEncSliceParameterBufferH264 enc_slice;
    VAEncSequenceParameterBufferHEVC enc_hevc_seq;
    VAEncPictureParameterBufferHEVC enc_hevc_pic;
    VAEncSliceParameterBufferHEVC enc_hevc_slice;
    bool has_enc_seq;
    bool has_enc_pic;
    bool has_enc_slice;
    uint32_t enc_bitrate;
    uint32_t enc_fps_num;
    uint32_t enc_fps_den;
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

    MppBuffer import_buf;
    int import_fd;
    size_t import_size;
    uint32_t import_pitch;
    uint32_t import_drm_format;
    bool imported_rgb;
    /* The application declared this surface as encoder input. Its content is
     * written through the checked vaPutImage path, never read back as decoded
     * pixels, so it must not be exposed through vaDeriveImage. */
    bool encoder_input;

    MppFrameFormat fmt;
    uint32_t fourcc;
    bool decoded;
    bool decode_failed;
    bool h264_field_pending;
    VAContextID ctx_id;
    uint64_t fence;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

typedef struct RKBuffer {
    RKObjectBase base;
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    size_t capacity;
    void *data;
    VACodedBufferSegment coded_segment;
    bool coded_ready;
    bool coded_failed;

    /* Set only on the buffer vaDeriveImage hands back. It aliases a surface's
     * DMA-BUF instead of owning heap memory, so map/unmap go through mmap with
     * CPU access brackets and vaAcquireBufferHandle can export the fd. */
    RKSurface *derived_surface;
    size_t derived_size;
    size_t derived_chroma_offset;
    void *derived_map;
    unsigned int derived_pitch;
    int derived_fd;
    int acquired_fd;
} RKBuffer;

typedef struct {
    RKObjectBase base;
    VABufferID buffer_id;
    RKBuffer *buffer;
    uint32_t fourcc;
    unsigned int width;
    unsigned int height;
    unsigned int num_planes;
    unsigned int pitches[3];
    unsigned int offsets[3];
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
