#include "cqueue.h"
#include <string.h>


CQueue_t *cqueue_init_static(CQueue_t *queue,void *buff,size_t len, size_t size){
    queue->buff = buff;
    queue->len = len;
    queue->size = size;
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
    return queue;
}
CQueue_t *cqueue_init(CQueue_t *queue,size_t len,size_t size){
    queue->buff = NULL;
    queue->len = len;
    queue->size = size;
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;

    queue->buff = (void*) malloc(queue->size * queue->len);

    CQUEUE_ASSERT(queue->buff != NULL);

    return queue;
}
bool cqueue_receive(CQueue_t *queue, void *item){
    if(!cqueue_is_empty(queue)){
        memcpy(item,queue->buff+queue->tail*queue->size,queue->size);
        queue->tail = (queue->tail+1) % queue->len;
        queue->count -= 1;
//        CQUEUE_DEBUG("Receiver Item 0X%02X , head %d , tail %d\n",*(uint8_t*)item,queue->head,queue->tail);
        return true;
    }
    CQUEUE_DEBUG("The Queue is empty\n");
    return false;
}
bool cqueue_send(CQueue_t *queue,void *item){
    if(!cqueue_is_full(queue)){
        memcpy(queue->buff+queue->head*queue->size,item,queue->size);
        int next = (queue->head+1) % queue->len;
        queue->head = next;
        queue->count += 1;
//        CQUEUE_DEBUG("Send Item 0X%02X, head %d , tail %d\n",*(uint8_t*)item,queue->head,queue->tail);
        return true;
    }
    CQUEUE_DEBUG("The Queue is full\n");
    return false;
}
bool cqueue_is_empty(CQueue_t *queue){
    return (queue->count == 0) ? true : false;
}
bool cqueue_is_full(CQueue_t *queue){
    return (queue->count == queue->len) ? true : false;
}
