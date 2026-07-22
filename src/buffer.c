#include "buffer.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver_internal.h"
#include "frame_layout.h"

static void buffer_destroy(void *opaque)
{
    RKBuffer *buffer = opaque;
    free(buffer->data);
    free(buffer);
}

static void image_destroy(void *opaque)
{
    RKImage *image = opaque;
    if (image->buffer)
        rk_object_unref(&image->buffer->base);
    free(image);
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
    buffer->data = malloc(bytes ? bytes : 1u);
    if (!buffer->data) {
        rk_object_unref(&buffer->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (data)
        memcpy(buffer->data, data, bytes);
    else
        memset(buffer->data, 0, bytes);

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
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_MapBuffer(VADriverContextP context, VABufferID id, void **data)
{
    RKDriver *driver = drv_from_ctx(context);
    RKBuffer *buffer = buffer_acquire(driver, id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    *data = buffer->data;
    rk_object_unref(&buffer->base);
    return VA_STATUS_SUCCESS;
}

VAStatus rk_UnmapBuffer(VADriverContextP context, VABufferID id)
{
    (void)context;
    (void)id;
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
    formats[0].fourcc = VA_FOURCC_NV12;
    formats[0].byte_order = VA_LSB_FIRST;
    formats[0].bits_per_pixel = 12;
    formats[1].fourcc = VA_FOURCC_P010;
    formats[1].byte_order = VA_LSB_FIRST;
    formats[1].bits_per_pixel = 24;
    *num_formats = 2;
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
    if (format->fourcc == VA_FOURCC_NV12)
        bytes_per_sample = 1;
    else if (format->fourcc == VA_FOURCC_P010)
        bytes_per_sample = 2;
    else
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    size_t aligned_width = ((size_t)(unsigned int)width + 15u) & ~15u;
    if (aligned_width > UINT_MAX / bytes_per_sample)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    unsigned int image_pitch =
        (unsigned int)aligned_width * bytes_per_sample;
    size_t allocation_size;
    if (!rk_nv12_layout_size(image_pitch, (size_t)(unsigned int)height,
                             &allocation_size) ||
        allocation_size > UINT_MAX)
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
    image_object->pitch = image_pitch;

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
    image->num_planes = 2;
    image->pitches[0] = image_pitch;
    image->pitches[1] = image_pitch;
    image->offsets[0] = 0;
    image->offsets[1] = image_pitch * (unsigned int)height;
    image->data_size = size;
    return VA_STATUS_SUCCESS;
}

VAStatus rk_DeriveImage(VADriverContextP context, VASurfaceID surface_id,
                        VAImage *image)
{
    RKDriver *driver = drv_from_ctx(context);
    RKSurface *surface = surface_acquire(driver, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    (void)image;
    rk_object_unref(&surface->base);
    return VA_STATUS_ERROR_OPERATION_FAILED;
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

VAStatus rk_PutImage(VADriverContextP context, VASurfaceID surface,
                     VAImageID image, int src_x, int src_y,
                     unsigned int src_width, unsigned int src_height,
                     int dest_x, int dest_y, unsigned int dest_width,
                     unsigned int dest_height)
{
    (void)context;
    (void)surface;
    (void)image;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dest_x;
    (void)dest_y;
    (void)dest_width;
    (void)dest_height;
    return VA_STATUS_SUCCESS;
}
