#include "vp9.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t data[16];
    size_t bit_pos;
} BitWriter;

static void put_bits(BitWriter *writer, uint32_t value, unsigned count)
{
    assert(writer->bit_pos + count <= sizeof(writer->data) * 8);
    for (unsigned i = 0; i < count; i++) {
        unsigned shift = count - i - 1;
        if ((value >> shift) & 1u)
            writer->data[writer->bit_pos / 8] |=
                (uint8_t)(1u << (7 - writer->bit_pos % 8));
        writer->bit_pos++;
    }
}

static void put_profile_prefix(BitWriter *writer, uint8_t profile,
                               bool show_frame, bool error_resilient)
{
    put_bits(writer, 2, 2); /* frame marker */
    put_bits(writer, profile & 1u, 1);
    put_bits(writer, (profile >> 1) & 1u, 1);
    put_bits(writer, 0, 1); /* show_existing_frame */
    put_bits(writer, 1, 1); /* inter frame */
    put_bits(writer, show_frame, 1);
    put_bits(writer, error_resilient, 1);
}

static void test_conformance_headers(void)
{
    /* First three bytes of the hidden frame in the official libvpx
     * vp90-2-10-show-existing-frame2 vector. It refreshes slots 2 and 4. */
    static const uint8_t hidden[] = {0x84, 0x02, 0x80};
    static const uint8_t visible[] = {0x86, 0x04, 0x40};
    static const uint8_t keyframe[] = {0x82, 0x49, 0x83, 0x42};
    RKVP9FrameInfo info;

    assert(rk_vp9_parse_frame(hidden, sizeof(hidden), 0, &info));
    assert(!info.show_existing_frame);
    assert(!info.show_frame);
    assert(info.refresh_frame_flags == 0x14);

    assert(rk_vp9_parse_frame(visible, sizeof(visible), 0, &info));
    assert(!info.show_existing_frame);
    assert(info.show_frame);
    assert(info.refresh_frame_flags == 0x11);

    assert(rk_vp9_parse_frame(keyframe, sizeof(keyframe), 0, &info));
    assert(!info.show_existing_frame);
    assert(info.show_frame);
    assert(info.refresh_frame_flags == 0xff);
}

static void test_hidden_header_variants(void)
{
    BitWriter resilient = {0};
    BitWriter intra_only = {0};
    RKVP9FrameInfo info;

    put_profile_prefix(&resilient, 0, false, true);
    put_bits(&resilient, 0, 1); /* intra_only */
    put_bits(&resilient, 0x80, 8);
    assert(rk_vp9_parse_frame(resilient.data,
                              (resilient.bit_pos + 7) / 8, 0, &info));
    assert(!info.show_frame);
    assert(info.refresh_frame_flags == 0x80);

    put_profile_prefix(&intra_only, 0, false, false);
    put_bits(&intra_only, 1, 1); /* intra_only */
    put_bits(&intra_only, 2, 2); /* reset_frame_context */
    put_bits(&intra_only, 0x498342, 24); /* frame sync code */
    put_bits(&intra_only, 0x40, 8);
    assert(rk_vp9_parse_frame(intra_only.data,
                              (intra_only.bit_pos + 7) / 8, 0, &info));
    assert(!info.show_frame);
    assert(info.refresh_frame_flags == 0x40);
}

static void test_profile2_headers(void)
{
    static const uint8_t keyframe[] = {0x92, 0x49, 0x83, 0x42, 0x00};
    static const uint8_t keyframe_12bit[] = {
        0x92, 0x49, 0x83, 0x42, 0x80
    };
    BitWriter intra_only = {0};
    RKVP9FrameInfo info;

    assert(rk_vp9_parse_frame(keyframe, sizeof(keyframe), 2, &info));
    assert(info.show_frame);
    assert(info.refresh_frame_flags == 0xff);
    assert(!rk_vp9_parse_frame(keyframe, sizeof(keyframe), 0, &info));
    assert(!rk_vp9_parse_frame(keyframe_12bit, sizeof(keyframe_12bit),
                               2, &info));

    put_profile_prefix(&intra_only, 2, false, false);
    put_bits(&intra_only, 1, 1); /* intra_only */
    put_bits(&intra_only, 0, 2); /* reset_frame_context */
    put_bits(&intra_only, 0x498342, 24); /* frame sync code */
    put_bits(&intra_only, 0, 1); /* ten-bit */
    put_bits(&intra_only, 1, 3); /* BT.601 */
    put_bits(&intra_only, 0, 1); /* studio range */
    put_bits(&intra_only, 0x24, 8); /* refresh_frame_flags */
    assert(rk_vp9_parse_frame(intra_only.data,
                              (intra_only.bit_pos + 7) / 8, 2, &info));
    assert(!info.show_frame);
    assert(info.refresh_frame_flags == 0x24);
}

static void test_show_existing_builder(void)
{
    static const uint8_t profiles[] = {0, 2};
    for (size_t p = 0; p < sizeof(profiles); p++) {
        uint8_t profile = profiles[p];
        for (uint8_t slot = 0; slot < 8; slot++) {
            uint8_t packet = 0;
            RKVP9FrameInfo info;

            assert(rk_vp9_make_show_existing(profile, slot, &packet));
            assert(packet ==
                   (uint8_t)(0x88u | (profile == 2 ? 0x10u : 0u) | slot));
            assert(rk_vp9_parse_frame(&packet, 1, profile, &info));
            assert(info.show_existing_frame);
            assert(info.show_frame);
            assert(info.frame_to_show_map_idx == slot);
            assert(info.refresh_frame_flags == 0);
        }
    }

    assert(!rk_vp9_make_show_existing(1, 0, &(uint8_t){0}));
    assert(!rk_vp9_make_show_existing(0, 8, &(uint8_t){0}));
    assert(!rk_vp9_make_show_existing(0, 0, NULL));
}

static void test_malformed_headers(void)
{
    RKVP9FrameInfo info;
    uint8_t bad_marker = 0;
    uint8_t profile_one = 0xa0;
    uint8_t truncated_hidden[] = {0x84, 0x02};

    memset(&info, 0xa5, sizeof(info));
    assert(!rk_vp9_parse_frame(NULL, 1, 0, &info));
    assert(!rk_vp9_parse_frame(&bad_marker, 1, 0, &info));
    assert(!rk_vp9_parse_frame(&profile_one, 1, 0, &info));
    assert(!rk_vp9_parse_frame(truncated_hidden,
                               sizeof(truncated_hidden), 0, &info));

    /* Exercise every short prefix under ASan/UBSan and Valgrind. */
    for (uint32_t raw = 0; raw <= UINT16_MAX; raw++) {
        uint8_t bytes[] = {(uint8_t)(raw >> 8), (uint8_t)raw};
        (void)rk_vp9_parse_frame(bytes, sizeof(bytes), 0, &info);
        (void)rk_vp9_parse_frame(bytes, sizeof(bytes), 2, &info);
    }
}

int main(void)
{
    test_conformance_headers();
    test_hidden_header_variants();
    test_profile2_headers();
    test_show_existing_builder();
    test_malformed_headers();
    puts("VP9 header tests: OK");
    return 0;
}
