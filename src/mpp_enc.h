#ifndef RK_VAAPI_MPP_ENC_H
#define RK_VAAPI_MPP_ENC_H

#include <va/va_backend.h>

#include "driver_internal.h"

VAStatus rk_mpp_enc_init(RKContext *context);
VAStatus rk_mpp_enc_render_buffer(RKContext *context, RKBuffer *buffer);
VAStatus rk_mpp_enc_encode(RKContext *context);

#endif
