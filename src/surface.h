#ifndef RK_VAAPI_SURFACE_H
#define RK_VAAPI_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include <va/va_backend.h>

typedef struct RKSurface RKSurface;

VAStatus rk_CreateSurfaces(VADriverContextP context, int width, int height,
                           int format, int count, VASurfaceID *ids);
VAStatus rk_DestroySurfaces(VADriverContextP context, VASurfaceID *ids,
                            int count);
VAStatus rk_CreateSurfaces2(VADriverContextP context, unsigned int format,
                            unsigned int width, unsigned int height,
                            VASurfaceID *ids, unsigned int count,
                            VASurfaceAttrib *attributes,
                            unsigned int attribute_count);
VAStatus rk_SyncSurface(VADriverContextP context, VASurfaceID id);
VAStatus rk_SyncSurface2(VADriverContextP context, VASurfaceID id,
                         uint64_t timeout_ns);
VAStatus rk_QuerySurfaceStatus(VADriverContextP context, VASurfaceID id,
                               VASurfaceStatus *status);
VAStatus rk_GetImage(VADriverContextP context, VASurfaceID surface,
                     int x, int y, unsigned int width, unsigned int height,
                     VAImageID image);
/* Caller holds surface->lock. Refresh the driver's contiguous backing buffer
 * from a two-object linear NV12/P010 import. */
bool rk_surface_normalize_multiplane_import(RKSurface *surface);

#endif
