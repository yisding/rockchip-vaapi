#ifndef RK_VAAPI_BUFFER_H
#define RK_VAAPI_BUFFER_H

#include <va/va_backend.h>

VAStatus rk_CreateBuffer(VADriverContextP context, VAContextID context_id,
                         VABufferType type, unsigned int size,
                         unsigned int num_elements, void *data,
                         VABufferID *buffer_id);
VAStatus rk_BufferSetNumElements(VADriverContextP context, VABufferID id,
                                 unsigned int num_elements);
VAStatus rk_MapBuffer(VADriverContextP context, VABufferID id, void **data);
VAStatus rk_UnmapBuffer(VADriverContextP context, VABufferID id);
VAStatus rk_DestroyBuffer(VADriverContextP context, VABufferID id);
VAStatus rk_BufferInfo(VADriverContextP context, VABufferID id,
                       VABufferType *type, unsigned int *size,
                       unsigned int *num_elements);
VAStatus rk_CreateBuffer2(VADriverContextP context, VAContextID context_id,
                          VABufferType type, unsigned int width,
                          unsigned int height, unsigned int *unit_size,
                          unsigned int *pitch, VABufferID *id);

VAStatus rk_QueryImageFormats(VADriverContextP context,
                              VAImageFormat *formats, int *num_formats);
VAStatus rk_CreateImage(VADriverContextP context, VAImageFormat *format,
                        int width, int height, VAImage *image);
VAStatus rk_DeriveImage(VADriverContextP context, VASurfaceID surface,
                        VAImage *image);
VAStatus rk_DestroyImage(VADriverContextP context, VAImageID id);
VAStatus rk_SetImagePalette(VADriverContextP context, VAImageID image,
                            unsigned char *palette);
VAStatus rk_PutImage(VADriverContextP context, VASurfaceID surface,
                     VAImageID image, int src_x, int src_y,
                     unsigned int src_width, unsigned int src_height,
                     int dest_x, int dest_y, unsigned int dest_width,
                     unsigned int dest_height);

#endif
