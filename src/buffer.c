#include "buffer.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <va/va_drmcommon.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver_internal.h"
#include "frame_layout.h"
#include "surface.h"

static void buffer_destroy(void *opaque)
{
    RKBuffer *buffer = opaque;
    if (buffer->derived_map)
        munmap(buffer->derived_map, buffer->derived_size);
    if (buffer->acquired_fd >= 0)
        close(buffer->acquired_fd);
    if (buffer->derived_surface)
        rk_object_unref(&buffer->derived_surface->base);
    free(buffer->data);
    free(buffer);
}

static bool dmabuf_cpu_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    int ret;
    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && errno == EINTR);
    return ret == 0;
}

static void image_destroy(void *opaque)
{
    RKImage *image = opaque;
    if (image->buffer)
        rk_object_unref(&image->buffer->base);
    free(image);
}

/* Resolve the buffer a surface is currently bound to, together with the layout
 * that buffer implies. Callers use this both to describe a derived image and to
 * re-check that the description still holds. */
static bool surface_linear_layout(RKSurface *surface, int *fd_out,
                                  size_t *object_size_out,
                                  unsigned int *pitch_out,
                                  size_t *chroma_offset_out)
{
    pthread_mutex_lock(&surface->lock);
    MppBuffer active = surface->import_buf ? surface->import_buf
                     : surface->backing_buf ? surface->backing_buf
                     : surface->frame ? mpp_frame_get_buffer(surface->frame)
                     : surface->priv_buf;
    int fd = active ? mpp_buffer_get_fd(active) : -1;
    size_t object_size = active ? mpp_buffer_get_size(active) : 0;
    unsigned int hstride = surface->hstride ? (unsigned int)surface->hstride
                                            : (unsigned int)surface->width;
    unsigned int vstride = surface->vstride ? (unsigned int)surface->vstride
                                            : (unsigned int)surface->height;
    bool ten_bit = MPP_FRAME_FMT_IS_YUV_10BIT(surface->fmt);
    pthread_mutex_unlock(&surface->lock);

    if (fd < 0 || object_size == 0)
        return false;

    /* hstride is a pixel stride, so P010's byte pitch is twice it. This is the
     * same convention vaExportSurfaceHandle uses for its layer pitches. */
    unsigned int pitch = ten_bit ? hstride * 2 : hstride;
    size_t chroma_offset = (size_t)pitch * vstride;
    if (chroma_offset + chroma_offset / 2 > object_size)
        return false;

    *fd_out = fd;
    *object_size_out = object_size;
    *pitch_out = pitch;
    *chroma_offset_out = chroma_offset;
    return true;
}

/* A derived image fixes its pitches, but decode can rebind a surface to a
 * different buffer with a different stride. Re-resolve before every use and
 * refuse when the description no longer matches, so a consumer that derived
 * during pool setup never reads pixels through the wrong geometry. */
static bool derived_still_valid(RKBuffer *buffer)
{
    int fd = -1;
    size_t object_size = 0;
    unsigned int pitch = 0;
    size_t chroma_offset = 0;

    if (!surface_linear_layout(buffer->derived_surface, &fd, &object_size,
                               &pitch, &chroma_offset)) {
        LOG("derived image: surface no longer has a linear layout");
        return false;
    }
    if (pitch != buffer->derived_pitch ||
        chroma_offset != buffer->derived_chroma_offset) {
        LOG("derived image: layout changed under it "
            "(pitch %u->%u chroma_offset %zu->%zu)",
            buffer->derived_pitch, pitch, buffer->derived_chroma_offset,
            chroma_offset);
        return false;
    }
    if (fd != buffer->derived_fd) {
        /* Same geometry, different buffer: drop the stale mapping and let the
         * caller map the buffer the surface actually holds now. */
        if (buffer->derived_map) {
            munmap(buffer->derived_map, buffer->derived_size);
            buffer->derived_map = NULL;
        }
        if (buffer->acquired_fd >= 0) {
            close(buffer->acquired_fd);
            buffer->acquired_fd = -1;
        }
        buffer->derived_fd = fd;
        buffer->derived_size = object_size;
    }
    return true;
}

