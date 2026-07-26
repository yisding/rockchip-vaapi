#include "frame_layout.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_vp9_odd_stride_surface(void)
{
    size_t required;
    size_t allocated;
    assert(rk_nv12_layout_size(768, 288, &required));
    assert(required == 331776);
    assert((size_t)352 * 288 * 3 == 304128); /* former allocation */
    assert((size_t)352 * 288 * 3 < required);
    assert(rk_surface_buffer_size(352, 288, &allocated));
    assert(allocated == (size_t)768 * 320 * 2);
    assert(allocated >= (size_t)768 * 288 * 2);
    assert(allocated >= required);

}

static void test_bounds_rejection(void)
{
    size_t size;

    assert(rk_nv12_layout_size(4, 4, &size));
    assert(size == 24);
    assert(rk_p010_layout_size(4, 4, &size));
    assert(size == 48);
    assert(!rk_nv12_layout_size(SIZE_MAX, 2, &size));
    assert(!rk_nv12_layout_size(4, 3, &size));
    assert(!rk_p010_layout_size(SIZE_MAX, 2, &size));
    assert(!rk_surface_buffer_size(0, 288, &size));
    assert(!rk_surface_buffer_size(UINT32_MAX, UINT32_MAX, &size));
}

static void test_placeholder_formats(void)
{
    size_t placeholder_8bit;
    size_t placeholder_10bit;

    assert(rk_surface_placeholder_size(7680, 4320, false,
                                       &placeholder_8bit));
    assert(rk_surface_placeholder_size(7680, 4320, true,
                                       &placeholder_10bit));
    assert(placeholder_10bit >= 7680u * 4320u * 3u);
    assert(placeholder_10bit > placeholder_8bit);
    assert(!rk_surface_placeholder_size(0, 4320, true,
                                        &placeholder_10bit));
}

int main(void)
{
    test_vp9_odd_stride_surface();
    test_bounds_rejection();
    test_placeholder_formats();
    puts("frame layout tests: OK");
    return 0;
}
