#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_hevc.h>

#include <drm/drm_fourcc.h>
#include <rockchip/mpp_buffer.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "convert.h"
#include "driver_internal.h"

extern VAStatus __vaDriverInit_1_20(VADriverContextP ctx);

#define CONFIG_COUNT 32
#define CONTEXT_COUNT 9
#define SURFACE_COUNT 65
#define BUFFER_COUNT 300
#define IMAGE_COUNT 300

#define CHECK_STATUS(call, expected) do {                               \
    VAStatus actual_ = (call);                                         \
    if (actual_ != (expected)) {                                       \
        fprintf(stderr, "%s:%d: %s returned %d, expected %d\n",       \
                __FILE__, __LINE__, #call, actual_, (expected));       \
        exit(1);                                                       \
    }                                                                  \
} while (0)

static void test_configs(struct VADriverVTable *v, VADriverContextP ctx,
                         VAConfigID configs[CONFIG_COUNT])
{
    for (size_t i = 0; i < CONFIG_COUNT; i++) {
        CHECK_STATUS(v->vaCreateConfig(ctx, VAProfileH264Main,
                                      VAEntrypointVLD, NULL, 0,
                                      &configs[i]), VA_STATUS_SUCCESS);
    }

    VAProfile profile;
    VAEntrypoint entrypoint;
    int attributes;
    CHECK_STATUS(v->vaQueryConfigAttributes(ctx, configs[CONFIG_COUNT - 1],
                                            &profile, &entrypoint, NULL,
                                            &attributes),
                 VA_STATUS_SUCCESS);
    if (profile != VAProfileH264Main || entrypoint != VAEntrypointVLD ||
        attributes != 1) {
        fputs("config attributes changed unexpectedly\n", stderr);
        exit(1);
    }
}

static void test_experimental_h264_encode(struct VADriverVTable *v,
                                          VADriverContextP ctx)
{
    VAEntrypoint entrypoints[2];
    int count = 0;
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, VAProfileH264High, entrypoints, &count),
                 VA_STATUS_SUCCESS);
    if (count != 1 || entrypoints[0] != VAEntrypointVLD) {
        fputs("H.264 encode must be hidden by default\n", stderr);
        exit(1);
    }

    if (setenv("RK_VAAPI_EXPERIMENTAL_ENCODE", "h264", 1) != 0) {
        perror("setenv");
        exit(1);
    }
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, VAProfileH264High, entrypoints, &count),
                 VA_STATUS_SUCCESS);
    if (count != 2 || entrypoints[0] != VAEntrypointVLD ||
        entrypoints[1] != VAEntrypointEncSlice) {
        fputs("experimental H.264 encode entrypoint is invalid\n", stderr);
        exit(1);
    }

    VAConfigAttrib attrs[] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncPackedHeaders },
        { .type = VAConfigAttribEncMaxSlices },
        { .type = VAConfigAttribEncSliceStructure },
    };
    CHECK_STATUS(v->vaGetConfigAttributes(
                     ctx, VAProfileH264High, VAEntrypointEncSlice,
                     attrs, 5), VA_STATUS_SUCCESS);
    if (attrs[0].value != VA_RT_FORMAT_YUV420 ||
        attrs[1].value != (VA_RC_CQP | VA_RC_CBR | VA_RC_VBR) ||
        attrs[2].value != VA_ENC_PACKED_HEADER_NONE ||
        /* (RK_MAX_HEIGHT + 15) / 16, one slice per macroblock row. */
        attrs[3].value != 512 ||
        attrs[4].value !=
            (VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS |
             VA_ENC_SLICE_STRUCTURE_EQUAL_ROWS |
             VA_ENC_SLICE_STRUCTURE_EQUAL_MULTI_ROWS)) {
        fputs("experimental H.264 encode attributes are invalid\n", stderr);
        exit(1);
    }

    VAConfigAttrib selected[] = {
        { .type = VAConfigAttribRTFormat,
          .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CQP },
    };
    VAConfigID config;
    CHECK_STATUS(v->vaCreateConfig(
                     ctx, VAProfileH264High, VAEntrypointEncSlice,
                     selected, 2, &config), VA_STATUS_SUCCESS);
    unsigned int surface_attr_count = 0;
    CHECK_STATUS(v->vaQuerySurfaceAttributes(
                     ctx, config, NULL, &surface_attr_count),
                 VA_STATUS_SUCCESS);
    if (surface_attr_count != 10) {
        fputs("experimental encode surface attribute count is invalid\n",
              stderr);
        exit(1);
    }
    VASurfaceAttrib surface_attrs[10];
    CHECK_STATUS(v->vaQuerySurfaceAttributes(
                     ctx, config, surface_attrs, &surface_attr_count),
                 VA_STATUS_SUCCESS);
    if (surface_attrs[0].type != VASurfaceAttribPixelFormat ||
        surface_attrs[0].value.value.i != VA_FOURCC_NV12 ||
        surface_attrs[1].type != VASurfaceAttribPixelFormat ||
        surface_attrs[1].value.value.i != VA_FOURCC_I420 ||
        surface_attrs[2].type != VASurfaceAttribPixelFormat ||
        surface_attrs[2].value.value.i != VA_FOURCC_YV12 ||
        surface_attrs[3].value.value.i != VA_FOURCC_RGBA ||
        surface_attrs[4].value.value.i != VA_FOURCC_RGBX ||
        surface_attrs[5].value.value.i != VA_FOURCC_BGRA ||
        surface_attrs[6].value.value.i != VA_FOURCC_BGRX) {
        fputs("H.264 encode upload surface formats are invalid\n", stderr);
        exit(1);
    }
    VAProfile profile;
    VAEntrypoint entrypoint;
    VAConfigAttrib queried[2];
    int queried_count = 0;
    CHECK_STATUS(v->vaQueryConfigAttributes(
                     ctx, config, &profile, &entrypoint, queried,
                     &queried_count), VA_STATUS_SUCCESS);
    if (profile != VAProfileH264High ||
        entrypoint != VAEntrypointEncSlice || queried_count != 2 ||
        queried[0].value != VA_RT_FORMAT_YUV420 ||
        queried[1].value != VA_RC_CQP) {
        fputs("experimental H.264 encode config changed unexpectedly\n", stderr);
        exit(1);
    }

    /*
     * One past the limit, whatever the limit currently is. Unlike the
     * advertised attribute values above, this asserts a property -- that the
     * driver enforces its own cap -- so it should follow RK_MAX_WIDTH rather
     * than pin a literal that needs editing every time the cap moves.
     */
    VAContextID invalid_context;
    CHECK_STATUS(v->vaCreateContext(ctx, config, RK_MAX_WIDTH + 1, 16, 0,
                                   NULL, 0, &invalid_context),
                 VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);

    VASurfaceID mismatched_surface;
    CHECK_STATUS(v->vaCreateSurfaces2(ctx, VA_RT_FORMAT_YUV420, 32, 16,
                                     &mismatched_surface, 1, NULL, 0),
                 VA_STATUS_SUCCESS);
    VAContextID encode_context;
    CHECK_STATUS(v->vaCreateContext(ctx, config, 16, 16, 0, NULL, 0,
                                   &encode_context), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaBeginPicture(ctx, encode_context, mismatched_surface),
                 VA_STATUS_ERROR_INVALID_SURFACE);
    CHECK_STATUS(v->vaDestroyContext(ctx, encode_context), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &mismatched_surface, 1),
                 VA_STATUS_SUCCESS);

    CHECK_STATUS(v->vaDestroyConfig(ctx, config), VA_STATUS_SUCCESS);
    if (unsetenv("RK_VAAPI_EXPERIMENTAL_ENCODE") != 0) {
        perror("unsetenv");
        exit(1);
    }
}