VAStatus rk_CreateBuffer(VADriverContextP context, VAContextID context_id,
                         VABufferType type, unsigned int size,
                         unsigned int num_elements, void *data,
                         VABufferID *buffer_id)
{
    RKDriver *driver = drv_from_ctx(context);
    (void)context_id;

    if (size != 0 && num_elements > SIZE_MAX / size)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    size_t bytes = (size_t)size * num_elements;

    RKBuffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    rk_object_init(&buffer->base, buffer_destroy);
    buffer->type = type;
    buffer->size = size;
    buffer->num_elements = num_elements;
    buffer->capacity = bytes;
    buffer->derived_fd = -1;
    buffer->acquired_fd = -1;
    buffer->data = malloc(bytes ? bytes : 1u);
    if (!buffer->data) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (data)
        memcpy(buffer->data, data, bytes);
    else
        memset(buffer->data, 0, bytes);
    if (type == VAEncCodedBufferType) {
        buffer->coded_segment.buf = buffer->data;
        buffer->coded_segment.next = NULL;
    }

    uint32_t id;
    pthread_mutex_lock(&driver->object_lock);
    bool inserted = rk_object_heap_insert(&driver->buffer_heap,
                                          &buffer->base, &id);
    pthread_mutex_unlock(&driver->object_lock);
    if (!inserted) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    *buffer_id = (VABufferID)id;
    return VA_STATUS_SUCCESS;
}

