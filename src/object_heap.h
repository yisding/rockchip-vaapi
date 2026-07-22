/*
 * Dynamic, generation-tagged VA object storage.
 *
 * The heap itself is deliberately lock-free: callers serialize insert,
 * acquire, remove, and finish with the driver's object lock.  Acquired
 * objects carry an atomic reference, so their lifetime remains valid after
 * that short critical section ends.
 */

#ifndef RK_OBJECT_HEAP_H
#define RK_OBJECT_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    RK_OBJECT_CONFIG  = 1,
    RK_OBJECT_CONTEXT = 2,
    RK_OBJECT_SURFACE = 3,
    RK_OBJECT_BUFFER  = 4,
    RK_OBJECT_IMAGE   = 5,
} RKObjectType;

typedef void (*RKObjectDestroyFunc)(void *object);

typedef struct {
    atomic_uint refs;
    RKObjectDestroyFunc destroy;
} RKObjectBase;

typedef struct RKObjectSlot RKObjectSlot;

typedef struct {
    RKObjectType type;
    RKObjectSlot *slots;
    uint32_t capacity;
    uint32_t high_water;
    uint32_t free_head;
    size_t count;
} RKObjectHeap;

void rk_object_init(RKObjectBase *object, RKObjectDestroyFunc destroy);
bool rk_object_ref(RKObjectBase *object);
void rk_object_unref(RKObjectBase *object);

bool rk_object_heap_init(RKObjectHeap *heap, RKObjectType type);
void rk_object_heap_finish(RKObjectHeap *heap);
bool rk_object_heap_insert(RKObjectHeap *heap, RKObjectBase *object,
                           uint32_t *id);
RKObjectBase *rk_object_heap_acquire(RKObjectHeap *heap, uint32_t id);
RKObjectBase *rk_object_heap_remove(RKObjectHeap *heap, uint32_t id);
size_t rk_object_heap_count(const RKObjectHeap *heap);

#endif
