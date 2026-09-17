#ifndef FB_RING_BUFFER_H
#define FB_RING_BUFFER_H

#include <stddef.h>

/*
 * Lock-free single-producer/single-consumer float ring buffer. head/tail are
 * monotonically increasing (wrapped only when indexing into data[]), which
 * is the standard SPSC pattern: the producer only ever writes head, the
 * consumer only ever writes tail, so there is no shared mutable state for
 * either side to race on. Deliberately not using <stdatomic.h> (C11 atomics
 * support is inconsistent across the MSVC/GCC/Clang versions this project
 * targets) — plain volatile size_t read/write is the same pattern most
 * real-time audio code relies on for this exact shape.
 */

typedef struct {
    float *data;
    size_t capacity;
    volatile size_t head; /* producer-owned */
    volatile size_t tail; /* consumer-owned */
} FbRingBuffer;

int fb_ring_buffer_init(FbRingBuffer *rb, size_t capacity);
void fb_ring_buffer_free(FbRingBuffer *rb);

/* Producer only. Never blocks; drops samples past capacity and returns how
   many were actually written. */
size_t fb_ring_buffer_write(FbRingBuffer *rb, const float *samples, size_t count);

/* Consumer only. Returns how many samples were actually read. */
size_t fb_ring_buffer_read(FbRingBuffer *rb, float *out, size_t max_count);

/* Consumer only. */
size_t fb_ring_buffer_available(const FbRingBuffer *rb);

#endif /* FB_RING_BUFFER_H */