static void test_experimental_hevc_encode(struct VADriverVTable *v,
                                          VADriverContextP ctx)
{
    VAEntrypoint entrypoints[2];
    int count = 0;
    /* HEVC Main decode ships; encode stays behind its own switch. */
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, VAProfileHEVCMain, entrypoints, &count),
                 VA_STATUS_SUCCESS);
    if (count != 1 || entrypoints[0] != VAEntrypointVLD) {
        fputs("HEVC Main must expose decode only by default\n", stderr);
        exit(1);
    }

    if (setenv("RK_VAAPI_EXPERIMENTAL_ENCODE", "hevc", 1) != 0) {
        perror("setenv");
        exit(1);
    }
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, VAProfileHEVCMain, entrypoints, &count),
                 VA_STATUS_SUCCESS);
    if (count != 2 || entrypoints[0] != VAEntrypointVLD ||
        entrypoints[1] != VAEntrypointEncSlice) {
        fputs("HEVC encode must be added alongside decode\n", stderr);
        exit(1);
    }

    VAConfigAttribValEncHEVCBlockSizes block_sizes;
    VAConfigAttrib attrs[] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncHEVCFeatures },
        { .type = VAConfigAttribEncHEVCBlockSizes },
        { .type = VAConfigAttribEncMaxSlices },
        { .type = VAConfigAttribEncSliceStructure },
    };
    CHECK_STATUS(v->vaGetConfigAttributes(
                     ctx, VAProfileHEVCMain, VAEntrypointEncSlice,
                     attrs, 6), VA_STATUS_SUCCESS);
    block_sizes.value = attrs[3].value;
    if (attrs[0].value != VA_RT_FORMAT_YUV420 ||
        attrs[1].value != (VA_RC_CQP | VA_RC_CBR | VA_RC_VBR) ||
        attrs[2].value != 0 ||
        block_sizes.bits.log2_max_coding_tree_block_size_minus3 != 3 ||
        block_sizes.bits.log2_min_coding_tree_block_size_minus3 != 3 ||
        block_sizes.bits.log2_min_luma_coding_block_size_minus3 != 0 ||
        /* (RK_MAX_HEIGHT + 63) / 64, one slice per CTU64 row. */
        attrs[4].value != 128 ||
        attrs[5].value !=
            (VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS |
             VA_ENC_SLICE_STRUCTURE_EQUAL_ROWS |
             VA_ENC_SLICE_STRUCTURE_EQUAL_MULTI_ROWS)) {
        fputs("experimental HEVC encode attributes are invalid\n", stderr);
        exit(1);
    }

    VAConfigAttrib selected[] = {
        { .type = VAConfigAttribRTFormat,
          .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CQP },
    };
    VAConfigID config;
    CHECK_STATUS(v->vaCreateConfig(
                     ctx, VAProfileHEVCMain, VAEntrypointEncSlice,
                     selected, 2, &config), VA_STATUS_SUCCESS);
    VAContextID context;
    CHECK_STATUS(v->vaCreateContext(ctx, config, 64, 80, 0, NULL, 0,
                                   &context), VA_STATUS_SUCCESS);
    VASurfaceAttrib i420 = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_FOURCC_I420,
        },
    };
    VASurfaceID visible_surface;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420, 64, 72,
                     &visible_surface, 1, &i420, 1), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaBeginPicture(ctx, context, visible_surface),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroyContext(ctx, context), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &visible_surface, 1),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroyConfig(ctx, config), VA_STATUS_SUCCESS);

    if (setenv("RK_VAAPI_EXPERIMENTAL_ENCODE", "h264,hevc", 1) != 0) {
        perror("setenv");
        exit(1);
    }
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, VAProfileH264High, entrypoints, &count),
                 VA_STATUS_SUCCESS);
    if (count != 2 || entrypoints[1] != VAEntrypointEncSlice) {
        fputs("combined encode opt-in did not retain H.264\n", stderr);
        exit(1);
    }
    if (unsetenv("RK_VAAPI_EXPERIMENTAL_ENCODE") != 0) {
        perror("unsetenv");
        exit(1);
    }
}

static void test_experimental_10bit_profile(struct VADriverVTable *v,
                                            VADriverContextP ctx,
                                            VAProfile profile,
                                            const char *token,
                                            const char *label)
{
    VAEntrypoint entrypoints[2];
    int entrypoint_count = 0;
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, profile, entrypoints, &entrypoint_count),
                 VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    if (setenv("RK_VAAPI_EXPERIMENTAL_PROFILES", token, 1) != 0) {
        perror("setenv");
        exit(1);
    }
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, profile, entrypoints, &entrypoint_count),
                 VA_STATUS_SUCCESS);
    if (entrypoint_count != 1 || entrypoints[0] != VAEntrypointVLD) {
        fprintf(stderr, "%s experimental entrypoint changed unexpectedly\n",
                label);
        exit(1);
    }

    VAConfigAttrib rt_format = { .type = VAConfigAttribRTFormat };
    CHECK_STATUS(v->vaGetConfigAttributes(
                     ctx, profile, VAEntrypointVLD, &rt_format, 1),
                 VA_STATUS_SUCCESS);
    if (rt_format.value != VA_RT_FORMAT_YUV420_10) {
        fprintf(stderr,
                "%s experimental RT format is not 10-bit 4:2:0\n", label);
        exit(1);
    }

    VAConfigID config;
    CHECK_STATUS(v->vaCreateConfig(ctx, profile,
                                   VAEntrypointVLD, &rt_format, 1, &config),
                 VA_STATUS_SUCCESS);

    VAContextID context;
    CHECK_STATUS(v->vaCreateContext(ctx, config, 64, 240, 0, NULL, 0,
                                   &context),
                 VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    CHECK_STATUS(v->vaCreateContext(ctx, config, 68, 240, 0, NULL, 0,
                                   &context), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroyContext(ctx, context), VA_STATUS_SUCCESS);

    CHECK_STATUS(v->vaDestroyConfig(ctx, config), VA_STATUS_SUCCESS);
    if (unsetenv("RK_VAAPI_EXPERIMENTAL_PROFILES") != 0) {
        perror("unsetenv");
        exit(1);
    }
}

static void test_rga_10bit_geometry(void)
{
    if (rk_rga_nv15_to_p010_geometry_supported(64, true) ||
        !rk_rga_nv15_to_p010_geometry_supported(68, true) ||
        !rk_rga_nv15_to_p010_geometry_supported(64, false)) {
        fputs("RGA AFBC 10-bit geometry guard is invalid\n", stderr);
        exit(1);
    }
}

