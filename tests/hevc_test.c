#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bs.h"
#include "hevc.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit;
} BitReader;

static uint32_t read_bit(BitReader *br)
{
    assert(br->bit < br->size * 8);
    uint32_t value = (br->data[br->bit / 8] >> (7 - br->bit % 8)) & 1;
    br->bit++;
    return value;
}

static uint32_t read_bits(BitReader *br, unsigned int count)
{
    uint32_t value = 0;
    while (count--)
        value = (value << 1) | read_bit(br);
    return value;
}

static uint32_t read_ue(BitReader *br)
{
    unsigned int zeros = 0;
    while (!read_bit(br))
        zeros++;
    assert(zeros < 32);
    return ((UINT32_C(1) << zeros) - 1) + read_bits(br, zeros);
}

static int32_t read_se(BitReader *br)
{
    uint32_t code = read_ue(br);
    return code & 1 ? (int32_t)((code + 1) / 2)
                    : -(int32_t)(code / 2);
}

static size_t unescape_nal(const uint8_t *nal, size_t nal_size,
                           uint8_t *rbsp, size_t rbsp_size)
{
    size_t out = 0;
    unsigned int zeros = 0;

    for (size_t i = 0; i < nal_size; i++) {
        if (zeros >= 2 && nal[i] == 3 && i + 1 < nal_size && nal[i + 1] <= 3) {
            zeros = 0;
            continue;
        }
        assert(out < rbsp_size);
        rbsp[out++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    return out;
}

static const uint8_t *find_nal(const uint8_t *bundle, size_t bundle_size,
                               unsigned int type, size_t *nal_size)
{
    for (size_t i = 0; i + 6 <= bundle_size; i++) {
        if (bundle[i] || bundle[i + 1] || bundle[i + 2] || bundle[i + 3] != 1)
            continue;
        const uint8_t *nal = bundle + i + 4;
        if (((nal[0] >> 1) & 0x3f) != type)
            continue;
        size_t end = i + 4;
        while (end + 4 <= bundle_size &&
               !(bundle[end] == 0 && bundle[end + 1] == 0 &&
                 bundle[end + 2] == 0 && bundle[end + 3] == 1))
            end++;
        if (end + 4 > bundle_size)
            end = bundle_size;
        *nal_size = end - (i + 4);
        return nal;
    }
    return NULL;
}

static void init_picture(VAPictureParameterBufferHEVC *pp)
{
    memset(pp, 0, sizeof(*pp));
    pp->CurrPic.picture_id = 1;
    pp->CurrPic.pic_order_cnt = 10;
    for (size_t i = 0; i < 15; i++) {
        pp->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pp->ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    pp->pic_width_in_luma_samples = 416;
    pp->pic_height_in_luma_samples = 240;
    pp->pic_fields.bits.chroma_format_idc = 1;
    pp->pic_fields.bits.amp_enabled_flag = 1;
    pp->pic_fields.bits.strong_intra_smoothing_enabled_flag = 1;
    pp->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    pp->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    pp->sps_max_dec_pic_buffering_minus1 = 4;
    pp->log2_min_luma_coding_block_size_minus3 = 0;
    pp->log2_diff_max_min_luma_coding_block_size = 3;
    pp->log2_min_transform_block_size_minus2 = 0;
    pp->log2_diff_max_min_transform_block_size = 3;
    pp->max_transform_hierarchy_depth_inter = 2;
    pp->max_transform_hierarchy_depth_intra = 2;
    pp->log2_max_pic_order_cnt_lsb_minus4 = 4;
    pp->num_ref_idx_l0_default_active_minus1 = 1;
    pp->num_ref_idx_l1_default_active_minus1 = 0;
    pp->log2_parallel_merge_level_minus2 = 0;
}

static void add_reference(VAPictureParameterBufferHEVC *pp, size_t index,
                          VASurfaceID id, int poc, uint32_t flags)
{
    assert(index < 15);
    pp->ReferenceFrames[index].picture_id = id;
    pp->ReferenceFrames[index].pic_order_cnt = poc;
    pp->ReferenceFrames[index].flags = flags;
}

static void check_nal_header(BitReader *br, unsigned int type)
{
    assert(read_bit(br) == 0);
    assert(read_bits(br, 6) == type);
    assert(read_bits(br, 6) == 0);
    assert(read_bits(br, 3) == 1);
}

static void check_ptl(BitReader *br, int profile_idc, int level_idc)
{
    assert(read_bits(br, 2) == 0);
    assert(read_bit(br) == 0);
    assert(read_bits(br, 5) == (uint32_t)profile_idc);
    for (int i = 0; i < 32; i++)
        assert(read_bit(br) == (uint32_t)(i == profile_idc));
    (void)read_bits(br, 4);
    (void)read_bits(br, 24);
    (void)read_bits(br, 20);
    assert(read_bits(br, 8) == (uint32_t)level_idc);
}

static void skip_sps_to_scaling(BitReader *br, int profile_idc)
{
    check_nal_header(br, 33);
    assert(read_bits(br, 4) == 0);
    assert(read_bits(br, 3) == 0);
    assert(read_bit(br) == 1);
    check_ptl(br, profile_idc, 60);
    assert(read_ue(br) == 0);
    assert(read_ue(br) == 1);
    assert(read_ue(br) == 416);
    assert(read_ue(br) == 240);
    assert(read_bit(br) == 0);
    assert(read_ue(br) == (uint32_t)(profile_idc == 2 ? 2 : 0));
    assert(read_ue(br) == (uint32_t)(profile_idc == 2 ? 2 : 0));
    assert(read_ue(br) == 4);
    assert(read_bit(br) == 0);
    assert(read_ue(br) == 4);
    assert(read_ue(br) == 4);
    assert(read_ue(br) == 0);
    assert(read_ue(br) == 0);
    assert(read_ue(br) == 3);
    assert(read_ue(br) == 0);
    assert(read_ue(br) == 3);
    assert(read_ue(br) == 2);
    assert(read_ue(br) == 2);
}

static void test_slice_info(void)
{
    uint8_t bare[32];
    BSWriter bs;
    bs_init(&bs, bare, sizeof(bare));
    bs_write(&bs, 0, 1);
    bs_write(&bs, 19, 6);
    bs_write(&bs, 0, 6);
    bs_write(&bs, 1, 3);
    bs_write(&bs, 1, 1);       /* first_slice_segment_in_pic_flag */
    bs_write(&bs, 0, 1);       /* no_output_of_prior_pics_flag */
    bs_write_ue(&bs, 37);
    bs_rbsp_trailing(&bs);
    size_t bare_size = bs_bytes(&bs);

    RKHEVCSliceInfo info;
    assert(rk_hevc_parse_slice_info(bare, bare_size, &info));
    assert(info.nal_unit_type == 19);
    assert(info.temporal_id_plus1 == 1);
    assert(info.pps_id == 37);
    assert(info.first_slice_segment_in_pic);

    uint8_t annexb[36] = {0, 0, 0, 1};
    memcpy(annexb + 4, bare, bare_size);
    assert(rk_hevc_parse_slice_info(annexb, bare_size + 4, &info));
    assert(info.pps_id == 37);

    assert(!rk_hevc_parse_slice_info(NULL, 0, &info));
    assert(!rk_hevc_parse_slice_info(bare, 2, &info));
    bare[1] &= 0xf8;
    assert(!rk_hevc_parse_slice_info(bare, bare_size, &info));

    uint8_t hostile[8];
    for (unsigned int value = 0; value <= UINT8_MAX; value++) {
        memset(hostile, (int)value, sizeof(hostile));
        for (size_t size = 0; size <= sizeof(hostile); size++)
            (void)rk_hevc_parse_slice_info(hostile, size, &info);
    }
}

static void test_reference_sets_and_pps(void)
{
    VAPictureParameterBufferHEVC pp;
    static uint8_t bundle[65536];
    uint8_t rbsp[32768];
    init_picture(&pp);
    pp.num_short_term_ref_pic_sets = 3;
    pp.slice_parsing_fields.bits.long_term_ref_pics_present_flag = 1;
    pp.num_long_term_ref_pic_sps = 2;
    add_reference(&pp, 0, 2, 9, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_reference(&pp, 1, 3, 7, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE);
    add_reference(&pp, 2, 4, 12, VA_PICTURE_HEVC_RPS_ST_CURR_AFTER);
    add_reference(&pp, 3, 5, 2, VA_PICTURE_HEVC_RPS_LT_CURR |
                                   VA_PICTURE_HEVC_LONG_TERM_REFERENCE);
    pp.pic_fields.bits.tiles_enabled_flag = 1;
    pp.pic_fields.bits.loop_filter_across_tiles_enabled_flag = 1;
    pp.num_tile_columns_minus1 = 1;
    pp.num_tile_rows_minus1 = 1;
    pp.column_width_minus1[0] = 2;
    pp.row_height_minus1[0] = 1;
    pp.slice_parsing_fields.bits.lists_modification_present_flag = 1;

    int bundle_size = rk_hevc_write_parameter_sets(bundle, sizeof(bundle),
                                                    &pp, NULL, 37, 1);
    assert(bundle_size > 0);

    size_t nal_size;
    const uint8_t *nal = find_nal(bundle, (size_t)bundle_size, 32, &nal_size);
    assert(nal && nal_size > 2);
    BitReader br = {rbsp, unescape_nal(nal, nal_size, rbsp, sizeof(rbsp)), 0};
    check_nal_header(&br, 32);
    assert(read_bits(&br, 4) == 0);

    nal = find_nal(bundle, (size_t)bundle_size, 33, &nal_size);
    assert(nal && nal_size > 2);
    br = (BitReader){rbsp, unescape_nal(nal, nal_size, rbsp, sizeof(rbsp)), 0};
    skip_sps_to_scaling(&br, 1);
    assert(read_bit(&br) == 0); /* scaling_list_enabled_flag */
    assert(read_bit(&br) == 1); /* amp */
    assert(read_bit(&br) == 1); /* sao */
    assert(read_bit(&br) == 0); /* pcm */
    assert(read_ue(&br) == 3);
    for (int set = 0; set < 3; set++) {
        if (set)
            assert(read_bit(&br) == 0);
        assert(read_ue(&br) == 2);
        assert(read_ue(&br) == 1);
        assert(read_ue(&br) == 0 && read_bit(&br) == 1);
        assert(read_ue(&br) == 1 && read_bit(&br) == 1);
        assert(read_ue(&br) == 1 && read_bit(&br) == 1);
    }
    assert(read_bit(&br) == 1);
    assert(read_ue(&br) == 2);
    for (int i = 0; i < 2; i++) {
        assert(read_bits(&br, 8) == 2);
        assert(read_bit(&br) == 1);
    }

    nal = find_nal(bundle, (size_t)bundle_size, 34, &nal_size);
    assert(nal && nal_size > 2);
    br = (BitReader){rbsp, unescape_nal(nal, nal_size, rbsp, sizeof(rbsp)), 0};
    check_nal_header(&br, 34);
    assert(read_ue(&br) == 37);
    assert(read_ue(&br) == 0);
    assert(read_bit(&br) == 0);
    assert(read_bit(&br) == 0);
    assert(read_bits(&br, 3) == 0);
}

static const uint8_t scan4x4_x[16] = {
    0,0,1,0,1,2,0,1,2,3,1,2,3,2,3,3,
};
static const uint8_t scan4x4_y[16] = {
    0,1,0,2,1,0,3,2,1,0,3,2,1,3,2,3,
};
static const uint8_t scan8x8_x[64] = {
    0,0,1,0,1,2,0,1,2,3,0,1,2,3,4,0,
    1,2,3,4,5,0,1,2,3,4,5,6,0,1,2,3,
    4,5,6,7,1,2,3,4,5,6,7,2,3,4,5,6,
    7,3,4,5,6,7,4,5,6,7,5,6,7,6,7,7,
};
static const uint8_t scan8x8_y[64] = {
    0,1,0,2,1,0,3,2,1,0,4,3,2,1,0,5,
    4,3,2,1,0,6,5,4,3,2,1,0,7,6,5,4,
    3,2,1,0,7,6,5,4,3,2,1,7,6,5,4,3,
    2,7,6,5,4,3,7,6,5,4,7,6,5,7,6,7,
};

static uint8_t *matrix_at(VAIQMatrixBufferHEVC *iq, int size, int matrix)
{
    if (size == 0) return iq->ScalingList4x4[matrix];
    if (size == 1) return iq->ScalingList8x8[matrix];
    if (size == 2) return iq->ScalingList16x16[matrix];
    return iq->ScalingList32x32[matrix / 3];
}

static int dc_at(const VAIQMatrixBufferHEVC *iq, int size, int matrix)
{
    if (size == 2) return iq->ScalingListDC16x16[matrix];
    if (size == 3) return iq->ScalingListDC32x32[matrix / 3];
    return 8;
}

static void test_scaling_lists(void)
{
    VAPictureParameterBufferHEVC pp;
    VAIQMatrixBufferHEVC iq = {0};
    static uint8_t bundle[65536];
    uint8_t rbsp[32768];
    init_picture(&pp);
    pp.pic_fields.bits.scaling_list_enabled_flag = 1;
    pp.bit_depth_luma_minus8 = 2;
    pp.bit_depth_chroma_minus8 = 2;

    for (int size = 0; size < 4; size++) {
        int step = size == 3 ? 3 : 1;
        int count = size == 0 ? 16 : 64;
        for (int matrix = 0; matrix < 6; matrix += step) {
            uint8_t *values = matrix_at(&iq, size, matrix);
            for (int i = 0; i < count; i++)
                values[i] = (uint8_t)((size * 53 + matrix * 29 + i * 7) & 255);
            if (size == 2)
                iq.ScalingListDC16x16[matrix] = (uint8_t)(16 + matrix);
            if (size == 3)
                iq.ScalingListDC32x32[matrix / 3] = (uint8_t)(24 + matrix);
        }
    }

    int bundle_size = rk_hevc_write_parameter_sets(bundle, sizeof(bundle),
                                                    &pp, &iq, 63, 2);
    assert(bundle_size > 0);
    size_t nal_size;
    const uint8_t *nal = find_nal(bundle, (size_t)bundle_size, 33, &nal_size);
    assert(nal);
    BitReader br = {rbsp, unescape_nal(nal, nal_size, rbsp, sizeof(rbsp)), 0};
    skip_sps_to_scaling(&br, 2);
    assert(read_bit(&br) == 1);
    assert(read_bit(&br) == 1);
    for (int size = 0; size < 4; size++) {
        int step = size == 3 ? 3 : 1;
        int count = size == 0 ? 16 : 64;
        for (int matrix = 0; matrix < 6; matrix += step) {
            const uint8_t *values = matrix_at(&iq, size, matrix);
            int next = 8;
            assert(read_bit(&br) == 1);
            if (size > 1) {
                next = read_se(&br) + 8;
                assert(next == dc_at(&iq, size, matrix));
            }
            for (int i = 0; i < count; i++) {
                int pos = size == 0 ? 4 * scan4x4_y[i] + scan4x4_x[i]
                                    : 8 * scan8x8_y[i] + scan8x8_x[i];
                next = (next + read_se(&br) + 256) & 255;
                assert(next == values[pos]);
            }
        }
    }
}

static void test_rejection(void)
{
    VAPictureParameterBufferHEVC pp;
    VAIQMatrixBufferHEVC iq;
    uint8_t out[64];
    static uint8_t large[65536];
    init_picture(&pp);

    assert(rk_hevc_write_parameter_sets(out, sizeof(out), &pp, NULL, 0, 1) < 0);
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 64, 1) < 0);
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 3) < 0);

    pp.pic_fields.bits.scaling_list_enabled_flag = 1;
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 1) < 0);
    memset(&iq, 16, sizeof(iq));
    iq.ScalingListDC16x16[0] = 0;
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, &iq, 0, 1) < 0);

    init_picture(&pp);
    pp.bit_depth_luma_minus8 = 2;
    pp.bit_depth_chroma_minus8 = 2;
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 1) < 0);

    init_picture(&pp);
    add_reference(&pp, 0, 2, 9, VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
                                  VA_PICTURE_HEVC_RPS_ST_CURR_AFTER);
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 1) < 0);

    init_picture(&pp);
    pp.num_extra_slice_header_bits = 8;
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 1) < 0);

    init_picture(&pp);
    pp.pic_width_in_luma_samples++;
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 1) < 0);

    init_picture(&pp);
    pp.pic_fields.bits.tiles_enabled_flag = 1;
    pp.num_tile_columns_minus1 = 7;
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 1) < 0);

    init_picture(&pp);
    pp.pps_beta_offset_div2 = 7;
    assert(rk_hevc_write_parameter_sets(large, sizeof(large), &pp, NULL, 0, 1) < 0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "--emit-headers") == 0 ||
                      strcmp(argv[1], "--emit-main10-headers") == 0)) {
        VAPictureParameterBufferHEVC pp;
        VAIQMatrixBufferHEVC iq;
        uint8_t bundle[65536];
        init_picture(&pp);
        bool main10 = strcmp(argv[1], "--emit-main10-headers") == 0;
        const VAIQMatrixBufferHEVC *iq_ptr = NULL;
        if (main10) {
            pp.bit_depth_luma_minus8 = 2;
            pp.bit_depth_chroma_minus8 = 2;
            pp.pic_fields.bits.scaling_list_enabled_flag = 1;
            memset(&iq, 16, sizeof(iq));
            iq_ptr = &iq;
        }
        int size = rk_hevc_write_parameter_sets(bundle, sizeof(bundle),
                                                &pp, iq_ptr, 0,
                                                main10 ? 2 : 1);
        assert(size > 0);
        assert(fwrite(bundle, 1, (size_t)size, stdout) == (size_t)size);
        return 0;
    }
    test_slice_info();
    test_reference_sets_and_pps();
    test_scaling_lists();
    test_rejection();
    puts("HEVC reconstruction tests: OK");
    return 0;
}
