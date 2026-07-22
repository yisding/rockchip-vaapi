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

static void put_profile0_prefix(BitWriter *writer, bool show_frame,
                                bool error_resilient)
{
    put_bits(writer, 2, 2); /* frame marker */
    put_bits(writer, 0, 2); /* profile */
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

    assert(rk_vp9_parse_profile0_frame(hidden, sizeof(hidden), &info));
    assert(!info.show_existing_frame);
    assert(!info.show_frame);
    assert(info.refresh_frame_flags == 0x14);

    assert(rk_vp9_parse_profile0_frame(visible, sizeof(visible), &info));
    assert(!info.show_existing_frame);
    assert(info.show_frame);
    assert(info.refresh_frame_flags == 0x11);

    assert(rk_vp9_parse_profile0_frame(keyframe, sizeof(keyframe), &info));
    assert(!info.show_existing_frame);
    assert(info.show_frame);
    assert(info.refresh_frame_flags == 0xff);
}

static void test_hidden_header_variants(void)
{
    BitWriter resilient = {0};
    BitWriter intra_only = {0};
    RKVP9FrameInfo info;

    put_profile0_prefix(&resilient, false, true);
    put_bits(&resilient, 0, 1); /* intra_only */
    put_bits(&resilient, 0x80, 8);
    assert(rk_vp9_parse_profile0_frame(
        resilient.data, (resilient.bit_pos + 7) / 8, &info));
    assert(!info.show_frame);
    assert(info.refresh_frame_flags == 0x80);

    put_profile0_prefix(&intra_only, false, false);
    put_bits(&intra_only, 1, 1); /* intra_only */
    put_bits(&intra_only, 2, 2); /* reset_frame_context */
    put_bits(&intra_only, 0x498342, 24); /* frame sync code */
    put_bits(&intra_only, 0x40, 8);
    assert(rk_vp9_parse_profile0_frame(
        intra_only.data, (intra_only.bit_pos + 7) / 8, &info));
    assert(!info.show_frame);
    assert(info.refresh_frame_flags == 0x40);
}

static void test_show_existing_builder(void)
{
    for (uint8_t slot = 0; slot < 8; slot++) {
        uint8_t packet = 0;
        RKVP9FrameInfo info;

        assert(rk_vp9_make_profile0_show_existing(slot, &packet));
        assert(packet == (uint8_t)(0x88u | slot));
        assert(rk_vp9_parse_profile0_frame(&packet, 1, &info));
        assert(info.show_existing_frame);
        assert(info.show_frame);
        assert(info.frame_to_show_map_idx == slot);
        assert(info.refresh_frame_flags == 0);
    }

    assert(!rk_vp9_make_profile0_show_existing(8, &(uint8_t){0}));
    assert(!rk_vp9_make_profile0_show_existing(0, NULL));
}

static void test_malformed_headers(void)
{
    RKVP9FrameInfo info;
    uint8_t bad_marker = 0;
    uint8_t profile_one = 0xa0;
    uint8_t truncated_hidden[] = {0x84, 0x02};

    memset(&info, 0xa5, sizeof(info));
    assert(!rk_vp9_parse_profile0_frame(NULL, 1, &info));
    assert(!rk_vp9_parse_profile0_frame(&bad_marker, 1, &info));
    assert(!rk_vp9_parse_profile0_frame(&profile_one, 1, &info));
    assert(!rk_vp9_parse_profile0_frame(truncated_hidden,
                                        sizeof(truncated_hidden), &info));

    /* Exercise every short prefix under ASan/UBSan and Valgrind. */
    for (uint32_t raw = 0; raw <= UINT16_MAX; raw++) {
        uint8_t bytes[] = {(uint8_t)(raw >> 8), (uint8_t)raw};
        (void)rk_vp9_parse_profile0_frame(bytes, sizeof(bytes), &info);
    }
}

int main(void)
{
    test_conformance_headers();
    test_hidden_header_variants();
    test_show_existing_builder();
    test_malformed_headers();
    puts("VP9 header tests: OK");
    return 0;
}
