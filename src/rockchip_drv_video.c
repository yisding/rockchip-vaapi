/*
 * rockchip_drv_video.c — VA-API driver for Rockchip RK3588 via MPP
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>

#include "buffer.h"
#include "context.h"
#include "driver_internal.h"
#include "export.h"
#include "surface.h"

/* ── VADriverVTable implementations ──────────────────────────── */

static VAStatus rk_Terminate(VADriverContextP ctx) {
    LOG("Terminate: cleaning up driver");
    RKDriver *d = drv_from_ctx(ctx);
    if (!d) return VA_STATUS_SUCCESS;

    /* destroy any leftover objects */
    rk_object_heap_finish(&d->context_heap);
    rk_object_heap_finish(&d->surface_heap);
    rk_object_heap_finish(&d->image_heap);
    rk_object_heap_finish(&d->buffer_heap);
    rk_object_heap_finish(&d->config_heap);
    pthread_mutex_destroy(&d->object_lock);
    free(d);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

/* Only profiles with validated decode paths are advertised (or accepted by
 * CreateConfig). Board-validated 2026-07-21 on RK3588 / kernel 6.18 via
 * software-vs-VAAPI framemd5 comparison:
 *   - H.264 Main/High: bit-exact on pinned conformance vectors
 *   - VP9 Profile 0: bit-exact
 * Deliberately not offered:
 *   - H.264 Constrained Baseline: MPP decodes the pinned SVA_Base_B stream
 *     incorrectly even though the reconstructed Annex B stream is
 *     software-exact. Fall back instead of returning corrupt frames.
 *   - HEVC: reconstruction and worker routing exist, and a narrow
 *     RK_VAAPI_EXPERIMENTAL_PROFILES=hevc-main gate can expose Main for
 *     validation. Seven pinned Main vectors are bit-exact as of 2026-07-26,
 *     but the profile remains hidden by default until the full pinned gate is
 *     bit-exact rather than partially fail-closed.
 *   - HEVC Main10: MPP AFBC-to-P010 conversion is bit-exact on the narrow
 *     hevc-main10 gate, but it remains hidden until the broader conformance
 *     and HDR gates pass.
 *   - VP9 Profile 2: a narrow vp9-profile2 AFBC-to-P010 gate is available,
 *     but the profile remains hidden pending broader conformance and HDR
 *     validation.
 *   - H.264 High10: not yet wired to an experimental gate.
 *   - VP8: verified segfault in the generic path.
 *   - AV1: MPP needs a full OBU bytestream but VA-API hands us only
 *     headerless tile data, so MPP can never parse it. Firefox falls back
 *     to VP9 (hardware-decoded) for AV1-capable content. */
static bool experimental_profile_enabled(const char *profile)
{
    const char *enabled = getenv("RK_VAAPI_EXPERIMENTAL_PROFILES");
    if (!enabled || !enabled[0] || !strcmp(enabled, "0"))
        return false;
    return !strcmp(enabled, "1") ||
           !strcmp(enabled, "all") ||
           !strcmp(enabled, profile);
}

static bool experimental_encode_enabled(const char *codec)
{
    const char *enabled = getenv("RK_VAAPI_EXPERIMENTAL_ENCODE");
    if (!enabled || !enabled[0] || !strcmp(enabled, "0"))
        return false;
    if (!strcmp(enabled, "1") || !strcmp(enabled, "all"))
        return true;

    size_t codec_len = strlen(codec);
    const char *cursor = enabled;
    while (*cursor) {
        while (*cursor == ',' || *cursor == ' ' || *cursor == '\t')
            cursor++;
        const char *end = cursor;
        while (*end && *end != ',' && *end != ' ' && *end != '\t')
            end++;
        if ((size_t)(end - cursor) == codec_len &&
            !memcmp(cursor, codec, codec_len))
            return true;
        cursor = end;
    }
    return false;
}

static bool decode_profile_supported(VAProfile p)
{
    switch (p) {
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileVP9Profile0:
        return true;
    case VAProfileHEVCMain:
        return experimental_profile_enabled("hevc-main");
    case VAProfileHEVCMain10:
        return experimental_profile_enabled("hevc-main10");
    case VAProfileVP9Profile2:
        return experimental_profile_enabled("vp9-profile2");
    default:
        return false;
    }
}

static bool encode_profile_supported(VAProfile profile)
{
    return ((profile == VAProfileH264Main ||
             profile == VAProfileH264High) &&
            experimental_encode_enabled("h264")) ||
           (profile == VAProfileHEVCMain &&
            experimental_encode_enabled("hevc"));
}

static bool profile_supported(VAProfile profile)
{
    return decode_profile_supported(profile) ||
           encode_profile_supported(profile);
}

static uint32_t hevc_encode_features(void)
{
    VAConfigAttribValEncHEVCFeatures features = { .value = 0 };
    return features.value;
}

static uint32_t hevc_encode_block_sizes(void)
{
    VAConfigAttribValEncHEVCBlockSizes sizes = { .value = 0 };
    sizes.bits.log2_max_coding_tree_block_size_minus3 = 3;
    sizes.bits.log2_min_coding_tree_block_size_minus3 = 3;
    sizes.bits.log2_min_luma_coding_block_size_minus3 = 0;
    sizes.bits.log2_max_luma_transform_block_size_minus2 = 3;
    sizes.bits.log2_min_luma_transform_block_size_minus2 = 0;
    sizes.bits.max_max_transform_hierarchy_depth_inter = 1;
    sizes.bits.min_max_transform_hierarchy_depth_inter = 1;
    sizes.bits.max_max_transform_hierarchy_depth_intra = 1;
    sizes.bits.min_max_transform_hierarchy_depth_intra = 1;
    sizes.bits.log2_max_pcm_coding_block_size_minus3 = 2;
    sizes.bits.log2_min_pcm_coding_block_size_minus3 = 0;
    return sizes.value;
}

static VAStatus rk_QueryConfigProfiles(VADriverContextP ctx,
                                       VAProfile *list, int *n) {
    (void)ctx;
    int i = 0;
    list[i++] = VAProfileH264Main;
    list[i++] = VAProfileH264High;
    list[i++] = VAProfileVP9Profile0;
    if (profile_supported(VAProfileHEVCMain))
        list[i++] = VAProfileHEVCMain;
    if (profile_supported(VAProfileHEVCMain10))
        list[i++] = VAProfileHEVCMain10;
    if (profile_supported(VAProfileVP9Profile2))
        list[i++] = VAProfileVP9Profile2;
    *n = i;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QueryConfigEntrypoints(VADriverContextP ctx,
                                          VAProfile profile,
                                          VAEntrypoint *list, int *n) {
    (void)ctx;
    if (!profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    int count = 0;
    if (decode_profile_supported(profile))
        list[count++] = VAEntrypointVLD;
    if (encode_profile_supported(profile))
        list[count++] = VAEntrypointEncSlice;
    *n = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_GetConfigAttributes(VADriverContextP ctx,
                                       VAProfile profile,
                                       VAEntrypoint entrypoint,
                                       VAConfigAttrib *list, int n) {
    (void)ctx;
    bool encode = entrypoint == VAEntrypointEncSlice;
    if (encode && !encode_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int i = 0; i < n; i++) {
        LOG("GetConfigAttributes: type=%d", list[i].type);
        switch (list[i].type) {
        case VAConfigAttribRTFormat:
            list[i].value = !encode &&
                            (profile == VAProfileHEVCMain10 ||
                             profile == VAProfileVP9Profile2)
                          ? VA_RT_FORMAT_YUV420_10
                          : VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribRateControl:
            list[i].value = encode ? VA_RC_CQP | VA_RC_CBR | VA_RC_VBR
                                   : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncPackedHeaders:
            list[i].value = encode ? VA_ENC_PACKED_HEADER_NONE
                                   : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncInterlaced:
            list[i].value = encode ? VA_ENC_INTERLACED_NONE
                                   : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxRefFrames:
            list[i].value = encode ? 1u : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxSlices:
            list[i].value = encode ? 1u : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncSliceStructure:
            list[i].value = encode ? VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS
                                   : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncQualityRange:
            list[i].value = encode ? 1u : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureWidth:
            list[i].value = encode ? RK_MAX_WIDTH : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureHeight:
            list[i].value = encode ? RK_MAX_HEIGHT : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncHEVCFeatures:
            list[i].value = encode && profile == VAProfileHEVCMain
                          ? hevc_encode_features()
                          : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncHEVCBlockSizes:
            list[i].value = encode && profile == VAProfileHEVCMain
                          ? hevc_encode_block_sizes()
                          : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribDecSliceMode:
            list[i].value = encode ? VA_ATTRIB_NOT_SUPPORTED
                                   : VA_DEC_SLICE_MODE_NORMAL;
            break;
        case VAConfigAttribEncryption:
            list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        default:
            LOG("GetConfigAttributes: unsupported type=%d", list[i].type);
            list[i].value = VA_ATTRIB_NOT_SUPPORTED;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_CreateConfig(VADriverContextP ctx,
                                 VAProfile profile, VAEntrypoint entrypoint,
                                 VAConfigAttrib *attribs, int n_attribs,
                                 VAConfigID *out_id) {
    RKDriver *d = drv_from_ctx(ctx);
    LOG("CreateConfig: profile=%d entrypoint=%d n_attribs=%d",
        profile, entrypoint, n_attribs);

    if (!profile_supported(profile)) {
        LOG("CreateConfig: unsupported profile %d", profile);
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    bool encode = entrypoint == VAEntrypointEncSlice;
    if (entrypoint != VAEntrypointVLD && !encode) {
        LOG("CreateConfig: unsupported entrypoint %d", entrypoint);
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if (entrypoint == VAEntrypointVLD &&
        !decode_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    if (encode && !encode_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    if (n_attribs < 0 || (n_attribs > 0 && !attribs))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    uint32_t expected_rt_format = !encode &&
                                  (profile == VAProfileHEVCMain10 ||
                                   profile == VAProfileVP9Profile2)
                                ? VA_RT_FORMAT_YUV420_10
                                : VA_RT_FORMAT_YUV420;
    uint32_t rt_format = expected_rt_format;
    uint32_t rate_control = VA_RC_CQP;
    for (int i = 0; i < n_attribs; i++) {
        switch (attribs[i].type) {
        case VAConfigAttribRTFormat:
            if (attribs[i].value != expected_rt_format)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            rt_format = attribs[i].value;
            break;
        case VAConfigAttribRateControl:
            if (!encode || (attribs[i].value != VA_RC_CQP &&
                            attribs[i].value != VA_RC_CBR &&
                            attribs[i].value != VA_RC_VBR))
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            rate_control = attribs[i].value;
            break;
        case VAConfigAttribEncPackedHeaders:
            if (!encode || attribs[i].value != VA_ENC_PACKED_HEADER_NONE)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        case VAConfigAttribDecSliceMode:
            if (encode || attribs[i].value != VA_DEC_SLICE_MODE_NORMAL)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        default:
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }

    RKConfig *config = calloc(1, sizeof(*config));
    if (!config)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    rk_object_init(&config->base, free);
    config->profile = profile;
    config->entrypoint = entrypoint;
    config->rt_format = rt_format;
    config->rate_control = rate_control;

    uint32_t id;
    pthread_mutex_lock(&d->object_lock);
    bool inserted = rk_object_heap_insert(&d->config_heap, &config->base,
                                          &id);
    pthread_mutex_unlock(&d->object_lock);
    if (!inserted) {
        rk_object_unref(&config->base);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    *out_id = (VAConfigID)id;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_DestroyConfig(VADriverContextP ctx, VAConfigID id) {
    RKDriver *d = drv_from_ctx(ctx);
    pthread_mutex_lock(&d->object_lock);
    RKConfig *c = (RKConfig *)rk_object_heap_remove(&d->config_heap,
                                                    (uint32_t)id);
    pthread_mutex_unlock(&d->object_lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    rk_object_unref(&c->base);
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QueryConfigAttributes(VADriverContextP ctx,
                                          VAConfigID id,
                                          VAProfile *profile,
                                          VAEntrypoint *entrypoint,
                                          VAConfigAttrib *attribs, int *n) {
    RKDriver *d = drv_from_ctx(ctx);
    RKConfig *c = config_acquire(d, id);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    *profile = c->profile;
    *entrypoint = c->entrypoint;
    int count = c->entrypoint == VAEntrypointEncSlice ? 2 : 1;
    if (attribs) {
        attribs[0] = (VAConfigAttrib) {
            .type = VAConfigAttribRTFormat,
            .value = c->rt_format,
        };
        if (c->entrypoint == VAEntrypointEncSlice) {
            attribs[1] = (VAConfigAttrib) {
                .type = VAConfigAttribRateControl,
                .value = c->rate_control,
            };
        }
    }
    *n = count;
    rk_object_unref(&c->base);
    return VA_STATUS_SUCCESS;
}

/* ── stub implementations ────────────────────────────────────── */

/* Suppress -Wunused-parameter for pure stub functions */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static VAStatus rk_QuerySubpicFmts(VADriverContextP ctx,
                                    VAImageFormat *format_list,
                                    unsigned int *flags,
                                    unsigned int *num_formats)
{ *num_formats = 0; return VA_STATUS_SUCCESS; }

static VAStatus rk_CreateSubpicture(VADriverContextP ctx,
                                     VAImageID image,
                                     VASubpictureID *subpicture)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_DestroySubpicture(VADriverContextP ctx,
                                      VASubpictureID subpicture)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicImage(VADriverContextP ctx,
                                   VASubpictureID subpicture, VAImageID image)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicChromakey(VADriverContextP ctx,
                                       VASubpictureID subpicture,
                                       unsigned int chromakey_min,
                                       unsigned int chromakey_max,
                                       unsigned int chromakey_mask)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetSubpicAlpha(VADriverContextP ctx,
                                   VASubpictureID subpicture, float global_alpha)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_AssociateSubpic(VADriverContextP ctx,
                                    VASubpictureID subpicture,
                                    VASurfaceID *target_surfaces, int num_surfaces,
                                    short src_x, short src_y,
                                    unsigned short src_width, unsigned short src_height,
                                    short dest_x, short dest_y,
                                    unsigned short dest_width, unsigned short dest_height,
                                    unsigned int flags)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_DeassociateSubpic(VADriverContextP ctx,
                                      VASubpictureID subpicture,
                                      VASurfaceID *target_surfaces, int num_surfaces)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QueryDisplayAttrs(VADriverContextP ctx,
                                      VADisplayAttribute *attr_list,
                                      int *num_attributes)
{ *num_attributes = 0; return VA_STATUS_SUCCESS; }

static VAStatus rk_GetDisplayAttrs(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_SetDisplayAttrs(VADriverContextP ctx,
                                    VADisplayAttribute *attr_list, int num_attributes)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_LockSurface(VADriverContextP ctx, VASurfaceID surface,
                                unsigned int *fourcc,
                                unsigned int *luma_stride,
                                unsigned int *chroma_u_stride,
                                unsigned int *chroma_v_stride,
                                unsigned int *luma_offset,
                                unsigned int *chroma_u_offset,
                                unsigned int *chroma_v_offset,
                                unsigned int *buffer_name, void **buffer)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_UnlockSurface(VADriverContextP ctx, VASurfaceID surface)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_QuerySurfaceAttrs(VADriverContextP ctx, VAConfigID config,
                                      VASurfaceAttrib *attrib_list,
                                      unsigned int *num_attribs)
{
    LOG("QuerySurfaceAttributes: config=0x%x list=%s",
        config, attrib_list ? "provided" : "NULL (query count)");

    RKDriver *driver = drv_from_ctx(ctx);
    RKConfig *cfg = config_acquire(driver, config);
    if (!cfg)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    bool is_10bit = cfg->rt_format == VA_RT_FORMAT_YUV420_10;
    bool planar_uploads = cfg->entrypoint == VAEntrypointEncSlice &&
                          !is_10bit;
    /* Firefox calls this twice: first with NULL to get count, then with buffer */
    const unsigned int pixel_formats = planar_uploads ? 3 : 1;
    const unsigned int n = pixel_formats + 3;
    if (!attrib_list) {
        *num_attribs = n;
        rk_object_unref(&cfg->base);
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < n) {
        *num_attribs = n;
        rk_object_unref(&cfg->base);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    rk_object_unref(&cfg->base);

    /* A config has one RT format; do not expose P010 to 8-bit encoders. */
    attrib_list[0].type              = VASurfaceAttribPixelFormat;
    attrib_list[0].flags             = VA_SURFACE_ATTRIB_GETTABLE |
                                       VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[0].value.type        = VAGenericValueTypeInteger;
    attrib_list[0].value.value.i     = is_10bit ? VA_FOURCC_P010
                                                : VA_FOURCC_NV12;
    if (planar_uploads) {
        attrib_list[1] = attrib_list[0];
        attrib_list[1].value.value.i = VA_FOURCC_I420;
        attrib_list[2] = attrib_list[0];
        attrib_list[2].value.value.i = VA_FOURCC_YV12;
    }

    /* Memory type: VA-managed + DRM PRIME 2 */
    unsigned int memory_index = pixel_formats;
    attrib_list[memory_index].type          = VASurfaceAttribMemoryType;
    attrib_list[memory_index].flags         = VA_SURFACE_ATTRIB_GETTABLE |
                                              VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[memory_index].value.type    = VAGenericValueTypeInteger;
    attrib_list[memory_index].value.value.i =
        (int)(VA_SURFACE_ATTRIB_MEM_TYPE_VA |
              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);

    /* Max resolution */
    attrib_list[memory_index + 1].type       = VASurfaceAttribMaxWidth;
    attrib_list[memory_index + 1].flags      = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[memory_index + 1].value.type =
        VAGenericValueTypeInteger;
    attrib_list[memory_index + 1].value.value.i = RK_MAX_WIDTH;
    attrib_list[memory_index + 2].type       = VASurfaceAttribMaxHeight;
    attrib_list[memory_index + 2].flags      = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[memory_index + 2].value.type =
        VAGenericValueTypeInteger;
    attrib_list[memory_index + 2].value.value.i = RK_MAX_HEIGHT;

    *num_attribs = n;
    LOG("QuerySurfaceAttributes: returned %u attribs (%s%s, DRM_PRIME_2)",
        n, is_10bit ? "P010" : "NV12",
        planar_uploads ? "/I420/YV12" : "");
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_AcquireBufferHandle(VADriverContextP ctx,
                                        VABufferID buf_id, VABufferInfo *buf_info)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{ return VA_STATUS_SUCCESS; }

static VAStatus rk_CreateMFContext(VADriverContextP ctx,
                                    VAMFContextID *mfe_context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFAddContext(VADriverContextP ctx,
                                 VAMFContextID mf_context, VAContextID context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFReleaseContext(VADriverContextP ctx,
                                     VAMFContextID mf_context, VAContextID context)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_MFSubmit(VADriverContextP ctx, VAMFContextID mf_context,
                             VAContextID *contexts, int num_contexts)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_QueryProcessingRate(VADriverContextP ctx,
                                        VAConfigID config_id,
                                        VAProcessingRateParameter *proc_buf,
                                        unsigned int *processing_rate)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus rk_SyncBuffer(VADriverContextP ctx, VABufferID buf_id,
                               uint64_t timeout_ns)
{
    (void)timeout_ns;
    RKBuffer *buffer = buffer_acquire(drv_from_ctx(ctx), buf_id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    VAStatus status = VA_STATUS_SUCCESS;
    if (buffer->type == VAEncCodedBufferType) {
        if (buffer->coded_failed)
            status = VA_STATUS_ERROR_ENCODING_ERROR;
        else if (!buffer->coded_ready)
            status = VA_STATUS_ERROR_TIMEDOUT;
    }
    rk_object_unref(&buffer->base);
    return status;
}

static VAStatus rk_Copy(VADriverContextP ctx, VACopyObject *dst,
                         VACopyObject *src, VACopyOption option)
{ return VA_STATUS_ERROR_UNIMPLEMENTED; }

#pragma GCC diagnostic pop

static VAStatus rk_PutSurface(VADriverContextP ctx, VASurfaceID s,
                               void *draw, short sx, short sy,
                               unsigned short sw, unsigned short sh,
                               short dx, short dy,
                               unsigned short dw, unsigned short dh,
                               VARectangle *clips, unsigned int nc,
                               unsigned int flags) {
    (void)ctx;(void)s;(void)draw;(void)sx;(void)sy;(void)sw;(void)sh;
    (void)dx;(void)dy;(void)dw;(void)dh;(void)clips;(void)nc;(void)flags;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_QuerySurfaceError(VADriverContextP ctx, VASurfaceID s,
                                      VAStatus err, void **info) {
    (void)ctx;(void)s;(void)err; *info = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus rk_GetSurfaceAttributes(VADriverContextP ctx,
                                         VAConfigID config,
                                         VASurfaceAttrib *list,
                                         unsigned int n) {
    (void)ctx;(void)config;(void)list;(void)n;
    return VA_STATUS_SUCCESS;
}

/* ── driver init ─────────────────────────────────────────────── */

VAStatus __vaDriverInit_1_20(VADriverContextP ctx)  /* NOLINT */
{
    rk_log_init();
    LOG("__vaDriverInit_1_20: entry");
    RKDriver *d = calloc(1, sizeof(*d));
    if (!d) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (pthread_mutex_init(&d->object_lock, NULL) != 0) {
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->config_heap, RK_OBJECT_CONFIG)) {
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->context_heap, RK_OBJECT_CONTEXT)) {
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->surface_heap, RK_OBJECT_SURFACE)) {
        rk_object_heap_finish(&d->context_heap);
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->buffer_heap, RK_OBJECT_BUFFER)) {
        rk_object_heap_finish(&d->surface_heap);
        rk_object_heap_finish(&d->context_heap);
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!rk_object_heap_init(&d->image_heap, RK_OBJECT_IMAGE)) {
        rk_object_heap_finish(&d->buffer_heap);
        rk_object_heap_finish(&d->surface_heap);
        rk_object_heap_finish(&d->context_heap);
        rk_object_heap_finish(&d->config_heap);
        pthread_mutex_destroy(&d->object_lock);
        free(d);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    ctx->pDriverData = d;

    ctx->version_major        = VA_MAJOR_VERSION;
    ctx->version_minor        = VA_MINOR_VERSION;
    ctx->max_profiles         = 16;
    ctx->max_entrypoints      = 4;
    ctx->max_attributes       = 8;
    ctx->max_image_formats    = 4;
    ctx->max_subpic_formats   = 4;
    ctx->max_display_attributes = 4;
    ctx->str_vendor           = "Rockchip MPP VA-API Driver 0.1";

    struct VADriverVTable *v = ctx->vtable;
    v->vaTerminate                = rk_Terminate;
    v->vaQueryConfigProfiles      = rk_QueryConfigProfiles;
    v->vaQueryConfigEntrypoints   = rk_QueryConfigEntrypoints;
    v->vaGetConfigAttributes      = rk_GetConfigAttributes;
    v->vaCreateConfig             = rk_CreateConfig;
    v->vaDestroyConfig            = rk_DestroyConfig;
    v->vaQueryConfigAttributes    = rk_QueryConfigAttributes;
    v->vaCreateSurfaces           = rk_CreateSurfaces;
    v->vaDestroySurfaces          = rk_DestroySurfaces;
    v->vaCreateContext            = rk_CreateContext;
    v->vaDestroyContext           = rk_DestroyContext;
    v->vaCreateBuffer             = rk_CreateBuffer;
    v->vaBufferSetNumElements     = rk_BufferSetNumElements;
    v->vaMapBuffer                = rk_MapBuffer;
    v->vaUnmapBuffer              = rk_UnmapBuffer;
    v->vaDestroyBuffer            = rk_DestroyBuffer;
    v->vaBeginPicture             = rk_BeginPicture;
    v->vaRenderPicture            = rk_RenderPicture;
    v->vaEndPicture               = rk_EndPicture;
    v->vaSyncSurface              = rk_SyncSurface;
    v->vaQuerySurfaceStatus       = rk_QuerySurfaceStatus;
    v->vaQuerySurfaceError        = rk_QuerySurfaceError;
    v->vaPutSurface               = rk_PutSurface;
    v->vaQueryImageFormats        = rk_QueryImageFormats;
    v->vaCreateImage              = rk_CreateImage;
    v->vaDeriveImage              = rk_DeriveImage;
    v->vaDestroyImage             = rk_DestroyImage;
    v->vaSetImagePalette          = rk_SetImagePalette;
    v->vaGetImage                 = rk_GetImage;
    v->vaPutImage                 = rk_PutImage;
    v->vaQuerySubpictureFormats   = rk_QuerySubpicFmts;
    v->vaCreateSubpicture         = rk_CreateSubpicture;
    v->vaDestroySubpicture        = rk_DestroySubpicture;
    v->vaSetSubpictureImage       = rk_SetSubpicImage;
    v->vaSetSubpictureChromakey   = rk_SetSubpicChromakey;
    v->vaSetSubpictureGlobalAlpha = rk_SetSubpicAlpha;
    v->vaAssociateSubpicture      = rk_AssociateSubpic;
    v->vaDeassociateSubpicture    = rk_DeassociateSubpic;
    v->vaQueryDisplayAttributes   = rk_QueryDisplayAttrs;
    v->vaGetDisplayAttributes     = rk_GetDisplayAttrs;
    v->vaSetDisplayAttributes     = rk_SetDisplayAttrs;
    v->vaBufferInfo               = rk_BufferInfo;
    v->vaLockSurface              = rk_LockSurface;
    v->vaUnlockSurface            = rk_UnlockSurface;
    v->vaGetSurfaceAttributes     = rk_GetSurfaceAttributes;
    v->vaCreateSurfaces2          = rk_CreateSurfaces2;
    v->vaQuerySurfaceAttributes   = rk_QuerySurfaceAttrs;
    v->vaAcquireBufferHandle      = rk_AcquireBufferHandle;
    v->vaReleaseBufferHandle      = rk_ReleaseBufferHandle;
    v->vaCreateMFContext          = rk_CreateMFContext;
    v->vaMFAddContext             = rk_MFAddContext;
    v->vaMFReleaseContext         = rk_MFReleaseContext;
    v->vaMFSubmit                 = rk_MFSubmit;
    v->vaCreateBuffer2            = rk_CreateBuffer2;
    v->vaQueryProcessingRate      = rk_QueryProcessingRate;
    v->vaExportSurfaceHandle      = rk_ExportSurfaceHandle;
    v->vaSyncSurface2             = rk_SyncSurface2;
    v->vaSyncBuffer               = rk_SyncBuffer;
    v->vaCopy                     = rk_Copy;

    LOG("driver init OK — Rockchip RK3588 MPP");
    return VA_STATUS_SUCCESS;
}
