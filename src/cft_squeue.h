/*
 * Byte queue in fixed-size segments (internal): data waiting to be sent,
 * which can grow to tens of kilobytes. Unlike a buffer that doubles, it
 * never needs a large contiguous block, which a fragmented heap may not
 * have.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CFT_SEG_MAX 4096 /* largest segment allocated */
#define CFT_SEG_MIN 256  /* smallest; sized to the data in between */

typedef struct cft_seg {
    struct cft_seg *next;
    size_t len;
    size_t off;
    size_t cap;
    uint8_t data[];
} cft_seg_t;

typedef struct {
    cft_seg_t *head;
    cft_seg_t *tail;
    size_t pending; /* bytes queued */
} cft_squeue_t;

/* Returns 0, or -1 if out of memory (then nothing was added) */
int cft_sq_append(cft_squeue_t *q, const void *data, size_t len);
/* printf into the queue; at most 255 bytes */
int cft_sq_printf(cft_squeue_t *q, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Copies up to len bytes from the front into buf and drops them; returns the count */
size_t cft_sq_read(cft_squeue_t *q, uint8_t *buf, size_t len);
/* Drops n <= pending bytes from the front */
void cft_sq_consume(cft_squeue_t *q, size_t n);
void cft_sq_free(cft_squeue_t *q);
