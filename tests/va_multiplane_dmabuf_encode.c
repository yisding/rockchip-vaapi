#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <rockchip/mpp_buffer.h>
#include <sys/ioctl.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>

#define WIDTH 320
#define HEIGHT 240
#define FRAMES 48
#define PITCH WIDTH
#define Y_SIZE ((size_t)PITCH * HEIGHT)
#define UV_SIZE ((size_t)PITCH * HEIGHT / 2)
#define CODED_BUFFER_SIZE (WIDTH * HEIGHT * 2)

static void fail_status(const char *operation, VAStatus status)
{
    fprintf(stderr, "%s failed: %d (%s)\n", operation, status,
            vaErrorStr(status));
    exit(1);
}

#define VA_CHECK(call) do {                                  \
    VAStatus status_ = (call);                               \
    if (status_ != VA_STATUS_SUCCESS)                        \
        fail_status(#call, status_);                         \
} while (0)

static void sync_dmabuf(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    int result;
    do {
        result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        perror("DMA_BUF_IOCTL_SYNC");
        exit(1);
    }
}

static void fill_nv12(uint8_t *y_plane, uint8_t *uv_plane,
                      unsigned int frame)
{
    for (unsigned int y = 0; y < HEIGHT; y++) {
        for (unsigned int x = 0; x < WIDTH; x++)
            y_plane[(size_t)y * PITCH + x] =
                (uint8_t)(16u + (x + 2u * y + 3u * frame) % 220u);
    }
    for (unsigned int y = 0; y < HEIGHT / 2; y++) {
        for (unsigned int x = 0; x < WIDTH; x += 2) {
            uv_plane[(size_t)y * PITCH + x] =
                (uint8_t)(64u + (y + 5u * frame) % 128u);
            uv_plane[(size_t)y * PITCH + x + 1] =
                (uint8_t)(64u + (x / 2u + 7u * frame) % 128u);
        }
    }
}

