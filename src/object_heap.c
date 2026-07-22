#include "object_heap.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define RK_OBJECT_TYPE_SHIFT       28u
#define RK_OBJECT_GENERATION_SHIFT 20u
#define RK_OBJECT_TYPE_MASK        0x0fu
#define RK_OBJECT_GENERATION_MASK  0xffu
#define RK_OBJECT_INDEX_MASK       0x000fffffu
#define RK_OBJECT_NO_SLOT          UINT32_MAX
#define RK_OBJECT_INITIAL_CAPACITY 16u

struct RKObjectSlot {
    RKObjectBase *object;
    uint32_t next_free;
    uint8_t generation;
    bool retired;
};

static bool valid_type(RKObjectType type)
{
    return type >= RK_OBJECT_CONFIG && type <= RK_OBJECT_IMAGE;
}

static uint32_t make_id(RKObjectType type, uint8_t generation,
                        uint32_t index)
{
    return ((uint32_t)type << RK_OBJECT_TYPE_SHIFT) |
           ((uint32_t)generation << RK_OBJECT_GENERATION_SHIFT) | index;
}

static bool decode_id(const RKObjectHeap *heap, uint32_t id,
                      uint32_t *index, uint8_t *generation)
{
    uint32_t encoded_type = (id >> RK_OBJECT_TYPE_SHIFT) &
                            RK_OBJECT_TYPE_MASK;
    uint32_t encoded_generation = (id >> RK_OBJECT_GENERATION_SHIFT) &
                                  RK_OBJECT_GENERATION_MASK;
    uint32_t encoded_index = id & RK_OBJECT_INDEX_MASK;

    if (!heap || encoded_type != (uint32_t)heap->type ||
        encoded_generation == 0 || encoded_index >= heap->high_water)
        return false;

    *index = encoded_index;
    *generation = (uint8_t)encoded_generation;
    return true;
}

static bool grow_heap(RKObjectHeap *heap)
{
    uint32_t maximum = RK_OBJECT_INDEX_MASK + 1u;
    uint32_t new_capacity = heap->capacity ? heap->capacity * 2u :
                                            RK_OBJECT_INITIAL_CAPACITY;

    if (heap->capacity >= maximum)
        return false;
    if (new_capacity < heap->capacity || new_capacity > maximum)
        new_capacity = maximum;

    RKObjectSlot *grown = realloc(heap->slots,
                                  (size_t)new_capacity * sizeof(*grown));
    if (!grown)
        return false;

    memset(grown + heap->capacity, 0,
           (size_t)(new_capacity - heap->capacity) * sizeof(*grown));
    heap->slots = grown;
    heap->capacity = new_capacity;
    return true;
}

void rk_object_init(RKObjectBase *object, RKObjectDestroyFunc destroy)
{
    if (!object)
        return;
    atomic_init(&object->refs, 1u);
    object->destroy = destroy;
}

bool rk_object_ref(RKObjectBase *object)
{
    if (!object)
        return false;

    unsigned int refs = atomic_load_explicit(&object->refs,
                                             memory_order_relaxed);
    while (refs != 0 && refs != UINT_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &object->refs, &refs, refs + 1u,
                memory_order_relaxed, memory_order_relaxed))
            return true;
    }
    return false;
}

void rk_object_unref(RKObjectBase *object)
{
    if (!object)
        return;

    unsigned int previous = atomic_fetch_sub_explicit(
        &object->refs, 1u, memory_order_acq_rel);
    if (previous == 1u && object->destroy)
        object->destroy(object);
}

bool rk_object_heap_init(RKObjectHeap *heap, RKObjectType type)
{
    if (!heap || !valid_type(type))
        return false;

    memset(heap, 0, sizeof(*heap));
    heap->type = type;
    heap->free_head = RK_OBJECT_NO_SLOT;
    return true;
}

void rk_object_heap_finish(RKObjectHeap *heap)
{
    if (!heap)
        return;

    for (uint32_t i = 0; i < heap->high_water; i++) {
        RKObjectBase *object = heap->slots[i].object;
        heap->slots[i].object = NULL;
        if (object)
            rk_object_unref(object);
    }
    free(heap->slots);
    memset(heap, 0, sizeof(*heap));
}

bool rk_object_heap_insert(RKObjectHeap *heap, RKObjectBase *object,
                           uint32_t *id)
{
    if (!heap || !object || !id || !valid_type(heap->type))
        return false;

    uint32_t index;
    RKObjectSlot *slot;

    if (heap->free_head != RK_OBJECT_NO_SLOT) {
        index = heap->free_head;
        slot = &heap->slots[index];
        heap->free_head = slot->next_free;
    } else {
        if (heap->high_water == heap->capacity && !grow_heap(heap))
            return false;
        index = heap->high_water++;
        slot = &heap->slots[index];
        slot->generation = 1u;
    }

    if (slot->retired || slot->object || slot->generation == 0)
        return false;

    slot->object = object;
    slot->next_free = RK_OBJECT_NO_SLOT;
    heap->count++;
    *id = make_id(heap->type, slot->generation, index);
    return true;
}

RKObjectBase *rk_object_heap_acquire(RKObjectHeap *heap, uint32_t id)
{
    uint32_t index;
    uint8_t generation;

    if (!decode_id(heap, id, &index, &generation))
        return NULL;

    RKObjectSlot *slot = &heap->slots[index];
    if (!slot->object || slot->retired || slot->generation != generation ||
        !rk_object_ref(slot->object))
        return NULL;
    return slot->object;
}

RKObjectBase *rk_object_heap_remove(RKObjectHeap *heap, uint32_t id)
{
    uint32_t index;
    uint8_t generation;

    if (!decode_id(heap, id, &index, &generation))
        return NULL;

    RKObjectSlot *slot = &heap->slots[index];
    if (!slot->object || slot->retired || slot->generation != generation)
        return NULL;

    RKObjectBase *object = slot->object;
    slot->object = NULL;
    heap->count--;

    if (slot->generation == RK_OBJECT_GENERATION_MASK) {
        slot->retired = true;
        slot->next_free = RK_OBJECT_NO_SLOT;
    } else {
        slot->generation++;
        slot->next_free = heap->free_head;
        heap->free_head = index;
    }
    return object;
}

size_t rk_object_heap_count(const RKObjectHeap *heap)
{
    return heap ? heap->count : 0;
}
