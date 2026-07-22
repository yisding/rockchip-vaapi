#ifndef RK_VAAPI_EXPORT_H
#define RK_VAAPI_EXPORT_H

#include <stdint.h>

#include <va/va_backend.h>

VAStatus rk_ExportSurfaceHandle(VADriverContextP context, VASurfaceID id,
                                uint32_t mem_type, uint32_t flags,
                                void *descriptor);

#endif
