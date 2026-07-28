/*
 * Minimal Rockchip MPP HEVC backend reproducer.
 *
 * This intentionally has no libva or driver dependencies. It submits one
 * complete Annex-B stream to MPP's split parser, drains every frame, and
 * reports errored/discarded output as a stream-specific backend failure.
 *
 * Exit status:
 *   0  clean decode with the expected frame count
 *   1  bad/missing output (stream-specific only after a clean control)
 *   2  invalid invocation or input
 *   3  MPP/device setup is unavailable
 *   4  MPP transport/runtime API failure
 */

#define _POSIX_C_SOURCE 200809L
#define MODULE_TAG "hevc_mpp_repro"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_vdec_cfg.h>

enum {
    REPRO_CLEAN = 0,
    REPRO_STREAM_FAILURE = 1,
    REPRO_USAGE = 2,
    REPRO_ENVIRONMENT = 3,
    REPRO_RUNTIME = 4,
    INPUT_PADDING = 256,
    OUTPUT_BUFFER_COUNT = 24,
    PUT_DEADLINE_MS = 5000,
    DRAIN_IDLE_DEADLINE_MS = 5000,
};

typedef struct {
    MppCtx context;
    MppApi *api;
    MppPacket packet;
    MppDecCfg config;
    MppBufferGroup frame_group;
    uint8_t *input;
    size_t input_size;
    unsigned frames;
    unsigned bad_frames;
    unsigned info_changes;
    bool saw_eos;
} Repro;

static const char *nal_name(unsigned type)
{
    switch (type) {
    case 32: return "VPS";
    case 33: return "SPS";
    case 34: return "PPS";
    case 35: return "AUD";
    case 36: return "EOS";
    case 37: return "EOB";
    case 38: return "FD";
    case 39: return "PREFIX_SEI";
    case 40: return "SUFFIX_SEI";
    default: return type <= 31 ? "VCL" : "OTHER";
    }
}

static bool find_start_code(const uint8_t *data, size_t size, size_t from,
                            size_t *offset, size_t *length)
{
    if (!data || !offset || !length)
        return false;
    for (size_t i = from; i + 3 <= size; i++) {
        if (data[i] || data[i + 1])
            continue;
        if (data[i + 2] == 1) {
            *offset = i;
            *length = 3;
            return true;
        }
        if (i + 4 <= size && !data[i + 2] && data[i + 3] == 1) {
            *offset = i;
            *length = 4;
            return true;
        }
    }
    return false;
}

static unsigned print_nal_inventory(const uint8_t *data, size_t size)
{
    size_t start = 0;
    size_t start_length = 0;
    unsigned count = 0;
    if (!find_start_code(data, size, 0, &start, &start_length)) {
        printf("NAL_INVENTORY count=0 bytes=%zu\n", size);
        return 0;
    }

    for (;;) {
        size_t payload = start + start_length;
        size_t next = size;
        size_t next_length = 0;
        bool have_next = find_start_code(data, size, payload, &next,
                                         &next_length);
        if (payload + 2 <= next) {
            unsigned type = (data[payload] >> 1) & 0x3fu;
            count++;
            printf("NAL index=%u type=%u name=%s offset=%zu size=%zu\n",
                   count, type, nal_name(type), start, next - payload);
        }
        if (!have_next)
            break;
        start = next;
        start_length = next_length;
    }
    printf("NAL_INVENTORY count=%u bytes=%zu\n", count, size);
    return count;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sleep_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
        ;
}

static bool parse_expected_frames(const char *text, unsigned *value)
{
    if (!text || !*text || !value)
        return false;

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > UINT32_MAX)
        return false;
    *value = (unsigned)parsed;
    return true;
}

static bool read_input(const char *path, uint8_t **data_out, size_t *size_out)
{
    FILE *input = fopen(path, "rb");
    if (!input) {
        fprintf(stderr, "INPUT_ERROR open=%s errno=%d (%s)\n",
                path, errno, strerror(errno));
        return false;
    }

    bool ok = false;
    if (fseek(input, 0, SEEK_END) != 0)
        goto done;
    long length = ftell(input);
    if (length <= 0 || (uintmax_t)length > SIZE_MAX - INPUT_PADDING)
        goto done;
    if (fseek(input, 0, SEEK_SET) != 0)
        goto done;

    size_t size = (size_t)length;
    uint8_t *data = calloc(1, size + INPUT_PADDING);
    if (!data)
        goto done;
    if (fread(data, 1, size, input) != size) {
        free(data);
        goto done;
    }

    *data_out = data;
    *size_out = size;
    ok = true;

done:
    if (!ok)
        fprintf(stderr, "INPUT_ERROR read=%s errno=%d (%s)\n",
                path, errno, strerror(errno));
    fclose(input);
    return ok;
}

