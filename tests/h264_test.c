#include "h264.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit;
} BitReader;

static unsigned read_bit(BitReader *br)
{
    assert(br->bit < br->size * 8);
    unsigned value = (br->data[br->bit / 8] >> (7 - br->bit % 8)) & 1;
    br->bit++;
    return value;
}

static uint32_t read_bits(BitReader *br, unsigned count)
{
    uint32_t value = 0;
    while (count--)
        value = (value << 1) | read_bit(br);
    return value;
}

static uint32_t read_ue(BitReader *br)
{
    unsigned zeros = 0;
    while (!read_bit(br))
        zeros++;
    assert(zeros < 32);
    return ((UINT32_C(1) << zeros) - 1) + read_bits(br, zeros);
}

static int32_t read_se(BitReader *br)
{
    uint32_t code = read_ue(br);
    return (code & 1) ? (int32_t)((code + 1) / 2)
                      : -(int32_t)(code / 2);
}

static size_t unescape_nal(const uint8_t *nal, size_t nal_size,
                           uint8_t *rbsp, size_t rbsp_size)
{
    size_t out = 0;
    unsigned zeros = 0;

    assert(nal_size >= 5);
    assert(nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1);
    for (size_t i = 4; i < nal_size; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        assert(out < rbsp_size);
        rbsp[out++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

static void skip_pps_prefix(BitReader *br)
{
    assert(read_bits(br, 8) == 0x68);
    assert(read_ue(br) == 0); /* pic_parameter_set_id */
    assert(read_ue(br) == 0); /* seq_parameter_set_id */
    assert(read_bit(br) == 0); /* entropy_coding_mode_flag */
    assert(read_bit(br) == 0); /* pic_order_present_flag */
    assert(read_ue(br) == 0); /* num_slice_groups_minus1 */
    assert(read_ue(br) == 2); /* num_ref_idx_l0_default_minus1 */
    assert(read_ue(br) == 1); /* num_ref_idx_l1_default_minus1 */
    assert(read_bit(br) == 0); /* weighted_pred_flag */
    assert(read_bits(br, 2) == 0); /* weighted_bipred_idc */
    assert(read_se(br) == 0); /* pic_init_qp_minus26 */
    assert(read_se(br) == 0); /* pic_init_qs_minus26 */
    assert(read_se(br) == 0); /* chroma_qp_index_offset */
    assert(read_bit(br) == 0); /* deblocking_filter_control_present_flag */
    assert(read_bit(br) == 0); /* constrained_intra_pred_flag */
    assert(read_bit(br) == 0); /* redundant_pic_cnt_present_flag */
}

static void test_scaling_matrices(void)
{
    static const uint8_t scan4x4[16] = {
         0,  1,  4,  8,  5,  2,  3,  6,
         9, 12, 13, 10,  7, 11, 14, 15,
    };
    static const uint8_t scan8x8[64] = {
         0,  1,  8, 16,  9,  2,  3, 10,
        17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63,
    };
    VAPictureParameterBufferH264 pp = {0};
    VAIQMatrixBufferH264 iq = {0};
    uint8_t nal[2048];
    uint8_t rbsp[1024];

    pp.pic_fields.bits.transform_8x8_mode_flag = 1;
    for (size_t list = 0; list < 6; list++)
        for (size_t i = 0; i < 16; i++)
            iq.ScalingList4x4[list][i] =
                (uint8_t)(1 + (list * 29 + i * 7) % 255);
    for (size_t list = 0; list < 2; list++)
        for (size_t i = 0; i < 64; i++)
            iq.ScalingList8x8[list][i] =
                (uint8_t)(1 + (list * 43 + i * 11) % 255);

    int nal_size = h264_write_pps(nal, sizeof(nal), &pp, &iq, 2, 1);
    assert(nal_size > 0);
    BitReader br = {rbsp, unescape_nal(nal, (size_t)nal_size,
                                      rbsp, sizeof(rbsp)), 0};
    skip_pps_prefix(&br);
    assert(read_bit(&br) == 1); /* transform_8x8_mode_flag */
    assert(read_bit(&br) == 1); /* pic_scaling_matrix_present_flag */

    for (size_t list = 0; list < 8; list++) {
        const uint8_t *expected = list < 6 ? iq.ScalingList4x4[list]
                                           : iq.ScalingList8x8[list - 6];
        const uint8_t *scan = list < 6 ? scan4x4 : scan8x8;
        size_t count = list < 6 ? 16 : 64;
        int last = 8;

        assert(read_bit(&br) == 1); /* pic_scaling_list_present_flag */
        for (size_t i = 0; i < count; i++) {
            int next = (last + read_se(&br) + 256) & 255;
            assert(next == expected[scan[i]]);
            last = next;
        }
    }
    assert(read_se(&br) == 0); /* second_chroma_qp_index_offset */
}

static void test_matrix_absence_and_rejection(void)
{
    VAPictureParameterBufferH264 pp = {0};
    VAIQMatrixBufferH264 iq;
    uint8_t nal[2048];
    uint8_t rbsp[1024];

    pp.pic_fields.bits.transform_8x8_mode_flag = 1;
    int nal_size = h264_write_pps(nal, sizeof(nal), &pp, NULL, 2, 1);
    assert(nal_size > 0);
    BitReader br = {rbsp, unescape_nal(nal, (size_t)nal_size,
                                      rbsp, sizeof(rbsp)), 0};
    skip_pps_prefix(&br);
    assert(read_bit(&br) == 1);
    assert(read_bit(&br) == 0);
    assert(read_se(&br) == 0);

    memset(&iq, 16, sizeof(iq));
    iq.ScalingList4x4[3][7] = 0;
    assert(h264_write_pps(nal, sizeof(nal), &pp, &iq, 2, 1) < 0);
}

static unsigned sps_level_idc(const VAPictureParameterBufferH264 *pp,
                              int profile_idc)
{
    uint8_t nal[2048];
    uint8_t rbsp[1024];
    int nal_size = h264_write_sps(nal, sizeof(nal), pp, profile_idc);

    assert(nal_size > 0);
    size_t rbsp_size = unescape_nal(nal, (size_t)nal_size,
                                    rbsp, sizeof(rbsp));
    assert(rbsp_size >= 4);
    assert(rbsp[0] == 0x67);
    assert(rbsp[1] == (uint8_t)profile_idc);
    return rbsp[3];
}

static void test_level_derivation(void)
{
    VAPictureParameterBufferH264 pp = {0};

    /* 176x144, one reference: Level 1 frame and DPB limits. */
    pp.picture_width_in_mbs_minus1 = 10;
    pp.picture_height_in_mbs_minus1 = 8;
    pp.num_ref_frames = 1;
    assert(h264_derive_level_idc(&pp) == 10);
    assert(sps_level_idc(&pp, 77) == 10);

    /* The VA flag preserves the source SPS's >= 3.1 bi-pred constraint. */
    pp.seq_fields.bits.MinLumaBiPredSize8x8 = 1;
    assert(h264_derive_level_idc(&pp) == 31);
    pp.seq_fields.bits.MinLumaBiPredSize8x8 = 0;

    /* 1920x1088 (120x68 macroblocks) fits Level 4.0. */
    pp.picture_width_in_mbs_minus1 = 119;
    pp.picture_height_in_mbs_minus1 = 67;
    pp.num_ref_frames = 4;
    assert(h264_derive_level_idc(&pp) == 40);
    assert(sps_level_idc(&pp, 100) == 40);

    /* The same frame with five references exceeds the Level 4.0 DPB. */
    pp.num_ref_frames = 5;
    assert(h264_derive_level_idc(&pp) == 50);

    /* 3840x2160 rounds to 240x135 macroblocks and requires Level 5.1. */
    pp.picture_width_in_mbs_minus1 = 239;
    pp.picture_height_in_mbs_minus1 = 134;
    pp.num_ref_frames = 4;
    assert(h264_derive_level_idc(&pp) == 51);

    /* Out-of-range hostile dimensions saturate at the standardized maximum. */
    pp.picture_width_in_mbs_minus1 = UINT16_MAX;
    pp.picture_height_in_mbs_minus1 = UINT16_MAX;
    pp.num_ref_frames = UINT8_MAX;
    assert(h264_derive_level_idc(&pp) == 62);
    assert(sps_level_idc(&pp, 100) == 62);
    assert(h264_derive_level_idc(NULL) == 62);
}

int main(void)
{
    test_scaling_matrices();
    test_matrix_absence_and_rejection();
    test_level_derivation();
    puts("H.264 reconstruction tests: OK");
    return 0;
}