static void initialize_picture(VAPictureH264 *picture)
{
    memset(picture, 0, sizeof(*picture));
    picture->picture_id = VA_INVALID_SURFACE;
    picture->flags = VA_PICTURE_H264_INVALID;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUTPUT.h264 SOURCE.nv12\n", argv[0]);
        return 2;
    }

    FILE *bitstream = fopen(argv[1], "wb");
    FILE *source = fopen(argv[2], "wb");
    if (!bitstream || !source) {
        perror("fopen");
        return 1;
    }

    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open renderD128");
        return 1;
    }
    VADisplay display = vaGetDisplayDRM(drm_fd);
    if (!display) {
        fputs("vaGetDisplayDRM returned NULL\n", stderr);
        return 1;
    }
    int major = 0;
    int minor = 0;
    VA_CHECK(vaInitialize(display, &major, &minor));

    VAConfigAttrib config_attributes[] = {
        {
            .type = VAConfigAttribRTFormat,
            .value = VA_RT_FORMAT_YUV420,
        },
        {
            .type = VAConfigAttribRateControl,
            .value = VA_RC_CQP,
        },
    };
    VAConfigID config = VA_INVALID_ID;
    VA_CHECK(vaCreateConfig(display, VAProfileH264High,
                            VAEntrypointEncSlice, config_attributes, 2,
                            &config));

    MppBufferGroup group = NULL;
    MppBuffer y_buffer = NULL;
    MppBuffer uv_buffer = NULL;
    if (mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM) != MPP_OK ||
        mpp_buffer_get(group, &y_buffer, Y_SIZE) != MPP_OK ||
        mpp_buffer_get(group, &uv_buffer, UV_SIZE) != MPP_OK) {
        fputs("failed to allocate two-plane DMA-BUFs\n", stderr);
        return 1;
    }
    int y_application_fd = dup(mpp_buffer_get_fd(y_buffer));
    int uv_application_fd = dup(mpp_buffer_get_fd(uv_buffer));
    if (y_application_fd < 0 || uv_application_fd < 0) {
        perror("dup two-plane DMA-BUFs");
        return 1;
    }

    VADRMPRIMESurfaceDescriptor descriptor = {
        .fourcc = VA_FOURCC_NV12,
        .width = WIDTH,
        .height = HEIGHT,
        .num_objects = 2,
        .objects = {
            {
                .fd = y_application_fd,
                .size = Y_SIZE,
                .drm_format_modifier = DRM_FORMAT_MOD_LINEAR,
            },
            {
                .fd = uv_application_fd,
                .size = UV_SIZE,
                .drm_format_modifier = DRM_FORMAT_MOD_LINEAR,
            },
        },
        .num_layers = 1,
        .layers = {
            {
                .drm_format = DRM_FORMAT_NV12,
                .num_planes = 2,
                .object_index = { 0, 1 },
                .offset = { 0, 0 },
                .pitch = { PITCH, PITCH },
            },
        },
    };
    VASurfaceAttrib surface_attributes[] = {
        {
            .type = VASurfaceAttribPixelFormat,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_FOURCC_NV12,
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
                .value.p = &descriptor,
            },
        },
        {
            .type = VASurfaceAttribUsageHint,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {
                .type = VAGenericValueTypeInteger,
                .value.i = VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER,
            },
        },
    };
    VASurfaceID surface = VA_INVALID_SURFACE;
    VA_CHECK(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT,
                              &surface, 1, surface_attributes, 4));
    close(y_application_fd);
    close(uv_application_fd);

    VAContextID context = VA_INVALID_ID;
    VA_CHECK(vaCreateContext(display, config, WIDTH, HEIGHT,
                             VA_PROGRESSIVE, NULL, 0, &context));

    VAEncSequenceParameterBufferH264 sequence = {
        .level_idc = 31,
        .intra_period = 24,
        .intra_idr_period = 24,
        .ip_period = 1,
        .max_num_ref_frames = 1,
        .picture_width_in_mbs = WIDTH / 16,
        .picture_height_in_mbs = HEIGHT / 16,
        .seq_fields.bits = {
            .chroma_format_idc = 1,
            .frame_mbs_only_flag = 1,
            .direct_8x8_inference_flag = 1,
            .log2_max_frame_num_minus4 = 4,
            .pic_order_cnt_type = 0,
            .log2_max_pic_order_cnt_lsb_minus4 = 4,
        },
    };
    VABufferID sequence_buffer = VA_INVALID_ID;
    VA_CHECK(vaCreateBuffer(display, context,
                            VAEncSequenceParameterBufferType,
                            sizeof(sequence), 1, &sequence,
                            &sequence_buffer));

    uint8_t *y_bytes = mpp_buffer_get_ptr(y_buffer);
    uint8_t *uv_bytes = mpp_buffer_get_ptr(uv_buffer);
    if (!y_bytes || !uv_bytes) {
        fputs("two-plane DMA-BUFs are not CPU-mappable\n", stderr);
        return 1;
    }
    for (unsigned int frame = 0; frame < FRAMES; frame++) {
        int y_fd = mpp_buffer_get_fd(y_buffer);
        int uv_fd = mpp_buffer_get_fd(uv_buffer);
        sync_dmabuf(y_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        sync_dmabuf(uv_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        fill_nv12(y_bytes, uv_bytes, frame);
        if (fwrite(y_bytes, Y_SIZE, 1, source) != 1 ||
            fwrite(uv_bytes, UV_SIZE, 1, source) != 1) {
            perror("fwrite source");
            return 1;
        }
        sync_dmabuf(uv_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        sync_dmabuf(y_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

        VABufferID coded_buffer = VA_INVALID_ID;
        VA_CHECK(vaCreateBuffer(display, context, VAEncCodedBufferType,
                                CODED_BUFFER_SIZE, 1, NULL, &coded_buffer));

        VAEncPictureParameterBufferH264 picture = {0};
        picture.CurrPic.picture_id = surface;
        picture.CurrPic.frame_idx = frame;
        picture.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        for (size_t i = 0; i < 16; i++)
            initialize_picture(&picture.ReferenceFrames[i]);
        picture.coded_buf = coded_buffer;
        picture.frame_num = (uint16_t)frame;
        picture.pic_init_qp = 24;
        picture.pic_fields.bits.idr_pic_flag = frame % 24u == 0;
        picture.pic_fields.bits.reference_pic_flag = 1;
        picture.pic_fields.bits.entropy_coding_mode_flag = 1;
        picture.pic_fields.bits.transform_8x8_mode_flag = 1;
        picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;

        VAEncSliceParameterBufferH264 slice = {0};
        slice.num_macroblocks = (WIDTH / 16) * (HEIGHT / 16);
        slice.macroblock_info = VA_INVALID_ID;
        slice.slice_type = picture.pic_fields.bits.idr_pic_flag ? 2 : 0;
        slice.idr_pic_id = (uint16_t)(frame / 24u);
        slice.pic_order_cnt_lsb = (uint16_t)(frame * 2u);
        for (size_t i = 0; i < 32; i++) {
            initialize_picture(&slice.RefPicList0[i]);
            initialize_picture(&slice.RefPicList1[i]);
        }

        VABufferID picture_buffer = VA_INVALID_ID;
        VABufferID slice_buffer = VA_INVALID_ID;
        VA_CHECK(vaCreateBuffer(display, context,
                                VAEncPictureParameterBufferType,
                                sizeof(picture), 1, &picture,
                                &picture_buffer));
        VA_CHECK(vaCreateBuffer(display, context,
                                VAEncSliceParameterBufferType,
                                sizeof(slice), 1, &slice, &slice_buffer));

        VA_CHECK(vaBeginPicture(display, context, surface));
        if (frame == 0)
            VA_CHECK(vaRenderPicture(display, context, &sequence_buffer, 1));
        VABufferID render_buffers[] = { picture_buffer, slice_buffer };
        VA_CHECK(vaRenderPicture(display, context, render_buffers, 2));
        VA_CHECK(vaEndPicture(display, context));

        VACodedBufferSegment *segment = NULL;
        VA_CHECK(vaMapBuffer(display, coded_buffer, (void **)&segment));
        if (!segment || !segment->buf || !segment->size ||
            segment->status != 0 ||
            fwrite(segment->buf, segment->size, 1, bitstream) != 1) {
            fputs("invalid encoded segment\n", stderr);
            return 1;
        }
        VA_CHECK(vaUnmapBuffer(display, coded_buffer));
        VA_CHECK(vaDestroyBuffer(display, slice_buffer));
        VA_CHECK(vaDestroyBuffer(display, picture_buffer));
        VA_CHECK(vaDestroyBuffer(display, coded_buffer));
    }

    VA_CHECK(vaDestroyBuffer(display, sequence_buffer));
    VA_CHECK(vaDestroyContext(display, context));
    VA_CHECK(vaDestroySurfaces(display, &surface, 1));
    VA_CHECK(vaDestroyConfig(display, config));
    VA_CHECK(vaTerminate(display));
    mpp_buffer_put(uv_buffer);
    mpp_buffer_put(y_buffer);
    mpp_buffer_group_put(group);
    close(drm_fd);
    fclose(source);
    fclose(bitstream);
    printf("two-object NV12 DRM PRIME encode: %d frames, %dx%d\n",
           FRAMES, WIDTH, HEIGHT);
    return 0;
}