static void test_rga_nv12_repack(void)
{
    enum {
        WIDTH = 352,
        HEIGHT = 288,
        SOURCE_STRIDE = 352,
        DESTINATION_STRIDE = 384,
    };
    const size_t source_size =
        (size_t)SOURCE_STRIDE * HEIGHT * 3u / 2u;
    const size_t destination_size =
        (size_t)DESTINATION_STRIDE * HEIGHT * 3u / 2u;
    MppBufferGroup group = NULL;
    MppBuffer source = NULL;
    MppBuffer destination = NULL;

    if (mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM) != MPP_OK ||
        mpp_buffer_get(group, &source, source_size) != MPP_OK ||
        mpp_buffer_get(group, &destination, destination_size) != MPP_OK) {
        fputs("failed to allocate NV12 repack test buffers\n", stderr);
        exit(1);
    }
    uint8_t *source_bytes = mpp_buffer_get_ptr(source);
    uint8_t *destination_bytes = mpp_buffer_get_ptr(destination);
    if (!source_bytes || !destination_bytes ||
        mpp_buffer_sync_begin(source) != MPP_OK ||
        mpp_buffer_sync_begin(destination) != MPP_OK) {
        fputs("failed to map NV12 repack test buffers\n", stderr);
        exit(1);
    }
    memset(source_bytes, 0, source_size);
    memset(destination_bytes, 0, destination_size);
    for (unsigned int row = 0; row < HEIGHT; row++)
        for (unsigned int column = 0; column < WIDTH; column++)
            source_bytes[(size_t)row * SOURCE_STRIDE + column] =
                (uint8_t)(row * 13u + column * 7u + 3u);
    uint8_t *source_uv = source_bytes +
                         (size_t)SOURCE_STRIDE * HEIGHT;
    for (unsigned int row = 0; row < HEIGHT / 2; row++)
        for (unsigned int column = 0; column < WIDTH; column++)
            source_uv[(size_t)row * SOURCE_STRIDE + column] =
                (uint8_t)(row * 5u + column * 11u + 19u);
    if (mpp_buffer_sync_end(source) != MPP_OK ||
        mpp_buffer_sync_end(destination) != MPP_OK ||
        !rk_repack_nv12(source, WIDTH, HEIGHT, SOURCE_STRIDE, HEIGHT,
                        destination, DESTINATION_STRIDE, HEIGHT) ||
        mpp_buffer_sync_ro_begin(destination) != MPP_OK) {
        fputs("NV12 repack operation failed\n", stderr);
        exit(1);
    }

    uint8_t *destination_uv = destination_bytes +
                              (size_t)DESTINATION_STRIDE * HEIGHT;
    for (unsigned int row = 0; row < HEIGHT; row++) {
        for (unsigned int column = 0; column < WIDTH; column++) {
            uint8_t expected = (uint8_t)(row * 13u + column * 7u + 3u);
            if (destination_bytes[(size_t)row * DESTINATION_STRIDE + column]
                    != expected) {
                fputs("NV12 luma repack changed active pixels\n", stderr);
                exit(1);
            }
        }
    }
    for (unsigned int row = 0; row < HEIGHT / 2; row++) {
        for (unsigned int column = 0; column < WIDTH; column++) {
            uint8_t expected = (uint8_t)(row * 5u + column * 11u + 19u);
            if (destination_uv[(size_t)row * DESTINATION_STRIDE + column]
                    != expected) {
                fputs("NV12 chroma repack changed active pixels\n", stderr);
                exit(1);
            }
        }
    }
    if (mpp_buffer_sync_ro_end(destination) != MPP_OK) {
        fputs("failed to finish NV12 repack readback\n", stderr);
        exit(1);
    }
    mpp_buffer_put(destination);
    mpp_buffer_put(source);
    mpp_buffer_group_put(group);
}

static void test_experimental_10bit_profiles(struct VADriverVTable *v,
                                             VADriverContextP ctx)
{
    test_experimental_10bit_profile(v, ctx, VAProfileHEVCMain10,
                                    "hevc-main10", "HEVC Main10");
    test_experimental_10bit_profile(v, ctx, VAProfileVP9Profile2,
                                    "vp9-profile2", "VP9 Profile 2");
}

