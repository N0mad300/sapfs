#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>

typedef struct RingBuffer RingBuffer;

RingBuffer* ring_buffer_create(size_t size);
void ring_buffer_destroy(RingBuffer* rb);
size_t ring_buffer_write(RingBuffer* rb, const void* data, size_t size);
size_t ring_buffer_read(RingBuffer* rb, void* data, size_t size);
size_t ring_buffer_available(RingBuffer* rb);
size_t ring_buffer_get_capacity(RingBuffer* rb);
void ring_buffer_cancel(RingBuffer* rb);

#endif