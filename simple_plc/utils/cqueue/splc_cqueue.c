#include "splc_cqueue.h"

#include <string.h>

splc_cqueue_t *splc_cqueue_init_static(splc_cqueue_t *queue, void *buff,
                                       size_t len, size_t size)
{
    if (queue == NULL || buff == NULL || len == 0U || size == 0U) {
        return NULL;
    }
    queue->buff  = buff;
    queue->len   = len;
    queue->size  = size;
    queue->count = 0U;
    queue->head  = 0U;
    queue->tail  = 0U;
    return queue;
}

bool splc_cqueue_receive(splc_cqueue_t *queue, void *item)
{
    if (splc_cqueue_is_empty(queue)) {
        return false;
    }
    memcpy(item, (uint8_t *)queue->buff + queue->tail * queue->size, queue->size);
    queue->tail = (queue->tail + 1U) % queue->len;
    queue->count -= 1U;
    return true;
}

bool splc_cqueue_send(splc_cqueue_t *queue, const void *item)
{
    if (splc_cqueue_is_full(queue)) {
        return false;
    }
    memcpy((uint8_t *)queue->buff + queue->head * queue->size, item, queue->size);
    queue->head = (queue->head + 1U) % queue->len;
    queue->count += 1U;
    return true;
}

bool splc_cqueue_is_empty(const splc_cqueue_t *queue)
{
    return queue->count == 0U;
}

bool splc_cqueue_is_full(const splc_cqueue_t *queue)
{
    return queue->count == queue->len;
}