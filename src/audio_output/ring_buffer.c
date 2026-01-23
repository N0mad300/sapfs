#include "ring_buffer.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>

struct RingBuffer {
    uint8_t* buffer;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t fill_count;
    
    /* Thread safety */
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE space_available; /* Signals writer that space opened up */
    int canceled;
};

RingBuffer* ring_buffer_create(size_t size) {
    RingBuffer* rb = (RingBuffer*)calloc(1, sizeof(RingBuffer));
    if (!rb) return NULL;
    
    rb->buffer = (uint8_t*)malloc(size);
    if (!rb->buffer) {
        free(rb);
        return NULL;
    }
    
    rb->capacity = size;
    InitializeCriticalSection(&rb->lock);
    InitializeConditionVariable(&rb->space_available);
    
    return rb;
}

void ring_buffer_destroy(RingBuffer* rb) {
    if (!rb) return;
    
    ring_buffer_cancel(rb);
    
    /* Ensure no one is using it before deleting */
    EnterCriticalSection(&rb->lock);
    free(rb->buffer);
    LeaveCriticalSection(&rb->lock);
    DeleteCriticalSection(&rb->lock);
    free(rb);
}

size_t ring_buffer_write(RingBuffer* rb, const void* data, size_t size) {
    const uint8_t* input = (const uint8_t*)data;
    size_t written = 0;
    
    EnterCriticalSection(&rb->lock);
    
    while (written < size) {
        if (rb->canceled) break;
        
        size_t space_free = rb->capacity - rb->fill_count;
        
        if (space_free == 0) {
            /* Buffer full: Wait for consumer to read data */
            SleepConditionVariableCS(&rb->space_available, &rb->lock, INFINITE);
            continue;
        }
        
        size_t chunk = size - written;
        if (chunk > space_free) chunk = space_free;
        
        /* Handle wrap-around */
        size_t end_space = rb->capacity - rb->write_pos;
        if (chunk > end_space) {
            /* Split write */
            memcpy(rb->buffer + rb->write_pos, input + written, end_space);
            memcpy(rb->buffer, input + written + end_space, chunk - end_space);
        } else {
            /* Contiguous write */
            memcpy(rb->buffer + rb->write_pos, input + written, chunk);
        }
        
        rb->write_pos = (rb->write_pos + chunk) % rb->capacity;
        rb->fill_count += chunk;
        written += chunk;
    }
    
    LeaveCriticalSection(&rb->lock);
    return written;
}

size_t ring_buffer_read(RingBuffer* rb, void* data, size_t size) {
    uint8_t* output = (uint8_t*)data;
    
    EnterCriticalSection(&rb->lock);
    
    size_t available = rb->fill_count;
    size_t to_read = (size < available) ? size : available;
    
    if (to_read > 0) {
        size_t end_data = rb->capacity - rb->read_pos;
        
        if (to_read > end_data) {
            /* Split read */
            memcpy(output, rb->buffer + rb->read_pos, end_data);
            memcpy(output + end_data, rb->buffer, to_read - end_data);
        } else {
            /* Contiguous read */
            memcpy(output, rb->buffer + rb->read_pos, to_read);
        }
        
        rb->read_pos = (rb->read_pos + to_read) % rb->capacity;
        rb->fill_count -= to_read;
        
        /* Signal writers that space is available */
        WakeConditionVariable(&rb->space_available);
    }
    
    LeaveCriticalSection(&rb->lock);
    return to_read;
}

size_t ring_buffer_available(RingBuffer* rb) {
    EnterCriticalSection(&rb->lock);
    size_t av = rb->fill_count;
    LeaveCriticalSection(&rb->lock);
    return av;
}

size_t ring_buffer_get_capacity(RingBuffer* rb) {
    if (!rb) return 0;
    return rb->capacity;
}

void ring_buffer_cancel(RingBuffer* rb) {
    EnterCriticalSection(&rb->lock);
    rb->canceled = 1;
    WakeAllConditionVariable(&rb->space_available);
    LeaveCriticalSection(&rb->lock);
}