static void cleanup(Repro *repro)
{
    if (repro->packet)
        mpp_packet_deinit(&repro->packet);
    if (repro->context) {
        if (repro->api)
            repro->api->reset(repro->context);
        mpp_destroy(repro->context);
    }
    if (repro->frame_group)
        mpp_buffer_group_put(repro->frame_group);
    if (repro->config)
        mpp_dec_cfg_deinit(repro->config);
    free(repro->input);
}

static int configure_decoder(Repro *repro)
{
    MPP_RET ret = mpp_create(&repro->context, &repro->api);
    if (ret != MPP_OK || !repro->context || !repro->api) {
        fprintf(stderr, "SETUP_ERROR stage=mpp_create ret=%d\n", ret);
        return REPRO_ENVIRONMENT;
    }

    ret = mpp_init(repro->context, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC);
    if (ret != MPP_OK) {
        fprintf(stderr, "SETUP_ERROR stage=mpp_init ret=%d\n", ret);
        return REPRO_ENVIRONMENT;
    }

    ret = mpp_dec_cfg_init(&repro->config);
    if (ret != MPP_OK || !repro->config) {
        fprintf(stderr, "SETUP_ERROR stage=cfg_init ret=%d\n", ret);
        return REPRO_ENVIRONMENT;
    }
    ret = repro->api->control(repro->context, MPP_DEC_GET_CFG,
                              repro->config);
    if (ret != MPP_OK ||
        mpp_dec_cfg_set_u32(repro->config, "base:split_parse", 1) != MPP_OK ||
        repro->api->control(repro->context, MPP_DEC_SET_CFG,
                            repro->config) != MPP_OK) {
        fprintf(stderr, "SETUP_ERROR stage=split_parser ret=%d\n", ret);
        return REPRO_ENVIRONMENT;
    }

    RK_S64 input_timeout_ms = 100;
    RK_S64 output_timeout_ms = 100;
    if (repro->api->control(repro->context, MPP_SET_INPUT_TIMEOUT,
                            &input_timeout_ms) != MPP_OK ||
        repro->api->control(repro->context, MPP_SET_OUTPUT_TIMEOUT,
                            &output_timeout_ms) != MPP_OK) {
        fputs("SETUP_ERROR stage=timeouts\n", stderr);
        return REPRO_ENVIRONMENT;
    }

    ret = mpp_packet_init(&repro->packet, repro->input, repro->input_size);
    if (ret != MPP_OK || !repro->packet) {
        fprintf(stderr, "SETUP_ERROR stage=packet_init ret=%d\n", ret);
        return REPRO_ENVIRONMENT;
    }
    mpp_packet_set_pos(repro->packet, repro->input);
    mpp_packet_set_length(repro->packet, repro->input_size);
    if (mpp_packet_set_eos(repro->packet) != MPP_OK) {
        fputs("SETUP_ERROR stage=packet_eos\n", stderr);
        return REPRO_ENVIRONMENT;
    }
    return REPRO_CLEAN;
}

static int accept_info_change(Repro *repro, MppFrame frame)
{
    size_t buffer_size = mpp_frame_get_buf_size(frame);
    unsigned width = mpp_frame_get_width(frame);
    unsigned height = mpp_frame_get_height(frame);
    unsigned horizontal_stride = mpp_frame_get_hor_stride(frame);
    unsigned vertical_stride = mpp_frame_get_ver_stride(frame);

    printf("INFO_CHANGE index=%u width=%u height=%u stride=%ux%u "
           "buffer_size=%zu\n",
           repro->info_changes, width, height, horizontal_stride,
           vertical_stride, buffer_size);
    repro->info_changes++;

    if (!buffer_size) {
        fputs("SETUP_ERROR stage=info_change zero_buffer_size\n", stderr);
        return REPRO_ENVIRONMENT;
    }
    if (!repro->frame_group) {
        MPP_RET ret = mpp_buffer_group_get_internal(
            &repro->frame_group, MPP_BUFFER_TYPE_DRM);
        if (ret != MPP_OK || !repro->frame_group) {
            fprintf(stderr,
                    "SETUP_ERROR stage=buffer_group_create ret=%d\n", ret);
            return REPRO_ENVIRONMENT;
        }
    }
    MPP_RET ret = mpp_buffer_group_limit_config(
        repro->frame_group, buffer_size, OUTPUT_BUFFER_COUNT);
    if (ret != MPP_OK ||
        repro->api->control(repro->context, MPP_DEC_SET_EXT_BUF_GROUP,
                            repro->frame_group) != MPP_OK ||
        repro->api->control(repro->context,
                            MPP_DEC_SET_INFO_CHANGE_READY, NULL) != MPP_OK) {
        fprintf(stderr, "SETUP_ERROR stage=info_change_ready ret=%d\n", ret);
        return REPRO_ENVIRONMENT;
    }
    return REPRO_CLEAN;
}

