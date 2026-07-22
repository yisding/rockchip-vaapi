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
    assert(!rk_nv12_layout_size(SIZE_MAX, 2, &size));
    assert(!rk_nv12_layout_size(4, 3, &size));
    assert(!rk_surface_buffer_size(0, 288, &size));
    assert(!rk_surface_buffer_size(UINT32_MAX, UINT32_MAX, &size));
}

int main(void)
{
    test_vp9_odd_stride_surface();
    test_bounds_rejection();
    puts("frame layout tests: OK");
    return 0;
}
