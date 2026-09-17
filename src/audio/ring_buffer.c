#include "audio/ring_buffer.h"

#include <stdlib.h>

int fb_ring_buffer_init(FbRingBuffer *rb, size_t capacity) {
    rb->data = (float *)malloc(capacity * sizeof(float));
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    return rb->data ? 0 : -1;
}

void fb_ring_buffer_free(FbRingBuffer *rb) {
    free(rb->data);
    rb->data = NULL;
    rb->capacity = 0;
    rb->head = 0;
    rb->tail = 0;
}

size_t fb_ring_buffer_write(FbRingBuffer *rb, const float *samples, size_t count) {
    size_t tail = rb->tail;
    size_t used = rb->head - tail;
    size_t free_space = rb->capacity - used;
    size_t n = count < free_space ? count : free_space;
    for (size_t i = 0; i < n; ++i) {
        rb->data[(rb->head + i) % rb->capacity] = samples[i];
    }
    rb->head += n;
    return n;
}

size_t fb_ring_buffer_read(FbRingBuffer *rb, float *out, size_t max_count) {
    size_t head = rb->head;
    size_t used = head - rb->tail;
    size_t n = max_count < used ? max_count : used;
    for (size_t i = 0; i < n; ++i) {
        out[i] = rb->data[(rb->tail + i) % rb->capacity];
    }
    rb->tail += n;
    return n;
}

size_t fb_ring_buffer_available(const FbRingBuffer *rb) {
    return rb->head - rb->tail;
}