static void test_buffers(struct VADriverVTable *v, VADriverContextP ctx,
                         VABufferID buffers[BUFFER_COUNT])
{
    for (size_t i = 0; i < BUFFER_COUNT; i++) {
        uint32_t initial = (uint32_t)i;
        CHECK_STATUS(v->vaCreateBuffer(ctx, VA_INVALID_ID,
                                      VASliceDataBufferType,
                                      sizeof(initial), 1, &initial,
                                      &buffers[i]), VA_STATUS_SUCCESS);
        uint32_t *mapped = NULL;
        CHECK_STATUS(v->vaMapBuffer(ctx, buffers[i], (void **)&mapped),
                     VA_STATUS_SUCCESS);
        if (!mapped || *mapped != initial) {
            fputs("mapped buffer contents changed unexpectedly\n", stderr);
            exit(1);
        }
        CHECK_STATUS(v->vaUnmapBuffer(ctx, buffers[i]), VA_STATUS_SUCCESS);
    }

    CHECK_STATUS(v->vaBufferSetNumElements(ctx, buffers[0], 0),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaBufferSetNumElements(ctx, buffers[0], 2),
                 VA_STATUS_SUCCESS);
    VABufferType type;
    unsigned int size;
    unsigned int elements;
    CHECK_STATUS(v->vaBufferInfo(ctx, buffers[0], &type, &size, &elements),
                 VA_STATUS_SUCCESS);
    if (type != VASliceDataBufferType || size != sizeof(uint32_t) ||
        elements != 2) {
        fputs("resized buffer metadata changed unexpectedly\n", stderr);
        exit(1);
    }

    VABufferID coded;
    CHECK_STATUS(v->vaCreateBuffer(ctx, VA_INVALID_ID,
                                   VAEncCodedBufferType, 4096, 1, NULL,
                                   &coded), VA_STATUS_SUCCESS);
    VACodedBufferSegment *segment = NULL;
    CHECK_STATUS(v->vaMapBuffer(ctx, coded, (void **)&segment),
                 VA_STATUS_SUCCESS);
    if (!segment || segment->size != 0 || !segment->buf || segment->next) {
        fputs("coded buffer mapping does not expose a VA segment\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaUnmapBuffer(ctx, coded), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroyBuffer(ctx, coded), VA_STATUS_SUCCESS);
}

static void test_images(struct VADriverVTable *v, VADriverContextP ctx,
                        VAImage images[IMAGE_COUNT])
{
    VAImageFormat queried[4];
    int queried_count = 0;
    CHECK_STATUS(v->vaQueryImageFormats(ctx, queried, &queried_count),
                 VA_STATUS_SUCCESS);
    if (queried_count != 4 ||
        queried[0].fourcc != VA_FOURCC_NV12 ||
        queried[1].fourcc != VA_FOURCC_P010 ||
        queried[2].fourcc != VA_FOURCC_I420 ||
        queried[3].fourcc != VA_FOURCC_YV12) {
        fputs("image format list changed unexpectedly\n", stderr);
        exit(1);
    }
    VAImageFormat odd_planar = queried[2];
    VAImage rejected_image;
    CHECK_STATUS(v->vaCreateImage(ctx, &odd_planar, 15, 16,
                                 &rejected_image),
                 VA_STATUS_ERROR_INVALID_PARAMETER);

    VAImageFormat format = {0};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;

    for (size_t i = 0; i < IMAGE_COUNT; i++) {
        unsigned int bytes_per_sample = (i & 1u) ? 2u : 1u;
        format.fourcc = (i & 1u) ? VA_FOURCC_P010 : VA_FOURCC_NV12;
        format.bits_per_pixel = (i & 1u) ? 24 : 12;
        CHECK_STATUS(v->vaCreateImage(ctx, &format, 16, 16, &images[i]),
                     VA_STATUS_SUCCESS);
        if (images[i].image_id == images[i].buf) {
            fputs("image and buffer handles must use distinct types\n", stderr);
            exit(1);
        }
        if (images[i].pitches[0] != 16u * bytes_per_sample ||
            images[i].offsets[1] != 256u * bytes_per_sample ||
            images[i].data_size != 384u * bytes_per_sample) {
            fputs("image layout changed unexpectedly\n", stderr);
            exit(1);
        }
        CHECK_STATUS(v->vaBufferInfo(ctx, (VABufferID)images[i].image_id,
                                    NULL, NULL, NULL),
                     VA_STATUS_ERROR_INVALID_BUFFER);
    }
}

/* vaDeriveImage aliases the surface's own DMA-BUF, and its buffer must hand
 * back a DRM PRIME fd. VLC's GL converters depend on both; without them VLC
 * drops its hardware decoder entirely. */
static void test_derive_image(struct VADriverVTable *v, VADriverContextP ctx)
{
    VASurfaceAttrib attrib = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value.type = VAGenericValueTypeInteger,
        .value.value.i = VA_FOURCC_NV12,
    };
    VASurfaceID surface;
    CHECK_STATUS(v->vaCreateSurfaces2(ctx, VA_RT_FORMAT_YUV420, 64, 64,
                                      &surface, 1, &attrib, 1),
                 VA_STATUS_SUCCESS);

    VAImage derived;
    CHECK_STATUS(v->vaDeriveImage(ctx, surface, &derived), VA_STATUS_SUCCESS);
    if (derived.format.fourcc != VA_FOURCC_NV12 || derived.num_planes != 2 ||
        derived.width != 64 || derived.height != 64 ||
        derived.pitches[0] < 64 ||
        derived.offsets[1] != derived.pitches[0] * 64u ||
        derived.data_size < derived.offsets[1] + derived.offsets[1] / 2) {
        fputs("derived image layout is not the surface's own layout\n", stderr);
        exit(1);
    }

    /* The mapping is the surface memory, so a write through it must be
     * visible through a second map of the same buffer. */
    unsigned char *pixels = NULL;
    CHECK_STATUS(v->vaMapBuffer(ctx, derived.buf, (void **)&pixels),
                 VA_STATUS_SUCCESS);
    pixels[0] = 0xa5;
    pixels[derived.offsets[1]] = 0x5a;
    CHECK_STATUS(v->vaUnmapBuffer(ctx, derived.buf), VA_STATUS_SUCCESS);
    pixels = NULL;
    CHECK_STATUS(v->vaMapBuffer(ctx, derived.buf, (void **)&pixels),
                 VA_STATUS_SUCCESS);
    if (pixels[0] != 0xa5 || pixels[derived.offsets[1]] != 0x5a) {
        fputs("derived image mapping does not alias the surface\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaUnmapBuffer(ctx, derived.buf), VA_STATUS_SUCCESS);

    VABufferInfo info = { .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME };
    CHECK_STATUS(v->vaAcquireBufferHandle(ctx, derived.buf, &info),
                 VA_STATUS_SUCCESS);
    if (info.mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME ||
        info.mem_size < derived.data_size || (int)info.handle < 0) {
        fputs("acquired buffer handle is not a usable DRM PRIME fd\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaReleaseBufferHandle(ctx, derived.buf),
                 VA_STATUS_SUCCESS);

    VABufferInfo unsupported = {
        .mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR
    };
    CHECK_STATUS(v->vaAcquireBufferHandle(ctx, derived.buf, &unsupported),
                 VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);

    CHECK_STATUS(v->vaDestroyImage(ctx, derived.image_id), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &surface, 1), VA_STATUS_SUCCESS);
}

static void test_surfaces(struct VADriverVTable *v, VADriverContextP ctx,
                          VASurfaceID surfaces[SURFACE_COUNT],
                          VAImage images[IMAGE_COUNT])
{
    CHECK_STATUS(v->vaCreateSurfaces2(ctx, VA_RT_FORMAT_YUV420, 16, 16,
                                     surfaces, SURFACE_COUNT, NULL, 0),
                 VA_STATUS_SUCCESS);

    uint8_t *upload = NULL;
    uint8_t *download = NULL;
    CHECK_STATUS(v->vaMapBuffer(ctx, images[0].buf, (void **)&upload),
                 VA_STATUS_SUCCESS);
    for (unsigned int i = 0; i < images[0].data_size; i++)
        upload[i] = (uint8_t)(i * 29u + 7u);
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[0].buf), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaPutImage(ctx, surfaces[0], images[0].image_id,
                              0, 0, 16, 16, 0, 0, 16, 16),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaGetImage(ctx, surfaces[0], 0, 0, 16, 16,
                              images[2].image_id), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaMapBuffer(ctx, images[0].buf, (void **)&upload),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaMapBuffer(ctx, images[2].buf, (void **)&download),
                 VA_STATUS_SUCCESS);
    if (memcmp(upload, download, images[0].data_size) != 0) {
        fputs("NV12 PutImage/GetImage round trip changed bytes\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[2].buf), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[0].buf), VA_STATUS_SUCCESS);

    const uint32_t planar_formats[] = { VA_FOURCC_I420, VA_FOURCC_YV12 };
    for (size_t index = 0; index < 2; index++) {
        VAImageFormat planar_format = {
            .fourcc = planar_formats[index],
            .byte_order = VA_LSB_FIRST,
            .bits_per_pixel = 12,
        };
        VAImage planar_source;
        VAImage planar_result;
        CHECK_STATUS(v->vaCreateImage(ctx, &planar_format, 16, 16,
                                     &planar_source), VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaCreateImage(ctx, &planar_format, 16, 16,
                                     &planar_result), VA_STATUS_SUCCESS);
        if (planar_source.num_planes != 3 ||
            planar_source.pitches[0] != 16 ||
            planar_source.pitches[1] != 8 ||
            planar_source.pitches[2] != 8 ||
            planar_source.offsets[1] != 256 ||
            planar_source.offsets[2] != 320 ||
            planar_source.data_size != 384) {
            fputs("planar image layout is invalid\n", stderr);
            exit(1);
        }

        VASurfaceAttrib pixel_format = {
            .type = VASurfaceAttribPixelFormat,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = (int)planar_formats[index],
            },
        };
        VASurfaceID planar_surface;
        CHECK_STATUS(v->vaCreateSurfaces2(
                         ctx, VA_RT_FORMAT_YUV420, 16, 16,
                         &planar_surface, 1, &pixel_format, 1),
                     VA_STATUS_SUCCESS);
        VASurfaceAttrib conflicting_formats[] = {
            pixel_format,
            pixel_format,
        };
        conflicting_formats[1].value.value.i =
            planar_formats[index] == VA_FOURCC_I420
                ? VA_FOURCC_YV12 : VA_FOURCC_I420;
        VASurfaceID rejected_surface;
        CHECK_STATUS(v->vaCreateSurfaces2(
                         ctx, VA_RT_FORMAT_YUV420, 16, 16,
                         &rejected_surface, 1, conflicting_formats, 2),
                     VA_STATUS_ERROR_INVALID_PARAMETER);
        CHECK_STATUS(v->vaMapBuffer(
                         ctx, planar_source.buf, (void **)&upload),
                     VA_STATUS_SUCCESS);
        for (unsigned int byte = 0; byte < planar_source.data_size; byte++)
            upload[byte] = (uint8_t)(byte * 17u + index * 31u + 3u);
        CHECK_STATUS(v->vaUnmapBuffer(ctx, planar_source.buf),
                     VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaPutImage(
                         ctx, planar_surface, planar_source.image_id,
                         0, 0, 16, 16, 0, 0, 16, 16),
                     VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaGetImage(
                         ctx, planar_surface, 0, 0, 16, 16,
                         planar_result.image_id), VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaMapBuffer(
                         ctx, planar_source.buf, (void **)&upload),
                     VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaMapBuffer(
                         ctx, planar_result.buf, (void **)&download),
                     VA_STATUS_SUCCESS);
        if (memcmp(upload, download, planar_source.data_size) != 0) {
            fputs("planar PutImage/GetImage round trip changed bytes\n",
                  stderr);
            exit(1);
        }
        CHECK_STATUS(v->vaUnmapBuffer(ctx, planar_result.buf),
                     VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaUnmapBuffer(ctx, planar_source.buf),
                     VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaDestroySurfaces(ctx, &planar_surface, 1),
                     VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaDestroyImage(ctx, planar_result.image_id),
                     VA_STATUS_SUCCESS);
        CHECK_STATUS(v->vaDestroyImage(ctx, planar_source.image_id),
                     VA_STATUS_SUCCESS);
    }

    VASurfaceStatus status;
    CHECK_STATUS(v->vaQuerySurfaceStatus(ctx, surfaces[SURFACE_COUNT - 1],
                                        &status), VA_STATUS_SUCCESS);
    if (status != VASurfaceReady) {
        fputs("unowned surface must report ready\n", stderr);
        exit(1);
    }

    VADRMPRIMESurfaceDescriptor descriptor = {0};
    CHECK_STATUS(v->vaExportSurfaceHandle(
                     ctx, surfaces[0],
                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_READ_ONLY |
                         VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                     &descriptor),
                 VA_STATUS_SUCCESS);
    if (descriptor.num_objects != 1 || descriptor.objects[0].fd < 0 ||
        descriptor.objects[0].size < 16u * 16u * 3u / 2u ||
        descriptor.num_layers != 2) {
        fputs("placeholder PRIME descriptor changed unexpectedly\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);

    VASurfaceAttrib p010_format = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_FOURCC_P010,
        },
    };
    VASurfaceID p010_surface;
    VASurfaceID aligned_p010_surface;
    VASurfaceID rejected_p010_surface;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 15, 16,
                     &rejected_p010_surface, 1, &p010_format, 1),
                 VA_STATUS_ERROR_INVALID_PARAMETER);
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16, &p010_surface, 1,
                     &p010_format, 1),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 128, 16,
                     &aligned_p010_surface, 1, &p010_format, 1),
                 VA_STATUS_SUCCESS);
    const uint32_t p010_pitch = 16u * 2u;
    const size_t p010_size = p010_pitch * 16u * 3u / 2u;

    CHECK_STATUS(v->vaMapBuffer(ctx, images[1].buf, (void **)&upload),
                 VA_STATUS_SUCCESS);
    for (unsigned int i = 0; i < images[1].data_size; i++)
        upload[i] = (uint8_t)(i * 17u + 3u);
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[1].buf), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaPutImage(ctx, p010_surface, images[1].image_id,
                              0, 0, 16, 16, 0, 0, 16, 16),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaGetImage(ctx, p010_surface, 0, 0, 16, 16,
                              images[3].image_id), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaMapBuffer(ctx, images[1].buf, (void **)&upload),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaMapBuffer(ctx, images[3].buf, (void **)&download),
                 VA_STATUS_SUCCESS);
    if (memcmp(upload, download, images[1].data_size) != 0) {
        fputs("P010 PutImage/GetImage round trip changed bytes\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[3].buf), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[1].buf), VA_STATUS_SUCCESS);

    VAImage derived_p010;
    CHECK_STATUS(v->vaDeriveImage(ctx, p010_surface, &derived_p010),
                 VA_STATUS_ERROR_OPERATION_FAILED);
    VAImage provisional_p010;
    CHECK_STATUS(v->vaDeriveImage(ctx, aligned_p010_surface,
                                 &provisional_p010), VA_STATUS_SUCCESS);
    if (provisional_p010.format.fourcc != VA_FOURCC_P010 ||
        provisional_p010.num_planes != 2 ||
        provisional_p010.pitches[0] != 256 ||
        provisional_p010.pitches[1] != 256 ||
        provisional_p010.offsets[1] != 256u * 16u ||
        provisional_p010.data_size != 256u * 16u * 3u / 2u) {
        fputs("aligned provisional P010 derived layout is invalid\n", stderr);
        exit(1);
    }
    RKSurface *provisional_surface =
        surface_acquire(drv_from_ctx(ctx), aligned_p010_surface);
    if (!provisional_surface) {
        fputs("failed to acquire provisional P010 surface\n", stderr);
        exit(1);
    }
    pthread_mutex_lock(&provisional_surface->lock);
    provisional_surface->hstride = 192;
    pthread_mutex_unlock(&provisional_surface->lock);
    CHECK_STATUS(v->vaMapBuffer(ctx, provisional_p010.buf, (void **)&download),
                 VA_STATUS_ERROR_OPERATION_FAILED);
    pthread_mutex_lock(&provisional_surface->lock);
    provisional_surface->hstride = 128;
    pthread_mutex_unlock(&provisional_surface->lock);
    rk_object_unref(&provisional_surface->base);
    CHECK_STATUS(v->vaDestroyImage(ctx, provisional_p010.image_id),
                 VA_STATUS_SUCCESS);
    RKSurface *completed_p010 =
        surface_acquire(drv_from_ctx(ctx), p010_surface);
    if (!completed_p010) {
        fputs("failed to acquire P010 surface for completed-decode test\n",
              stderr);
        exit(1);
    }
    pthread_mutex_lock(&completed_p010->lock);
    completed_p010->decoded = true;
    pthread_mutex_unlock(&completed_p010->lock);
    rk_object_unref(&completed_p010->base);
    CHECK_STATUS(v->vaDeriveImage(ctx, p010_surface, &derived_p010),
                 VA_STATUS_SUCCESS);
    if (derived_p010.format.fourcc != VA_FOURCC_P010 ||
        derived_p010.num_planes != 2 ||
        derived_p010.pitches[0] != p010_pitch ||
        derived_p010.pitches[1] != p010_pitch ||
        derived_p010.offsets[1] != p010_pitch * 16u ||
        derived_p010.data_size != p010_size) {
        fputs("completed P010 derived image layout is invalid\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaMapBuffer(ctx, derived_p010.buf, (void **)&download),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaUnmapBuffer(ctx, derived_p010.buf), VA_STATUS_SUCCESS);
    completed_p010 = surface_acquire(drv_from_ctx(ctx), p010_surface);
    if (!completed_p010) {
        fputs("failed to reacquire P010 surface for stale-layout test\n",
              stderr);
        exit(1);
    }
    pthread_mutex_lock(&completed_p010->lock);
    completed_p010->hstride = 32;
    pthread_mutex_unlock(&completed_p010->lock);
    CHECK_STATUS(v->vaMapBuffer(ctx, derived_p010.buf, (void **)&download),
                 VA_STATUS_ERROR_OPERATION_FAILED);
    pthread_mutex_lock(&completed_p010->lock);
    completed_p010->hstride = 16;
    pthread_mutex_unlock(&completed_p010->lock);
    rk_object_unref(&completed_p010->base);
    CHECK_STATUS(v->vaDestroyImage(ctx, derived_p010.image_id),
                 VA_STATUS_SUCCESS);

    memset(&descriptor, 0, sizeof(descriptor));
    CHECK_STATUS(v->vaExportSurfaceHandle(
                     ctx, p010_surface,
                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_READ_ONLY |
                         VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                     &descriptor),
                 VA_STATUS_SUCCESS);
    if (descriptor.fourcc != VA_FOURCC_P010 ||
        descriptor.num_objects != 1 || descriptor.objects[0].fd < 0 ||
        descriptor.objects[0].size < 16u * 16u * 3u ||
        descriptor.num_layers != 1 ||
        descriptor.layers[0].drm_format != DRM_FORMAT_P010 ||
        descriptor.layers[0].num_planes != 2 ||
        descriptor.layers[0].pitch[0] != 32 ||
        descriptor.layers[0].pitch[1] != 32 ||
        descriptor.layers[0].offset[1] != 16u * 16u * 2u) {
        fputs("P010 composed placeholder descriptor is invalid\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);

    memset(&descriptor, 0, sizeof(descriptor));
    CHECK_STATUS(v->vaExportSurfaceHandle(
                     ctx, p010_surface,
                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_READ_ONLY |
                         VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                     &descriptor),
                 VA_STATUS_SUCCESS);
    if (descriptor.fourcc != VA_FOURCC_P010 ||
        descriptor.num_layers != 2 ||
        descriptor.layers[0].drm_format != DRM_FORMAT_R16 ||
        descriptor.layers[1].drm_format != DRM_FORMAT_GR1616 ||
        descriptor.layers[0].pitch[0] != 32 ||
        descriptor.layers[1].pitch[0] != 32 ||
        descriptor.layers[1].offset[0] != 16u * 16u * 2u) {
        fputs("P010 split placeholder descriptor is invalid\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &p010_surface, 1),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &aligned_p010_surface, 1),
                 VA_STATUS_SUCCESS);

    MppBufferGroup p010_group = NULL;
    MppBuffer p010_buffer = NULL;
    if (mpp_buffer_group_get_internal(&p010_group, MPP_BUFFER_TYPE_DRM) !=
            MPP_OK ||
        mpp_buffer_get(p010_group, &p010_buffer, p010_size) != MPP_OK) {
        fputs("failed to allocate P010 import test buffer\n", stderr);
        exit(1);
    }
    uint8_t *p010_bytes = mpp_buffer_get_ptr(p010_buffer);
    if (!p010_bytes) {
        fputs("failed to map P010 import test buffer\n", stderr);
        exit(1);
    }
    for (size_t byte = 0; byte < p010_size; byte++)
        p010_bytes[byte] = (uint8_t)(byte * 23u + 11u);
    int p010_application_fd = dup(mpp_buffer_get_fd(p010_buffer));
    if (p010_application_fd < 0) {
        perror("dup");
        exit(1);
    }
    VADRMPRIMESurfaceDescriptor p010_descriptor = {
        .fourcc = VA_FOURCC_P010,
        .width = 16,
        .height = 16,
        .num_objects = 1,
        .objects = {
            {
                .fd = p010_application_fd,
                .size = (uint32_t)p010_size,
                .drm_format_modifier = DRM_FORMAT_MOD_LINEAR,
            },
        },
        .num_layers = 1,
        .layers = {
            {
                .drm_format = DRM_FORMAT_P010,
                .num_planes = 2,
                .object_index = { 0, 0 },
                .offset = { 0, p010_pitch * 16u },
                .pitch = { p010_pitch, p010_pitch },
            },
        },
    };
    VASurfaceAttrib p010_import_attributes[] = {
        p010_format,
        {
            .type = VASurfaceAttribMemoryType,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            },
        },
        {
            .type = VASurfaceAttribExternalBufferDescriptor,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypePointer,
                .value.p = &p010_descriptor,
            },
        },
    };
    p010_descriptor.layers[0].pitch[0]--;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16,
                     &p010_surface, 1, p010_import_attributes, 3),
                 VA_STATUS_ERROR_INVALID_PARAMETER);
    p010_descriptor.layers[0].pitch[0]++;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16,
                     &p010_surface, 1, p010_import_attributes, 3),
                 VA_STATUS_SUCCESS);
    VAImage imported_derived;
    CHECK_STATUS(v->vaDeriveImage(ctx, p010_surface, &imported_derived),
                 VA_STATUS_ERROR_OPERATION_FAILED);
    close(p010_application_fd);
    CHECK_STATUS(v->vaGetImage(ctx, p010_surface, 0, 0, 16, 16,
                              images[3].image_id), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaMapBuffer(ctx, images[3].buf, (void **)&download),
                 VA_STATUS_SUCCESS);
    if (memcmp(p010_bytes, download, p010_size) != 0) {
        fputs("imported P010 readback changed bytes\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[3].buf), VA_STATUS_SUCCESS);
    memset(&descriptor, 0, sizeof(descriptor));
    CHECK_STATUS(v->vaExportSurfaceHandle(
                     ctx, p010_surface,
                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_COMPOSED_LAYERS, &descriptor),
                 VA_STATUS_SUCCESS);
    if (descriptor.objects[0].fd < 0 ||
        descriptor.objects[0].size != p010_size ||
        descriptor.layers[0].drm_format != DRM_FORMAT_P010) {
        fputs("imported P010 surface did not retain its descriptor\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &p010_surface, 1),
                 VA_STATUS_SUCCESS);

    MppBuffer p010_y_buffer = NULL;
    MppBuffer p010_uv_buffer = NULL;
    const size_t p010_y_size = p010_pitch * 16u;
    const size_t p010_uv_size = p010_pitch * 8u;
    if (mpp_buffer_get(p010_group, &p010_y_buffer, p010_y_size) != MPP_OK ||
        mpp_buffer_get(p010_group, &p010_uv_buffer, p010_uv_size) != MPP_OK) {
        fputs("failed to allocate two-object P010 import buffers\n", stderr);
        exit(1);
    }
    uint8_t *p010_y_bytes = mpp_buffer_get_ptr(p010_y_buffer);
    uint8_t *p010_uv_bytes = mpp_buffer_get_ptr(p010_uv_buffer);
    if (!p010_y_bytes || !p010_uv_bytes) {
        fputs("failed to map two-object P010 import buffers\n", stderr);
        exit(1);
    }
    for (size_t byte = 0; byte < p010_y_size; byte++)
        p010_y_bytes[byte] = (uint8_t)(byte * 13u + 5u);
    for (size_t byte = 0; byte < p010_uv_size; byte++)
        p010_uv_bytes[byte] = (uint8_t)(byte * 31u + 9u);
    int p010_y_fd = dup(mpp_buffer_get_fd(p010_y_buffer));
    int p010_uv_fd = dup(mpp_buffer_get_fd(p010_uv_buffer));
    if (p010_y_fd < 0 || p010_uv_fd < 0) {
        perror("dup two-object P010");
        exit(1);
    }
    p010_descriptor.num_objects = 2;
    p010_descriptor.objects[0].fd = p010_y_fd;
    p010_descriptor.objects[0].size = (uint32_t)p010_y_size;
    p010_descriptor.objects[1].fd = p010_uv_fd;
    p010_descriptor.objects[1].size = (uint32_t)p010_uv_size;
    p010_descriptor.objects[1].drm_format_modifier =
        DRM_FORMAT_MOD_LINEAR;
    p010_descriptor.layers[0].object_index[1] = 1;
    p010_descriptor.layers[0].offset[1] = 0;
    p010_descriptor.objects[1].size = (uint32_t)p010_uv_size - 1u;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16,
                     &p010_surface, 1, p010_import_attributes, 3),
                 VA_STATUS_ERROR_INVALID_PARAMETER);
    p010_descriptor.objects[1].size = (uint32_t)p010_uv_size;
    p010_descriptor.layers[0].offset[1] = 2;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16,
                     &p010_surface, 1, p010_import_attributes, 3),
                 VA_STATUS_ERROR_INVALID_PARAMETER);
    p010_descriptor.layers[0].offset[1] = 0;
    p010_descriptor.objects[1].drm_format_modifier = 1;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16,
                     &p010_surface, 1, p010_import_attributes, 3),
                 VA_STATUS_ERROR_INVALID_PARAMETER);
    p010_descriptor.objects[1].drm_format_modifier =
        DRM_FORMAT_MOD_LINEAR;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16,
                     &p010_surface, 1, p010_import_attributes, 3),
                 VA_STATUS_SUCCESS);
    close(p010_y_fd);
    close(p010_uv_fd);
    CHECK_STATUS(v->vaGetImage(ctx, p010_surface, 0, 0, 16, 16,
                              images[3].image_id), VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaMapBuffer(ctx, images[3].buf, (void **)&download),
                 VA_STATUS_SUCCESS);
    if (memcmp(p010_y_bytes, download, p010_y_size) != 0 ||
        memcmp(p010_uv_bytes, download + p010_y_size,
               p010_uv_size) != 0) {
        fputs("two-object imported P010 readback changed bytes\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaUnmapBuffer(ctx, images[3].buf), VA_STATUS_SUCCESS);
    memset(&descriptor, 0, sizeof(descriptor));
    CHECK_STATUS(v->vaExportSurfaceHandle(
                     ctx, p010_surface,
                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_COMPOSED_LAYERS, &descriptor),
                 VA_STATUS_SUCCESS);
    if (descriptor.num_objects != 2 ||
        descriptor.objects[0].fd < 0 || descriptor.objects[1].fd < 0 ||
        descriptor.layers[0].object_index[0] != 0 ||
        descriptor.layers[0].object_index[1] != 1 ||
        descriptor.layers[0].offset[0] != 0 ||
        descriptor.layers[0].offset[1] != 0 ||
        descriptor.layers[0].pitch[0] != p010_pitch ||
        descriptor.layers[0].pitch[1] != p010_pitch) {
        fputs("two-object P010 export descriptor is invalid\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);
    close(descriptor.objects[1].fd);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &p010_surface, 1),
                 VA_STATUS_SUCCESS);
    mpp_buffer_put(p010_y_buffer);
    mpp_buffer_put(p010_uv_buffer);
    mpp_buffer_put(p010_buffer);
    mpp_buffer_group_put(p010_group);

    p010_format.value.value.i = VA_FOURCC_NV12;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16, &p010_surface, 1,
                     &p010_format, 1),
                 VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);

    MppBufferGroup rgb_group = NULL;
    MppBuffer rgb_buffer = NULL;
    const uint32_t rgb_pitch = 16u * 4u;
    const size_t rgb_size = rgb_pitch * 16u;
    if (mpp_buffer_group_get_internal(&rgb_group, MPP_BUFFER_TYPE_DRM) !=
            MPP_OK ||
        mpp_buffer_get(rgb_group, &rgb_buffer, rgb_size) != MPP_OK) {
        fputs("failed to allocate RGB import test buffer\n", stderr);
        exit(1);
    }
    int rgb_application_fd = dup(mpp_buffer_get_fd(rgb_buffer));
    if (rgb_application_fd < 0) {
        perror("dup");
        exit(1);
    }
    VADRMPRIMESurfaceDescriptor rgb_descriptor = {
        .fourcc = VA_FOURCC_BGRA,
        .width = 16,
        .height = 16,
        .num_objects = 1,
        .objects = {
            {
                .fd = rgb_application_fd,
                .size = (uint32_t)rgb_size,
                .drm_format_modifier = DRM_FORMAT_MOD_LINEAR,
            },
        },
        .num_layers = 1,
        .layers = {
            {
                .drm_format = DRM_FORMAT_ARGB8888,
                .num_planes = 1,
                .object_index = { 0 },
                .offset = { 0 },
                .pitch = { rgb_pitch },
            },
        },
    };
    VASurfaceAttrib rgb_attributes[] = {
        {
            .type = VASurfaceAttribPixelFormat,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_FOURCC_BGRA,
            },
        },
        {
            .type = VASurfaceAttribMemoryType,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            },
        },
        {
            .type = VASurfaceAttribExternalBufferDescriptor,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypePointer,
                .value.p = &rgb_descriptor,
            },
        },
    };
    VASurfaceID rgb_surface;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_RGB32, 16, 16, &rgb_surface, 1,
                     rgb_attributes, 3), VA_STATUS_SUCCESS);
    close(rgb_application_fd);

    memset(&descriptor, 0, sizeof(descriptor));
    CHECK_STATUS(v->vaExportSurfaceHandle(
                     ctx, rgb_surface,
                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_COMPOSED_LAYERS, &descriptor),
                 VA_STATUS_SUCCESS);
    if (descriptor.fourcc != VA_FOURCC_BGRA ||
        descriptor.num_objects != 1 || descriptor.objects[0].fd < 0 ||
        descriptor.objects[0].size != rgb_size ||
        descriptor.num_layers != 1 ||
        descriptor.layers[0].drm_format != DRM_FORMAT_ARGB8888 ||
        descriptor.layers[0].num_planes != 1 ||
        descriptor.layers[0].pitch[0] != rgb_pitch) {
        fputs("imported RGB export descriptor is invalid\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &rgb_surface, 1),
                 VA_STATUS_SUCCESS);

    int nv12_application_fd = dup(mpp_buffer_get_fd(rgb_buffer));
    if (nv12_application_fd < 0) {
        perror("dup");
        exit(1);
    }
    VADRMPRIMESurfaceDescriptor nv12_descriptor = {
        .fourcc = VA_FOURCC_NV12,
        .width = 16,
        .height = 16,
        .num_objects = 1,
        .objects = {
            {
                .fd = nv12_application_fd,
                .size = (uint32_t)rgb_size,
                .drm_format_modifier = DRM_FORMAT_MOD_LINEAR,
            },
        },
        .num_layers = 1,
        .layers = {
            {
                .drm_format = DRM_FORMAT_NV12,
                .num_planes = 2,
                .object_index = { 0, 0 },
                .offset = { 0, 16u * 16u },
                .pitch = { 16, 16 },
            },
        },
    };
    VASurfaceAttrib nv12_attributes[] = {
        {
            .type = VASurfaceAttribPixelFormat,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_FOURCC_NV12,
            },
        },
        rgb_attributes[1],
        rgb_attributes[2],
    };
    nv12_attributes[2].value.value.p = &nv12_descriptor;
    VASurfaceID imported_nv12_surface;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420, 16, 16,
                     &imported_nv12_surface, 1, nv12_attributes, 3),
                 VA_STATUS_SUCCESS);
    close(nv12_application_fd);
    memset(&descriptor, 0, sizeof(descriptor));
    CHECK_STATUS(v->vaExportSurfaceHandle(
                     ctx, imported_nv12_surface,
                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_COMPOSED_LAYERS, &descriptor),
                 VA_STATUS_SUCCESS);
    if (descriptor.fourcc != VA_FOURCC_NV12 ||
        descriptor.num_objects != 1 || descriptor.objects[0].fd < 0 ||
        descriptor.num_layers != 1 ||
        descriptor.layers[0].drm_format != DRM_FORMAT_NV12 ||
        descriptor.layers[0].num_planes != 2 ||
        descriptor.layers[0].offset[1] != 16u * 16u ||
        descriptor.layers[0].pitch[0] != 16 ||
        descriptor.layers[0].pitch[1] != 16) {
        fputs("imported NV12 export descriptor is invalid\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);
    CHECK_STATUS(v->vaDestroySurfaces(
                     ctx, &imported_nv12_surface, 1),
                 VA_STATUS_SUCCESS);

    rgb_attributes[1].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_RGB32, 16, 16, &rgb_surface, 1,
                     rgb_attributes, 1),
                 VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    rgb_attributes[1].value.value.i =
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    rgb_descriptor.num_objects = 2;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_RGB32, 16, 16, &rgb_surface, 1,
                     rgb_attributes, 3), VA_STATUS_ERROR_INVALID_PARAMETER);
    mpp_buffer_put(rgb_buffer);
    mpp_buffer_group_put(rgb_group);
}