static int drain_one(Repro *repro, bool *made_progress)
{
    *made_progress = false;
    MppFrame frame = NULL;
    MPP_RET ret = repro->api->decode_get_frame(repro->context, &frame);
    if (ret == MPP_ERR_TIMEOUT)
        return REPRO_CLEAN;
    if (ret != MPP_OK) {
        fprintf(stderr, "RUNTIME_ERROR stage=decode_get_frame ret=%d\n", ret);
        return REPRO_RUNTIME;
    }
    if (!frame)
        return REPRO_CLEAN;

    *made_progress = true;
    int status = REPRO_CLEAN;
    if (mpp_frame_get_info_change(frame)) {
        status = accept_info_change(repro, frame);
    } else {
        unsigned errinfo = mpp_frame_get_errinfo(frame);
        unsigned discard = mpp_frame_get_discard(frame);
        bool eos = mpp_frame_get_eos(frame) != 0;
        repro->frames++;
        if (errinfo || discard)
            repro->bad_frames++;
        if (eos)
            repro->saw_eos = true;
        printf("FRAME index=%u errinfo=0x%x discard=0x%x eos=%u "
               "width=%u height=%u\n",
               repro->frames, errinfo, discard, eos ? 1u : 0u,
               mpp_frame_get_width(frame), mpp_frame_get_height(frame));
    }
    mpp_frame_deinit(&frame);
    return status;
}

static int submit_packet(Repro *repro)
{
    int64_t deadline = monotonic_ms() + PUT_DEADLINE_MS;
    for (;;) {
        MPP_RET ret = repro->api->decode_put_packet(repro->context,
                                                    repro->packet);
        if (ret == MPP_OK)
            return REPRO_CLEAN;
        if (ret != MPP_ERR_BUFFER_FULL && ret != MPP_ERR_TIMEOUT) {
            fprintf(stderr, "RUNTIME_ERROR stage=decode_put_packet ret=%d\n",
                    ret);
            return REPRO_RUNTIME;
        }

        bool progress = false;
        int status = drain_one(repro, &progress);
        if (status != REPRO_CLEAN)
            return status;
        if (!progress)
            sleep_ms(1);
        if (monotonic_ms() >= deadline) {
            fputs("RUNTIME_ERROR stage=decode_put_packet timeout\n", stderr);
            return REPRO_RUNTIME;
        }
    }
}

static int drain_to_eos(Repro *repro)
{
    int64_t idle_deadline = monotonic_ms() + DRAIN_IDLE_DEADLINE_MS;
    while (!repro->saw_eos) {
        bool progress = false;
        int status = drain_one(repro, &progress);
        if (status != REPRO_CLEAN)
            return status;
        if (progress) {
            idle_deadline = monotonic_ms() + DRAIN_IDLE_DEADLINE_MS;
            continue;
        }
        if (monotonic_ms() >= idle_deadline)
            break;
        sleep_ms(1);
    }
    return REPRO_CLEAN;
}

static void print_result(const char *status, const Repro *repro,
                         unsigned expected_frames, int api_status)
{
    printf("RESULT status=%s frames=%u expected=%u bad_frames=%u "
           "info_changes=%u eos=%u api_status=%d\n",
           status, repro->frames, expected_frames, repro->bad_frames,
           repro->info_changes, repro->saw_eos ? 1u : 0u, api_status);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--inspect") == 0) {
        uint8_t *input = NULL;
        size_t input_size = 0;
        if (!read_input(argv[2], &input, &input_size))
            return REPRO_USAGE;
        print_nal_inventory(input, input_size);
        free(input);
        return REPRO_CLEAN;
    }
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s INPUT.h265 EXPECTED_FRAMES\n"
                "       %s --inspect INPUT.h265\n",
                argv[0], argv[0]);
        return REPRO_USAGE;
    }

    unsigned expected_frames = 0;
    if (!parse_expected_frames(argv[2], &expected_frames)) {
        fprintf(stderr, "INPUT_ERROR invalid expected frame count: %s\n",
                argv[2]);
        return REPRO_USAGE;
    }

    Repro repro = {0};
    if (!read_input(argv[1], &repro.input, &repro.input_size))
        return REPRO_USAGE;
    print_nal_inventory(repro.input, repro.input_size);

    int status = configure_decoder(&repro);
    if (status != REPRO_CLEAN) {
        print_result("environment-error", &repro, expected_frames, status);
        cleanup(&repro);
        return status;
    }

    status = submit_packet(&repro);
    if (status == REPRO_CLEAN)
        status = drain_to_eos(&repro);

    int result;
    if (status != REPRO_CLEAN) {
        print_result(status == REPRO_ENVIRONMENT ? "environment-error"
                                                : "runtime-error",
                     &repro, expected_frames, status);
        result = status;
    } else if (repro.bad_frames || repro.frames != expected_frames ||
               !repro.saw_eos) {
        print_result("stream-error", &repro, expected_frames, status);
        result = REPRO_STREAM_FAILURE;
    } else {
        print_result("clean", &repro, expected_frames, status);
        result = REPRO_CLEAN;
    }

    cleanup(&repro);
    return result;
}
