#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>

typedef struct RingBuffer RingBuffer;

/**
 * Create a ring buffer.
 * @param size Capacity in bytes.
 * @return Handle or NULL.
 */
RingBuffer* ring_buffer_create(size_t size);

/**
 * Destroy the ring buffer.
 */
void ring_buffer_destroy(RingBuffer* rb);

/**
 * Write data to the ring buffer.
 * Blocks if there isn't enough space.
 * 
 * @param rb Handle.
 * @param data Data to write.
 * @param size Bytes to write.
 * @return Bytes written (or -1 on fatal error).
 */
size_t ring_buffer_write(RingBuffer* rb, const void* data, size_t size);

/**
 * Read data from the ring buffer.
 * Non-blocking: returns immediately with whatever data is available.
 * 
 * @param rb Handle.
 * @param data Output buffer.
 * @param size Bytes requested.
 * @return Bytes actually read.
 */
size_t ring_buffer_read(RingBuffer* rb, void* data, size_t size);

/**
 * Get available bytes for reading.
 */
size_t ring_buffer_available(RingBuffer* rb);

/**
 * Get the total capacity of the buffer in bytes.
 */
size_t ring_buffer_get_capacity(RingBuffer* rb);

/**
 * Signal the buffer to stop blocking (used for shutdown).
 */
void ring_buffer_cancel(RingBuffer* rb);

#endif