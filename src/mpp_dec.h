#ifndef RK_VAAPI_MPP_DEC_H
#define RK_VAAPI_MPP_DEC_H

#include <stdbool.h>
#include <stdint.h>

#include "driver_internal.h"

void *rk_mpp_dec_worker_main(void *opaque);
void rk_mpp_dec_stop(RKContext *context);
void rk_mpp_dec_fail_surface(RKSurface *surface, uint64_t fence);
VAStatus rk_mpp_dec_build_job(RKContext *context, RKDriver *driver,
                              RKDecodeJob **job);
void rk_mpp_dec_job_destroy(RKDecodeJob *job);
bool rk_mpp_dec_enqueue_job(RKContext *context, RKDecodeJob *job);
void rk_mpp_dec_reject_job(RKDecodeJob *job);

#endif
