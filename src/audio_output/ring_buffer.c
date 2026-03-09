#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <pthread.h>
#endif

struct RingBuffer {
    uint8_t* buffer;
    size_t   capacity;
    size_t   read_pos;
    size_t   write_pos;
    size_t   fill_count;
    int      canceled;

#ifdef _WIN32
    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE space_available;
#else
    pthread_mutex_t lock;
    pthread_cond_t  space_available;
#endif
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

#ifdef _WIN32
    InitializeCriticalSection(&rb->lock);
    InitializeConditionVariable(&rb->space_available);
#else
    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->space_available, NULL);
#endif

    return rb;
}

void ring_buffer_destroy(RingBuffer* rb) {
    if (!rb) return;

    ring_buffer_cancel(rb);

#ifdef _WIN32
    EnterCriticalSection(&rb->lock);
    free(rb->buffer);
    LeaveCriticalSection(&rb->lock);
    DeleteCriticalSection(&rb->lock);
#else
    pthread_mutex_lock(&rb->lock);
    free(rb->buffer);
    pthread_mutex_unlock(&rb->lock);
    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->space_available);
#endif

    free(rb);
}

size_t ring_buffer_write(RingBuffer* rb, const void* data, size_t size) {
    const uint8_t* input = (const uint8_t*)data;
    size_t written = 0;

#ifdef _WIN32
    EnterCriticalSection(&rb->lock);
#else
    pthread_mutex_lock(&rb->lock);
#endif

    while (written < size) {
        if (rb->canceled) break;

        size_t space_free = rb->capacity - rb->fill_count;

        if (space_free == 0) {
            /* Buffer full: wait for consumer to drain some data */
#ifdef _WIN32
            SleepConditionVariableCS(&rb->space_available, &rb->lock, INFINITE);
#else
            pthread_cond_wait(&rb->space_available, &rb->lock);
#endif
            continue;
        }

        size_t chunk = size - written;
        if (chunk > space_free) chunk = space_free;

        /* Handle wrap-around */
        size_t end_space = rb->capacity - rb->write_pos;
        if (chunk > end_space) {
            /* Split write across the wrap boundary */
            memcpy(rb->buffer + rb->write_pos, input + written,            end_space);
            memcpy(rb->buffer,                 input + written + end_space, chunk - end_space);
        } else {
            memcpy(rb->buffer + rb->write_pos, input + written, chunk);
        }

        rb->write_pos  = (rb->write_pos + chunk) % rb->capacity;
        rb->fill_count += chunk;
        written        += chunk;
    }

#ifdef _WIN32
    LeaveCriticalSection(&rb->lock);
#else
    pthread_mutex_unlock(&rb->lock);
#endif

    return written;
}

size_t ring_buffer_read(RingBuffer* rb, void* data, size_t size) {
    uint8_t* output = (uint8_t*)data;

#ifdef _WIN32
    EnterCriticalSection(&rb->lock);
#else
    pthread_mutex_lock(&rb->lock);
#endif

    size_t available = rb->fill_count;
    size_t to_read   = (size < available) ? size : available;

    if (to_read > 0) {
        size_t end_data = rb->capacity - rb->read_pos;

        if (to_read > end_data) {
            /* Split read across the wrap boundary */
            memcpy(output,            rb->buffer + rb->read_pos, end_data);
            memcpy(output + end_data, rb->buffer,                to_read - end_data);
        } else {
            memcpy(output, rb->buffer + rb->read_pos, to_read);
        }

        rb->read_pos    = (rb->read_pos + to_read) % rb->capacity;
        rb->fill_count -= to_read;

        /* Signal any blocked writer that space has opened up */
#ifdef _WIN32
        WakeConditionVariable(&rb->space_available);
#else
        pthread_cond_signal(&rb->space_available);
#endif
    }

#ifdef _WIN32
    LeaveCriticalSection(&rb->lock);
#else
    pthread_mutex_unlock(&rb->lock);
#endif

    return to_read;
}

size_t ring_buffer_available(RingBuffer* rb) {
    if (!rb) return 0;
#ifdef _WIN32
    EnterCriticalSection(&rb->lock);
#else
    pthread_mutex_lock(&rb->lock);
#endif
    size_t av = rb->fill_count;
#ifdef _WIN32
    LeaveCriticalSection(&rb->lock);
#else
    pthread_mutex_unlock(&rb->lock);
#endif
    return av;
}

size_t ring_buffer_get_capacity(RingBuffer* rb) {
    if (!rb) return 0;
    return rb->capacity;
}

void ring_buffer_cancel(RingBuffer* rb) {
    if (!rb) return;
#ifdef _WIN32
    EnterCriticalSection(&rb->lock);
    rb->canceled = 1;
    WakeAllConditionVariable(&rb->space_available);
    LeaveCriticalSection(&rb->lock);
#else
    pthread_mutex_lock(&rb->lock);
    rb->canceled = 1;
    pthread_cond_broadcast(&rb->space_available);
    pthread_mutex_unlock(&rb->lock);
#endif
}
