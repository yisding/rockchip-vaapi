#include "object_heap.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    RKObjectBase base;
    unsigned int value;
    atomic_uint *destroyed;
} TestObject;

static void destroy_test_object(void *opaque)
{
    TestObject *object = opaque;
    if (object->destroyed)
        atomic_fetch_add_explicit(object->destroyed, 1u,
                                  memory_order_relaxed);
    free(object);
}

static TestObject *new_test_object(unsigned int value,
                                   atomic_uint *destroyed)
{
    TestObject *object = calloc(1, sizeof(*object));
    assert(object);
    rk_object_init(&object->base, destroy_test_object);
    object->value = value;
    object->destroyed = destroyed;
    return object;
}

static void test_growth_types_and_stale_handles(void)
{
    RKObjectHeap configs;
    RKObjectHeap surfaces;
    uint32_t ids[80];
    atomic_uint destroyed = 0;

    assert(!rk_object_heap_init(NULL, RK_OBJECT_CONFIG));
    assert(!rk_object_heap_init(&configs, 0));
    assert(rk_object_heap_init(&configs, RK_OBJECT_CONFIG));
    assert(rk_object_heap_init(&surfaces, RK_OBJECT_SURFACE));

    for (unsigned int i = 0; i < 80; i++) {
        TestObject *object = new_test_object(i, &destroyed);
        assert(rk_object_heap_insert(&configs, &object->base, &ids[i]));
    }
    assert(rk_object_heap_count(&configs) == 80);

    TestObject *surface = new_test_object(900, &destroyed);
    uint32_t surface_id;
    assert(rk_object_heap_insert(&surfaces, &surface->base, &surface_id));

    for (unsigned int i = 0; i < 80; i++) {
        TestObject *object = (TestObject *)rk_object_heap_acquire(
            &configs, ids[i]);
        assert(object && object->value == i);
        assert(!rk_object_heap_acquire(&surfaces, ids[i]));
        assert(!rk_object_heap_acquire(&configs, surface_id));
        rk_object_unref(&object->base);
    }

    uint32_t stale = ids[17];
    TestObject *removed = (TestObject *)rk_object_heap_remove(&configs, stale);
    assert(removed && removed->value == 17);
    assert(!rk_object_heap_acquire(&configs, stale));
    rk_object_unref(&removed->base);
    assert(atomic_load(&destroyed) == 1);

    TestObject *replacement = new_test_object(117, &destroyed);
    uint32_t replacement_id;
    assert(rk_object_heap_insert(&configs, &replacement->base,
                                 &replacement_id));
    assert(replacement_id != stale);
    assert(!rk_object_heap_acquire(&configs, stale));

    rk_object_heap_finish(&surfaces);
    rk_object_heap_finish(&configs);
    assert(atomic_load(&destroyed) == 82);
}

static void test_acquired_lifetime_after_remove(void)
{
    RKObjectHeap heap;
    atomic_uint destroyed = 0;
    uint32_t id;

    assert(rk_object_heap_init(&heap, RK_OBJECT_BUFFER));
    TestObject *object = new_test_object(42, &destroyed);
    assert(rk_object_heap_insert(&heap, &object->base, &id));

    TestObject *held = (TestObject *)rk_object_heap_acquire(&heap, id);
    assert(held == object);
    TestObject *removed = (TestObject *)rk_object_heap_remove(&heap, id);
    assert(removed == object);
    rk_object_unref(&removed->base);
    assert(atomic_load(&destroyed) == 0);
    assert(held->value == 42);

    rk_object_unref(&held->base);
    assert(atomic_load(&destroyed) == 1);
    rk_object_heap_finish(&heap);
}

static void test_generation_exhaustion_retires_slot(void)
{
    RKObjectHeap heap;
    atomic_uint destroyed = 0;
    uint32_t first_id = 0;
    uint32_t previous_id = 0;

    assert(rk_object_heap_init(&heap, RK_OBJECT_CONTEXT));
    for (unsigned int generation = 1; generation <= 255; generation++) {
        TestObject *object = new_test_object(generation, &destroyed);
        uint32_t id;
        assert(rk_object_heap_insert(&heap, &object->base, &id));
        if (generation == 1)
            first_id = id;
        if (previous_id)
            assert(id != previous_id);
        previous_id = id;
        TestObject *removed = (TestObject *)rk_object_heap_remove(&heap, id);
        assert(removed == object);
        rk_object_unref(&removed->base);
    }

    TestObject *next = new_test_object(256, &destroyed);
    uint32_t next_id;
    assert(rk_object_heap_insert(&heap, &next->base, &next_id));
    assert((next_id & 0x000fffffu) != (first_id & 0x000fffffu));
    assert(!rk_object_heap_acquire(&heap, first_id));
    rk_object_heap_finish(&heap);
    assert(atomic_load(&destroyed) == 256);
}

typedef struct {
    RKObjectHeap heap;
    pthread_mutex_t lock;
    atomic_uint destroyed;
} ThreadTest;

static void *heap_worker(void *opaque)
{
    ThreadTest *test = opaque;

    for (unsigned int i = 0; i < 2000; i++) {
        TestObject *object = new_test_object(i, &test->destroyed);
        uint32_t id;

        pthread_mutex_lock(&test->lock);
        assert(rk_object_heap_insert(&test->heap, &object->base, &id));
        TestObject *held = (TestObject *)rk_object_heap_acquire(&test->heap,
                                                               id);
        TestObject *removed = (TestObject *)rk_object_heap_remove(&test->heap,
                                                                 id);
        assert(!rk_object_heap_acquire(&test->heap, id));
        pthread_mutex_unlock(&test->lock);

        assert(held == object);
        assert(removed == object);
        rk_object_unref(&held->base);
        rk_object_unref(&removed->base);
    }
    return NULL;
}

static void test_concurrent_lifetimes_with_external_lock(void)
{
    ThreadTest test = {0};
    pthread_t threads[4];

    assert(rk_object_heap_init(&test.heap, RK_OBJECT_BUFFER));
    assert(pthread_mutex_init(&test.lock, NULL) == 0);
    for (size_t i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, heap_worker, &test) == 0);
    for (size_t i = 0; i < 4; i++)
        assert(pthread_join(threads[i], NULL) == 0);

    assert(rk_object_heap_count(&test.heap) == 0);
    assert(atomic_load(&test.destroyed) == 8000);
    pthread_mutex_destroy(&test.lock);
    rk_object_heap_finish(&test.heap);
}

int main(void)
{
    test_growth_types_and_stale_handles();
    test_acquired_lifetime_after_remove();
    test_generation_exhaustion_retires_slot();
    test_concurrent_lifetimes_with_external_lock();
    puts("object heap tests: OK");
    return 0;
}
