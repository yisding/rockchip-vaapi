#ifndef RK_VAAPI_CONTEXT_H
#define RK_VAAPI_CONTEXT_H

#include <va/va_backend.h>

VAStatus rk_CreateContext(VADriverContextP context, VAConfigID config,
                          int width, int height, int flags,
                          VASurfaceID *targets, int target_count,
                          VAContextID *id);
VAStatus rk_DestroyContext(VADriverContextP context, VAContextID id);
VAStatus rk_BeginPicture(VADriverContextP context, VAContextID id,
                         VASurfaceID target);
VAStatus rk_RenderPicture(VADriverContextP context, VAContextID id,
                          VABufferID *buffers, int buffer_count);
VAStatus rk_EndPicture(VADriverContextP context, VAContextID id);

#endif