VAStatus rk_BufferSetNumElements(VADriverContextP context, VABufferID id,
                                 unsigned int num_elements)
{
    RKDriver *driver = drv_from_ctx(context);
    RKBuffer *buffer = buffer_acquire(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    if (buffer->size != 0 && num_elements > SIZE_MAX / buffer->size) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    size_t bytes = (size_t)buffer->size * num_elements;
    void *resized = realloc(buffer->data, bytes ? bytes : 1u);
    if (!resized) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (bytes > buffer->capacity)
        memset((uint8_t *)resized + buffer->capacity, 0,
               bytes - buffer->capacity);
    buffer->data = resized;
    buffer->capacity = bytes;
    buffer->num_elements = num_elements;
    if (buffer->type == VAEncCodedBufferType) {
        buffer->coded_segment.buf = resized;
        buffer->coded_segment.size = 0;
        buffer->coded_ready = false;
        buffer->coded_failed = false;
    }
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_MapBuffer(VADriverContextP context, VABufferID id, void **data)
{
    RKDriver *driver = drv_from_ctx(context);
    RKBuffer *buffer = buffer_acquire(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    /* A derived image's memory is the surface's DMA-BUF, so map it directly
     * and bracket the CPU access. Without the sync the mapping can read stale
     * lines the hardware wrote behind the cache. */
    if (buffer->derived_surface) {
        if (!derived_still_valid(buffer)) {
            rk_object_unref(&buffer->base);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (!buffer->derived_map) {
            void *mapped = mmap(NULL, buffer->derived_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                buffer->derived_fd, 0);
            if (mapped == MAP_FAILED) {
                LOG("MapBuffer: derived mmap failed fd=%d size=%zu errno=%d",
                    buffer->derived_fd, buffer->derived_size, errno);
                rk_object_unref(&buffer->base);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            buffer->derived_map = mapped;
        }
        if (!dmabuf_cpu_sync(buffer->derived_fd,
                             DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW)) {
            LOG("MapBuffer: derived sync start failed fd=%d errno=%d",
                buffer->derived_fd, errno);
            rk_object_unref(&buffer->base);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        *data = buffer->derived_map;
        rk_object_unref(&buffer->base);
        return VA_STATUS_SUCCESS;
    }

    *data = buffer->type == VAEncCodedBufferType
          ? (void *)&buffer->coded_segment : buffer->data;
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_buffer_store_coded(RKBuffer *buffer, const void *data, size_t size,
                               uint32_t status)
{
    if (!buffer || buffer->type != VAEncCodedBufferType || !data)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (size > buffer->capacity) {
        buffer->coded_segment.size = 0;
        buffer->coded_segment.status = VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW;
        buffer->coded_ready = true;
        buffer->coded_failed = true;
        return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
    }

    memcpy(buffer->data, data, size);
    memset(&buffer->coded_segment, 0, sizeof(buffer->coded_segment));
    buffer->coded_segment.size = (uint32_t)size;
    buffer->coded_segment.status = status;
    buffer->coded_segment.buf = buffer->data;
    buffer->coded_ready = true;
    buffer->coded_failed = false;
    return VA_STATUS_SUCCESS;
}

VAStatus rk_UnmapBuffer(VADriverContextP context, VABufferID id)
{
    RKDriver *driver = drv_from_ctx(context);
    RKBuffer *buffer = buffer_acquire(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    /* Keep the mapping for the next vaMapBuffer; only close the CPU access
     * window so the hardware may write again. */
    VAStatus status = VA_STATUS_SUCCESS;
    if (buffer->derived_surface && buffer->derived_map &&
        !dmabuf_cpu_sync(buffer->derived_fd,
                         DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW)) {
        LOG("UnmapBuffer: derived sync end failed fd=%d errno=%d",
            buffer->derived_fd, errno);
        status = VA_STATUS_ERROR_OPERATION_FAILED;
    }
    rk_object_unref(&buffer->base);
    return status;
}

VAStatus rk_buffer_acquire_handle(VADriverContextP context, VABufferID id,
                                  VABufferInfo *info)
{
    RKDriver *driver = drv_from_ctx(context);
    if (!info)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    RKBuffer *buffer = buffer_acquire(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!buffer->derived_surface) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    /* Only DRM PRIME is offered. A caller asking for a GEM name or a generic
     * pointer gets a real error rather than a handle it cannot use. */
    if (info->mem_type != 0 &&
        info->mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
        LOG("AcquireBufferHandle: unsupported mem_type 0x%x",
            (unsigned)info->mem_type);
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    if (!derived_still_valid(buffer)) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (buffer->acquired_fd < 0) {
        buffer->acquired_fd = dup(buffer->derived_fd);
        if (!derived_still_valid(buffer)) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (buffer->acquired_fd < 0) {
            LOG("AcquireBufferHandle: dup(%d) failed errno=%d",
                buffer->derived_fd, errno);
            rk_object_unref(&buffer->base);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    memset(info, 0, sizeof(*info));
    info->handle = (uintptr_t)buffer->acquired_fd;
    info->type = buffer->type;
    info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    info->mem_size = buffer->derived_size;
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_buffer_release_handle(VADriverContextP context, VABufferID id)
{
    RKDriver *driver = drv_from_ctx(context);
    RKBuffer *buffer = buffer_acquire(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->acquired_fd >= 0) {
        close(buffer->acquired_fd);
        buffer->acquired_fd = -1;
    }
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_DestroyBuffer(VADriverContextP context, VABufferID id)
{
    RKDriver *driver = drv_from_ctx(context);
    pthread_mutex_lock(&driver->object_lock);
    RKBuffer *buffer = (RKBuffer *)rk_object_heap_remove(
        &driver->buffer_heap, (uint32_t)id);
    pthread_mutex_unlock(&driver->object_lock);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_BufferInfo(VADriverContextP context, VABufferID id,
                       VABufferType *type, unsigned int *size,
                       unsigned int *num_elements)
{
    RKDriver *driver = drv_from_ctx(context);
    RKBuffer *buffer = buffer_acquire(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (type)
        *type = buffer->type;
    if (size)
        *size = buffer->size;
    if (num_elements)
        *num_elements = buffer->num_elements;
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_CreateBuffer2(VADriverContextP context, VAContextID context_id,
                          VABufferType type, unsigned int width,
                          unsigned int height, unsigned int *unit_size,
                          unsigned int *pitch, VABufferID *id)
{
    unsigned int stride = (width + 15) & ~15u;
    unsigned int size = stride * height;
    if (unit_size)
        *unit_size = size;
    if (pitch)
        *pitch = stride;
    return rk_CreateBuffer(context, context_id, type, size, 1, NULL, id);
}

VAStatus rk_QueryImageFormats(VADriverContextP context,
                              VAImageFormat *formats, int *num_formats)
{
    (void)context;
    memset(formats, 0, 4 * sizeof(*formats));
    formats[0].fourcc = VA_FOURCC_NV12;
    formats[0].byte_order = VA_LSB_FIRST;
    formats[0].bits_per_pixel = 12;
    formats[1].fourcc = VA_FOURCC_P010;
    formats[1].byte_order = VA_LSB_FIRST;
    formats[1].bits_per_pixel = 24;
    formats[2].fourcc = VA_FOURCC_I420;
    formats[2].byte_order = VA_LSB_FIRST;
    formats[2].bits_per_pixel = 12;
    formats[3].fourcc = VA_FOURCC_YV12;
    formats[3].byte_order = VA_LSB_FIRST;
    formats[3].bits_per_pixel = 12;
    *num_formats = 4;
    return VA_STATUS_SUCCESS;
}

VAStatus rk_CreateImage(VADriverContextP context, VAImageFormat *format,
                        int width, int height, VAImage *image)
{
    RKDriver *driver = drv_from_ctx(context);
    if (!format || !image || width <= 0 || height <= 0 ||
        width > USHRT_MAX || height > USHRT_MAX)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    unsigned int bytes_per_sample;
    bool planar = format->fourcc == VA_FOURCC_I420 ||
                  format->fourcc == VA_FOURCC_YV12;
    if (format->fourcc == VA_FOURCC_NV12)
        bytes_per_sample = 1;
    else if (format->fourcc == VA_FOURCC_P010)
        bytes_per_sample = 2;
    else if (planar && ((width & 1) || (height & 1)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    else if (planar)
        bytes_per_sample = 1;
    else
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    size_t aligned_width = ((size_t)(unsigned int)width + 15u) & ~15u;
    if (aligned_width > UINT_MAX / bytes_per_sample)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    unsigned int image_pitch = (unsigned int)aligned_width *
                               bytes_per_sample;
    unsigned int chroma_pitch = planar ? (unsigned int)aligned_width / 2
                                       : image_pitch;
    size_t allocation_size;
    if (planar) {
        size_t luma_size = (size_t)image_pitch * (unsigned int)height;
        size_t chroma_size = (size_t)chroma_pitch *
                             ((unsigned int)height / 2);
        if (chroma_size > (SIZE_MAX - luma_size) / 2)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        allocation_size = luma_size + 2 * chroma_size;
    } else if (!rk_nv12_layout_size(
                   image_pitch, (size_t)(unsigned int)height,
                   &allocation_size)) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (allocation_size > UINT_MAX)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    VABufferID buffer_id;
    unsigned int size = (unsigned int)allocation_size;
    VAStatus status = rk_CreateBuffer(context, 0, VAImageBufferType, size, 1,
                                      NULL, &buffer_id);
    if (status != VA_STATUS_SUCCESS)
        return status;

    RKBuffer *buffer = buffer_acquire(driver, buffer_id);
    if (!buffer) {
        rk_DestroyBuffer(context, buffer_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    RKImage *image_object = calloc(1, sizeof(*image_object));
    if (!image_object) {
        rk_object_unref(&buffer->base);
        rk_DestroyBuffer(context, buffer_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    rk_object_init(&image_object->base, image_destroy);
    image_object->buffer_id = buffer_id;
    image_object->buffer = buffer;
    image_object->fourcc = format->fourcc;
    image_object->width = (unsigned int)width;
    image_object->height = (unsigned int)height;
    image_object->num_planes = planar ? 3 : 2;
    image_object->pitches[0] = image_pitch;
    image_object->pitches[1] = chroma_pitch;
    image_object->pitches[2] = planar ? chroma_pitch : 0;
    image_object->offsets[0] = 0;
    image_object->offsets[1] = image_pitch * (unsigned int)height;
    image_object->offsets[2] = planar
        ? image_object->offsets[1] +
          chroma_pitch * ((unsigned int)height / 2)
        : 0;

    uint32_t image_id;
    pthread_mutex_lock(&driver->object_lock);
    bool inserted = rk_object_heap_insert(&driver->image_heap,
                                          &image_object->base, &image_id);
    pthread_mutex_unlock(&driver->object_lock);
    if (!inserted) {
        rk_object_unref(&image_object->base);
        rk_DestroyBuffer(context, buffer_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    memset(image, 0, sizeof(*image));
    image->image_id = (VAImageID)image_id;
    image->buf = buffer_id;
    image->format = *format;
    image->width = (unsigned short)width;
    image->height = (unsigned short)height;
    image->num_planes = image_object->num_planes;
    for (unsigned int plane = 0; plane < image_object->num_planes; plane++) {
        image->pitches[plane] = image_object->pitches[plane];
        image->offsets[plane] = image_object->offsets[plane];
    }
    image->data_size = size;
    return VA_STATUS_SUCCESS;
}

/* Build a VAImage that aliases the surface's own DMA-BUF rather than copying
 * into a fresh allocation. VLC's OpenGL VA-API converters need this: they
 * derive an image, take its buffer handle as a DRM PRIME fd, and import that
 * as an EGLImage. Refusing it made VLC drop its hardware decoder module
 * entirely and fall back to software.
 *
 * Only the driver's own linear NV12/P010 surfaces are derivable. Imported RGB
 * and AFBC layouts are not describable as a VAImage, so they fail closed. */
VAStatus rk_DeriveImage(VADriverContextP context, VASurfaceID surface_id,
                        VAImage *image)
{
    RKDriver *driver = drv_from_ctx(context);
    if (!image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    RKSurface *surface = surface_acquire(driver, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    /* A consumer deriving an image expects the decoded frame, and may not have
     * synced first. Mirror vaExportSurfaceHandle and settle the surface. */
    pthread_mutex_lock(&surface->lock);
    bool needs_sync = !surface->decoded && surface->ctx_id != 0;
    pthread_mutex_unlock(&surface->lock);
    if (needs_sync) {
        VAStatus sync_status = rk_SyncSurface(context, surface_id);
        if (sync_status != VA_STATUS_SUCCESS) {
            rk_object_unref(&surface->base);
            return sync_status;
        }
    }


    pthread_mutex_lock(&surface->lock);
    unsigned int width = (unsigned int)surface->width;
    unsigned int height = (unsigned int)surface->height;
    bool is_10bit = MPP_FRAME_FMT_IS_YUV_10BIT(surface->fmt);
    bool imported_rgb = surface->imported_rgb;
    /* surface->fmt is MPP's frame format, which stays AFBC even after a 10-bit
     * frame has been repacked into a driver-owned linear P010 buffer. Only a
     * surface still bound to the MPP frame itself can actually be compressed;
     * once backing_buf holds the conversion result the layout is linear. */
    bool fbc = surface->frame != NULL && MPP_FRAME_FMT_IS_FBC(surface->fmt);
    pthread_mutex_unlock(&surface->lock);

    /* A VAImage fixes its pitches once, but a surface's layout is only final
     * after decode binds the real frame -- and consumers legitimately derive
     * during pool setup, before that. 10-bit is the class where the two
     * reliably disagree: the placeholder is sized for its declared linear
     * P010, while the decoded frame arrives as AFBC NV15 and RGA repacks it at
     * MPP's stride. Refuse it and let the consumer use vaGetImage, whose
     * readback resolves the layout per call. Map and acquire re-check the rest
     * against the surface, so a stale image fails instead of returning pixels
     * from the wrong geometry. */
    if (imported_rgb || fbc || is_10bit) {
        LOG("DeriveImage: surface=0x%x layout is not a stable VAImage "
            "(rgb=%d fbc=%d 10bit=%d)", surface_id, imported_rgb, fbc,
            is_10bit);
        rk_object_unref(&surface->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int fd = -1;
    size_t object_size = 0;
    unsigned int luma_pitch = 0;
    size_t chroma_offset = 0;
    if (!surface_linear_layout(surface, &fd, &object_size, &luma_pitch,
                               &chroma_offset) ||
        object_size > UINT_MAX || width > USHRT_MAX || height > USHRT_MAX) {
        LOG("DeriveImage: surface=0x%x has no describable linear layout",
            surface_id);
        rk_object_unref(&surface->base);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    size_t required = chroma_offset + chroma_offset / 2;

    VABufferID buffer_id;
    VAStatus status = rk_CreateBuffer(context, 0, VAImageBufferType, 0, 0,
                                      NULL, &buffer_id);
    if (status != VA_STATUS_SUCCESS) {
        rk_object_unref(&surface->base);
        return status;
    }

    RKBuffer *buffer = buffer_acquire(driver, buffer_id);
    RKImage *image_object = buffer ? calloc(1, sizeof(*image_object)) : NULL;
    if (!image_object) {
        if (buffer)
            rk_object_unref(&buffer->base);
        rk_DestroyBuffer(context, buffer_id);
        rk_object_unref(&surface->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    /* The buffer keeps the surface alive: the image outlives the caller's
     * surface reference, and its memory is the surface's.
     *
     * The mapping spans the whole DMA-BUF, but the buffer's advertised size is
     * the image layout. MPP's decode buffers reserve codec side data past the
     * picture, and a consumer that trusts data_size as the frame extent -- as
     * GStreamer does -- would otherwise emit that padding as if it were
     * pixels. */
    buffer->derived_surface = surface;
    buffer->derived_fd = fd;
    buffer->derived_size = object_size;
    buffer->derived_pitch = luma_pitch;
    buffer->derived_chroma_offset = chroma_offset;
    buffer->capacity = required;
    buffer->size = (unsigned int)required;
    buffer->num_elements = 1;

    rk_object_init(&image_object->base, image_destroy);
    image_object->buffer_id = buffer_id;
    image_object->buffer = buffer;
    image_object->fourcc = is_10bit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    image_object->width = width;
    image_object->height = height;
    image_object->num_planes = 2;
    image_object->pitches[0] = luma_pitch;
    image_object->pitches[1] = luma_pitch;
    image_object->offsets[0] = 0;
    image_object->offsets[1] = (unsigned int)chroma_offset;

    uint32_t image_id;
    pthread_mutex_lock(&driver->object_lock);
    bool inserted = rk_object_heap_insert(&driver->image_heap,
                                          &image_object->base, &image_id);
    pthread_mutex_unlock(&driver->object_lock);
    if (!inserted) {
        rk_object_unref(&image_object->base);
        rk_DestroyBuffer(context, buffer_id);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    memset(image, 0, sizeof(*image));
    image->image_id = (VAImageID)image_id;
    image->buf = buffer_id;
    image->format.fourcc = image_object->fourcc;
    image->format.byte_order = VA_LSB_FIRST;
    image->format.bits_per_pixel = is_10bit ? 24 : 12;
    image->width = (unsigned short)width;
    image->height = (unsigned short)height;
    image->num_planes = 2;
    image->pitches[0] = image_object->pitches[0];
    image->pitches[1] = image_object->pitches[1];
    image->offsets[0] = image_object->offsets[0];
    image->offsets[1] = image_object->offsets[1];
    image->data_size = (unsigned int)required;

    LOG("DeriveImage: surface=0x%x %ux%u %s pitch=%u chroma_offset=%zu "
        "image=%zu object=%zu fd=%d", surface_id, width, height,
        is_10bit ? "P010" : "NV12", luma_pitch, chroma_offset, required,
        object_size, fd);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_DestroyImage(VADriverContextP context, VAImageID id)
{
    RKDriver *driver = drv_from_ctx(context);
    pthread_mutex_lock(&driver->object_lock);
    RKImage *image = (RKImage *)rk_object_heap_remove(
        &driver->image_heap, (uint32_t)id);
    RKBuffer *buffer = image ? (RKBuffer *)rk_object_heap_remove(
        &driver->buffer_heap, (uint32_t)image->buffer_id) : NULL;
    pthread_mutex_unlock(&driver->object_lock);
    if (!image)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if (buffer)
        rk_object_unref(&buffer->base);
    rk_object_unref(&image->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_SetImagePalette(VADriverContextP context, VAImageID image,
                            unsigned char *palette)
{
    (void)context;
    (void)image;
    (void)palette;
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

VAStatus rk_PutImage(VADriverContextP context, VASurfaceID surface,
                     VAImageID image, int src_x, int src_y,
                     unsigned int src_width, unsigned int src_height,
                     int dest_x, int dest_y, unsigned int dest_width,
                     unsigned int dest_height)
{
    RKDriver *driver = drv_from_ctx(context);
    RKSurface *target = surface_acquire(driver, surface);
    RKImage *source = image_acquire(driver, image);
    RKBuffer *source_buffer = source ? source->buffer : NULL;
    if (!target) {
        if (source)
            rk_object_unref(&source->base);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!source || !source_buffer || !source_buffer->data) {
        rk_object_unref(&target->base);
        if (source)
            rk_object_unref(&source->base);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    bool full_frame = src_x == 0 && src_y == 0 && dest_x == 0 && dest_y == 0 &&
                      src_width == source->width &&
                      src_height == source->height &&
                      dest_width == (unsigned int)target->width &&
                      dest_height == (unsigned int)target->height &&
                      src_width == dest_width && src_height == dest_height;
    bool is_10bit = MPP_FRAME_FMT_IS_YUV_10BIT(target->fmt);
    bool planar = source->fourcc == VA_FOURCC_I420 ||
                  source->fourcc == VA_FOURCC_YV12;
    unsigned int bytes_per_sample = is_10bit ? 2u : 1u;
    size_t row_bytes = (size_t)target->width * bytes_per_sample;
    size_t chroma_bytes = (size_t)target->width / 2;
    bool source_layout_valid =
        image_plane_fits(source, 0, planar ? (size_t)target->width : row_bytes,
                         (unsigned int)target->height,
                         source_buffer->capacity) &&
        image_plane_fits(source, 1, planar ? chroma_bytes : row_bytes,
                         (unsigned int)target->height / 2,
                         source_buffer->capacity) &&
        (!planar ||
         image_plane_fits(source, 2, chroma_bytes,
                          (unsigned int)target->height / 2,
                          source_buffer->capacity));
    if (target->import_buf || !full_frame ||
        source->fourcc != target->fourcc ||
        (planar && is_10bit) || !source_layout_valid) {
        rk_object_unref(&source->base);
        rk_object_unref(&target->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    pthread_mutex_lock(&target->lock);
    MppBuffer destination = target->priv_buf;
    size_t destination_pitch = (size_t)target->hstride * bytes_per_sample;
    size_t destination_size;
    if (!destination || destination_pitch < row_bytes ||
        !rk_nv12_layout_size(destination_pitch, (size_t)target->vstride,
                             &destination_size) ||
        destination_size > mpp_buffer_get_size(destination) ||
        source->offsets[0] > source_buffer->capacity) {
        pthread_mutex_unlock(&target->lock);
        rk_object_unref(&source->base);
        rk_object_unref(&target->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    int fd = mpp_buffer_get_fd(destination);
    uint8_t *dst = mpp_buffer_get_ptr(destination);
    const uint8_t *src = source_buffer->data;
    if (fd < 0 || !dst ||
        !dmabuf_cpu_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE)) {
        pthread_mutex_unlock(&target->lock);
        rk_object_unref(&source->base);
        rk_object_unref(&target->base);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    memset(dst, 0, destination_size);
    for (int row = 0; row < target->height; row++)
        memcpy(dst + (size_t)row * destination_pitch,
               src + source->offsets[0] +
                   (size_t)row * source->pitches[0],
               planar ? (size_t)target->width : row_bytes);
    uint8_t *dst_uv = dst + destination_pitch * (size_t)target->vstride;
    if (planar) {
        unsigned int u_plane = source->fourcc == VA_FOURCC_I420 ? 1 : 2;
        unsigned int v_plane = source->fourcc == VA_FOURCC_I420 ? 2 : 1;
        for (int row = 0; row < target->height / 2; row++) {
            const uint8_t *src_u = src + source->offsets[u_plane] +
                                   (size_t)row *
                                       source->pitches[u_plane];
            const uint8_t *src_v = src + source->offsets[v_plane] +
                                   (size_t)row *
                                       source->pitches[v_plane];
            uint8_t *row_uv = dst_uv + (size_t)row * destination_pitch;
            for (size_t column = 0; column < chroma_bytes; column++) {
                row_uv[2 * column] = src_u[column];
                row_uv[2 * column + 1] = src_v[column];
            }
        }
    } else {
        const uint8_t *src_uv = src + source->offsets[1];
        for (int row = 0; row < target->height / 2; row++)
            memcpy(dst_uv + (size_t)row * destination_pitch,
                   src_uv + (size_t)row * source->pitches[1], row_bytes);
    }

    bool sync_ok = dmabuf_cpu_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    if (sync_ok && planar)
        LOG("PutImage: %s->NV12 %dx%d",
            source->fourcc == VA_FOURCC_I420 ? "I420" : "YV12",
            target->width, target->height);
    pthread_mutex_unlock(&target->lock);
    rk_object_unref(&source->base);
    rk_object_unref(&target->base);
    return sync_ok ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_OPERATION_FAILED;
}
