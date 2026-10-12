#ifndef SPLC_CQUEUE_H
#define SPLC_CQUEUE_H

/*
 * splc_cqueue.h - Layer U. SimplePLC's own fixed-size circular queue.
 *
 * File, type and every function carry the splc_ prefix so the library can live
 * next to a host firmware's own cqueue (same API, same file name) without
 * symbol or include-guard clashes -- the same approach as the nanoMODBUS
 * wrapper (docs/handoff.md decision 15). Pure logic: no heap, no hardware, no
 * OS. The storage is always supplied by the caller; there is deliberately no
 * malloc-based init (project rule: no dynamic heap).
 *
 * Not thread-safe by itself: one producer context (e.g. a UART RX ISR) and one
 * consumer context (the engine), with the caller's own protection.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct splc_cqueue {
    void   *buff;
    size_t  size;    /* bytes per element */
    size_t  len;     /* number of elements the buffer holds */
    size_t  head;    /* next write slot */
    size_t  tail;    /* next read slot */
    size_t  count;
} splc_cqueue_t;

/* Set up a queue over a caller-supplied buffer of len elements of size bytes.
 * Returns queue, or NULL when queue or buff is NULL or len/size is 0. */
splc_cqueue_t *splc_cqueue_init_static(splc_cqueue_t *queue, void *buff,
                                       size_t len, size_t size);

/* Remove the oldest element into item. false when the queue is empty. */
bool splc_cqueue_receive(splc_cqueue_t *queue, void *item);

/* Append an element. false when the queue is full. */
bool splc_cqueue_send(splc_cqueue_t *queue, const void *item);

bool splc_cqueue_is_empty(const splc_cqueue_t *queue);
bool splc_cqueue_is_full(const splc_cqueue_t *queue);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_CQUEUE_H */