static void test_contexts(struct VADriverVTable *v, VADriverContextP ctx,
                          VAConfigID config,
                          VASurfaceID surfaces[SURFACE_COUNT],
                          VAContextID contexts[CONTEXT_COUNT])
{
    for (size_t i = 0; i < CONTEXT_COUNT; i++) {
        VASurfaceID *targets = i == 0 ? surfaces : NULL;
        int target_count = i == 0 ? 2 : 0;
        CHECK_STATUS(v->vaCreateContext(ctx, config, 16, 16, 0,
                                       targets, target_count,
                                       &contexts[i]), VA_STATUS_SUCCESS);
    }

    VASurfaceID invalid_target = VA_INVALID_SURFACE;
    VAContextID invalid_context;
    CHECK_STATUS(v->vaCreateContext(ctx, config, 16, 16, 0,
                                   &invalid_target, 1, &invalid_context),
                 VA_STATUS_ERROR_INVALID_SURFACE);

    CHECK_STATUS(v->vaSyncSurface2(ctx, surfaces[2], 0),
                 VA_STATUS_SUCCESS);
    CHECK_STATUS(v->vaBeginPicture(ctx, contexts[0], surfaces[1]),
                 VA_STATUS_SUCCESS);
    VASurfaceStatus status;
    CHECK_STATUS(v->vaQuerySurfaceStatus(ctx, surfaces[1], &status),
                 VA_STATUS_SUCCESS);
    if (status != VASurfaceRendering) {
        fputs("in-flight surface must report rendering\n", stderr);
        exit(1);
    }
    CHECK_STATUS(v->vaSyncSurface2(ctx, surfaces[1], 0),
                 VA_STATUS_ERROR_TIMEDOUT);
}

