#ifndef RK_VAAPI_SURFACE_H
#define RK_VAAPI_SURFACE_H

#include <stdint.h>

#include <va/va_backend.h>

VAStatus rk_SyncSurface(VADriverContextP context, VASurfaceID id);
VAStatus rk_SyncSurface2(VADriverContextP context, VASurfaceID id,
                         uint64_t timeout_ns);

#endif
