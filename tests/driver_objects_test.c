#include <va/va.h>
#include <va/va_backend.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        attributes != 0) {
        fputs("config attributes changed unexpectedly\n", stderr);
        exit(1);
    }
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
}

static void test_images(struct VADriverVTable *v, VADriverContextP ctx,
                        VAImage images[IMAGE_COUNT])
{
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
                          VASurfaceID surfaces[SURFACE_COUNT])
{
    CHECK_STATUS(v->vaCreateSurfaces2(ctx, VA_RT_FORMAT_YUV420, 16, 16,
                                     surfaces, SURFACE_COUNT, NULL, 0),
                 VA_STATUS_SUCCESS);
    VASurfaceStatus status;
    CHECK_STATUS(v->vaQuerySurfaceStatus(ctx, surfaces[SURFACE_COUNT - 1],
                                        &status), VA_STATUS_SUCCESS);
}

static void test_contexts(struct VADriverVTable *v, VADriverContextP ctx,
                          VAConfigID config,
                          VAContextID contexts[CONTEXT_COUNT])
{
    for (size_t i = 0; i < CONTEXT_COUNT; i++) {
        CHECK_STATUS(v->vaCreateContext(ctx, config, 16, 16, 0, NULL, 0,
                                       &contexts[i]), VA_STATUS_SUCCESS);
    }
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

    test_configs(&vtable, &ctx, configs);
    test_buffers(&vtable, &ctx, buffers);
    test_images(&vtable, &ctx, images);
    test_surfaces(&vtable, &ctx, surfaces);
    test_contexts(&vtable, &ctx, configs[0], contexts);

    CHECK_STATUS(vtable.vaDestroyBuffer(&ctx, configs[0]),
                 VA_STATUS_ERROR_INVALID_BUFFER);
    CHECK_STATUS(vtable.vaDestroyConfig(&ctx, (VAConfigID)buffers[0]),
                 VA_STATUS_ERROR_INVALID_CONFIG);
    CHECK_STATUS(vtable.vaDestroyImage(&ctx, (VAImageID)buffers[0]),
                 VA_STATUS_ERROR_INVALID_IMAGE);

    for (size_t i = 0; i < CONTEXT_COUNT; i++)
        CHECK_STATUS(vtable.vaDestroyContext(&ctx, contexts[i]),
                     VA_STATUS_SUCCESS);
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