int main(void)
{
    struct VADriverContext ctx = {0};
    struct VADriverVTable vtable = {0};
    ctx.vtable = &vtable;
    CHECK_STATUS(__vaDriverInit_1_20(&ctx), VA_STATUS_SUCCESS);

    VAConfigID configs[CONFIG_COUNT];
    VAContextID contexts[CONTEXT_COUNT];
    VASurfaceID surfaces[SURFACE_COUNT];
    VABufferID buffers[BUFFER_COUNT];
    VAImage images[IMAGE_COUNT];

    test_rga_10bit_geometry();
    test_rga_nv12_repack();
    test_experimental_10bit_profiles(&vtable, &ctx);
    test_experimental_h264_encode(&vtable, &ctx);
    test_experimental_hevc_encode(&vtable, &ctx);
    test_configs(&vtable, &ctx, configs);
    test_buffers(&vtable, &ctx, buffers);
    test_images(&vtable, &ctx, images);
    test_derive_image(&vtable, &ctx);
    test_surfaces(&vtable, &ctx, surfaces, images);
    test_contexts(&vtable, &ctx, configs[0], surfaces, contexts);

    CHECK_STATUS(vtable.vaDestroyBuffer(&ctx, configs[0]),
                 VA_STATUS_ERROR_INVALID_BUFFER);
    CHECK_STATUS(vtable.vaDestroyConfig(&ctx, (VAConfigID)buffers[0]),
                 VA_STATUS_ERROR_INVALID_CONFIG);
    CHECK_STATUS(vtable.vaDestroyImage(&ctx, (VAImageID)buffers[0]),
                 VA_STATUS_ERROR_INVALID_IMAGE);

    for (size_t i = 0; i < CONTEXT_COUNT; i++)
        CHECK_STATUS(vtable.vaDestroyContext(&ctx, contexts[i]),
                     VA_STATUS_SUCCESS);
    CHECK_STATUS(vtable.vaSyncSurface2(&ctx, surfaces[1], 0),
                 VA_STATUS_ERROR_DECODING_ERROR);
    CHECK_STATUS(vtable.vaDestroyContext(&ctx, contexts[0]),
                 VA_STATUS_ERROR_INVALID_CONTEXT);

    CHECK_STATUS(vtable.vaDestroySurfaces(&ctx, surfaces, SURFACE_COUNT),
                 VA_STATUS_SUCCESS);
    VASurfaceStatus status;
    CHECK_STATUS(vtable.vaQuerySurfaceStatus(&ctx, surfaces[0], &status),
                 VA_STATUS_ERROR_INVALID_SURFACE);

    VABufferID first_image_buffer = images[0].buf;
    VAImageID first_image = images[0].image_id;
    for (size_t i = 0; i < IMAGE_COUNT; i++)
        CHECK_STATUS(vtable.vaDestroyImage(&ctx, images[i].image_id),
                     VA_STATUS_SUCCESS);
    CHECK_STATUS(vtable.vaDestroyImage(&ctx, first_image),
                 VA_STATUS_ERROR_INVALID_IMAGE);
    CHECK_STATUS(vtable.vaBufferInfo(&ctx, first_image_buffer,
                                    NULL, NULL, NULL),
                 VA_STATUS_ERROR_INVALID_BUFFER);

    VABufferID first_buffer = buffers[0];
    for (size_t i = 0; i < BUFFER_COUNT; i++)
        CHECK_STATUS(vtable.vaDestroyBuffer(&ctx, buffers[i]),
                     VA_STATUS_SUCCESS);
    CHECK_STATUS(vtable.vaDestroyBuffer(&ctx, first_buffer),
                 VA_STATUS_ERROR_INVALID_BUFFER);

    VAConfigID first_config = configs[0];
    for (size_t i = 0; i < CONFIG_COUNT; i++)
        CHECK_STATUS(vtable.vaDestroyConfig(&ctx, configs[i]),
                     VA_STATUS_SUCCESS);
    CHECK_STATUS(vtable.vaDestroyConfig(&ctx, first_config),
                 VA_STATUS_ERROR_INVALID_CONFIG);

    CHECK_STATUS(vtable.vaTerminate(&ctx), VA_STATUS_SUCCESS);
    puts("driver object lifecycle tests: OK");
    return 0;
}
