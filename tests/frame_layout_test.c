#include "frame_layout.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_vp9_odd_stride_surface(void)
{
    size_t required;
    size_t allocated;
    assert(rk_nv12_layout_size(768, 288, &required));
    assert(required == 331776);
    assert((size_t)352 * 288 * 3 == 304128); /* former allocation */
    assert((size_t)352 * 288 * 3 < required);
    assert(rk_surface_buffer_size(352, 288, &allocated));
    assert(allocated >= required);

    uint8_t *src = malloc(required);
    uint8_t *dst = malloc(allocated + 16);
    assert(src && dst);
    for (size_t i = 0; i < required; i++)
        src[i] = (uint8_t)(i * 17u + 3u);
    memset(dst, 0, allocated + 16);
    memset(dst + allocated, 0xa5, 16);

    assert(rk_copy_nv12_frame(dst, allocated, src, required, 768, 288));
    assert(memcmp(dst, src, required) == 0);
    for (size_t i = 0; i < 16; i++)
        assert(dst[allocated + i] == 0xa5);

    free(dst);
    free(src);
}

static void test_bounds_rejection(void)
{
    uint8_t src[24] = {0};
    uint8_t dst[24] = {0};
    size_t size;

    assert(rk_nv12_layout_size(4, 4, &size));
    assert(size == sizeof(src));
    assert(rk_copy_nv12_frame(dst, sizeof(dst), src, sizeof(src), 4, 4));
    assert(!rk_copy_nv12_frame(dst, sizeof(dst) - 1, src, sizeof(src), 4, 4));
    assert(!rk_copy_nv12_frame(dst, sizeof(dst), src, sizeof(src) - 1, 4, 4));
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
