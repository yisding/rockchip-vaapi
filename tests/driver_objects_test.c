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
    };
    CHECK_STATUS(v->vaGetConfigAttributes(
                     ctx, VAProfileH264High, VAEntrypointEncSlice,
                     attrs, 4), VA_STATUS_SUCCESS);
    if (attrs[0].value != VA_RT_FORMAT_YUV420 ||
        attrs[1].value != (VA_RC_CQP | VA_RC_CBR | VA_RC_VBR) ||
        attrs[2].value != VA_ENC_PACKED_HEADER_NONE ||
        attrs[3].value != 1) {
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

    VAContextID invalid_context;
    CHECK_STATUS(v->vaCreateContext(ctx, config, 7681, 16, 0,
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
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, VAProfileHEVCMain, entrypoints, &count),
                 VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    if (setenv("RK_VAAPI_EXPERIMENTAL_ENCODE", "hevc", 1) != 0) {
        perror("setenv");
        exit(1);
    }
    CHECK_STATUS(v->vaQueryConfigEntrypoints(
                     ctx, VAProfileHEVCMain, entrypoints, &count),
                 VA_STATUS_SUCCESS);
    if (count != 1 || entrypoints[0] != VAEntrypointEncSlice) {
        fputs("HEVC encode must not expose experimental decode\n", stderr);
        exit(1);
    }

    VAConfigAttribValEncHEVCBlockSizes block_sizes;
    VAConfigAttrib attrs[] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncHEVCFeatures },
        { .type = VAConfigAttribEncHEVCBlockSizes },
    };
    CHECK_STATUS(v->vaGetConfigAttributes(
                     ctx, VAProfileHEVCMain, VAEntrypointEncSlice,
                     attrs, 4), VA_STATUS_SUCCESS);
    block_sizes.value = attrs[3].value;
    if (attrs[0].value != VA_RT_FORMAT_YUV420 ||
        attrs[1].value != (VA_RC_CQP | VA_RC_CBR | VA_RC_VBR) ||
        attrs[2].value != 0 ||
        block_sizes.bits.log2_max_coding_tree_block_size_minus3 != 3 ||
        block_sizes.bits.log2_min_coding_tree_block_size_minus3 != 3 ||
        block_sizes.bits.log2_min_luma_coding_block_size_minus3 != 0) {
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
    CHECK_STATUS(v->vaDestroyConfig(ctx, config), VA_STATUS_SUCCESS);
    if (unsetenv("RK_VAAPI_EXPERIMENTAL_PROFILES") != 0) {
        perror("unsetenv");
        exit(1);
    }
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
    VASurfaceID rejected_p010_surface;
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 15, 16,
                     &rejected_p010_surface, 1, &p010_format, 1),
                 VA_STATUS_ERROR_INVALID_PARAMETER);
    CHECK_STATUS(v->vaCreateSurfaces2(
                     ctx, VA_RT_FORMAT_YUV420_10, 16, 16, &p010_surface, 1,
                     &p010_format, 1),
                 VA_STATUS_SUCCESS);

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
        descriptor.layers[0].drm_format != VA_FOURCC_P010 ||
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
        descriptor.layers[0].drm_format != 0x20363152 ||
        descriptor.layers[1].drm_format != 0x36315247 ||
        descriptor.layers[0].pitch[0] != 32 ||
        descriptor.layers[1].pitch[0] != 32 ||
        descriptor.layers[1].offset[0] != 16u * 16u * 2u) {
        fputs("P010 split placeholder descriptor is invalid\n", stderr);
        exit(1);
    }
    close(descriptor.objects[0].fd);
    CHECK_STATUS(v->vaDestroySurfaces(ctx, &p010_surface, 1),
                 VA_STATUS_SUCCESS);

    MppBufferGroup p010_group = NULL;
    MppBuffer p010_buffer = NULL;
    const uint32_t p010_pitch = 16u * 2u;
    const size_t p010_size = p010_pitch * 16u * 3u / 2u;
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

    test_experimental_10bit_profiles(&vtable, &ctx);
    test_experimental_h264_encode(&vtable, &ctx);
    test_experimental_hevc_encode(&vtable, &ctx);
    test_configs(&vtable, &ctx, configs);
    test_buffers(&vtable, &ctx, buffers);
    test_images(&vtable, &ctx, images);
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
