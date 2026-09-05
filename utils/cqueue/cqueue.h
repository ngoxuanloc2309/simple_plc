
#ifndef __QUEUE_H
#define __QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define CQUEUE_ASSERT(x) assert(x)

#define CQUEUE_DEBUG_ENABLE 0

#if QUEUE_DEBUG_ENABLE
#define CQUEUE_DEBUG     printf
#else
#define CQUEUE_DEBUG     (void)
#endif

#define CQUEUE_BUFFER_IS_POWER_OF_TWO(buffer_size) ((buffer_size & (buffer_size - 1)) == 0)

typedef struct CQueue
{
    void *buff;
    size_t size;
    size_t len;
    int head;
    int tail;
    size_t count;
}CQueue_t;

/**
 * @brief Initializes a static circular queue with the provided buffer.
 *
 * This function sets up a circular queue structure using a user-supplied memory buffer.
 * It does not allocate memory dynamically; instead, it uses the buffer provided by the caller.
 *
 * @param queue Pointer to the CQueue_t structure to initialize.
 * @param buff Pointer to the memory buffer to be used as the queue storage.
 * @param len Number of elements the buffer can hold.
 * @param size Size (in bytes) of each element in the queue.
 * @return Pointer to the initialized CQueue_t structure, or NULL on failure.
 */
CQueue_t *cqueue_init_static(CQueue_t *queue,void *buff,size_t len, size_t size);
/**
 * @brief Initializes a circular queue.
 *
 * This function initializes the given circular queue structure with the specified
 * number of elements and the size of each element.
 *
 * @param queue Pointer to the CQueue_t structure to initialize.
 * @param len   Number of elements the queue can hold.
 * @param size  Size (in bytes) of each element in the queue.
 * @return Pointer to the initialized CQueue_t structure, or NULL on failure.
 */
CQueue_t *cqueue_init(CQueue_t *queue,size_t len,size_t size);
/**
 * @brief Receives (removes) an item from the circular queue.
 *
 * This function retrieves an item from the specified circular queue and stores it
 * at the memory location pointed to by 'item'. The item is removed from the queue.
 *
 * @param queue Pointer to the CQueue_t structure representing the circular queue.
 * @param item Pointer to the memory where the received item will be stored.
 * @return true if an item was successfully received from the queue; 
 *         false if the queue is empty or an error occurred.
 */
bool cqueue_receive(CQueue_t *queue, void *item);
/**
 * @brief Sends an item to the specified circular queue.
 *
 * This function attempts to add the given item to the provided circular queue.
 *
 * @param queue Pointer to the CQueue_t structure representing the queue.
 * @param item Pointer to the item to be added to the queue.
 * @return true if the item was successfully added to the queue, false otherwise (e.g., if the queue is full).
 */
bool cqueue_send(CQueue_t *queue,void *item);
/**
 * @brief Checks if the circular queue is empty.
 *
 * This function determines whether the specified circular queue contains any elements.
 *
 * @param queue Pointer to the CQueue_t structure representing the queue.
 * @return true if the queue is empty, false otherwise.
 */
bool cqueue_is_empty(CQueue_t *queue);

bool cqueue_is_full(CQueue_t *queue);


#ifdef __cplusplus
}
#endif

#endif
