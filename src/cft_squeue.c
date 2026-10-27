#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cft_debug.h"
#include "cft_squeue.h"

int cft_sq_append(cft_squeue_t *q, const void *data, size_t len)
{
    /* Allocate the segments first, so a failure leaves the queue as it was */
    size_t room = q->tail ? q->tail->cap - q->tail->len : 0;
    cft_seg_t *first = NULL, *last = NULL;
    for (size_t need = len > room ? len - room : 0; need;) {
        size_t cap = need < CFT_SEG_MIN ? CFT_SEG_MIN : need > CFT_SEG_MAX ? CFT_SEG_MAX : need;
        cft_seg_t *sg = malloc(sizeof(*sg) + cap);
        if (!sg) {
            while (first) {
                cft_seg_t *next = first->next;
                CFT_DEBUG_ADD(sq_bytes, -(sizeof(*first) + first->cap));
                free(first);
                first = next;
            }
            return -1;
        }
        sg->next = NULL;
        sg->len = sg->off = 0;
        sg->cap = cap;
        CFT_DEBUG_ADD(sq_bytes, sizeof(*sg) + cap);
        CFT_DEBUG_PEAK(sq_bytes, sq_peak);
        if (last) {
            last->next = sg;
        } else {
            first = sg;
        }
        last = sg;
        need = need > cap ? need - cap : 0;
    }
    if (first) {
        if (q->tail) {
            q->tail->next = first;
        } else {
            q->head = first;
        }
    }
    const uint8_t *d = data;
    cft_seg_t *sg = q->tail ? q->tail : first;
    while (len) {
        if (sg->len == sg->cap) {
            sg = sg->next;
        }
        size_t n = sg->cap - sg->len < len ? sg->cap - sg->len : len;
        memcpy(sg->data + sg->len, d, n);
        sg->len += n;
        q->pending += n;
        d += n;
        len -= n;
    }
    if (last) {
        q->tail = last;
    }
    return 0;
}

int cft_sq_printf(cft_squeue_t *q, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(tmp)) {
        return -1;
    }
    return cft_sq_append(q, tmp, (size_t)n);
}

void cft_sq_consume(cft_squeue_t *q, size_t n)
{
    q->pending -= n;
    while (n) {
        cft_seg_t *sg = q->head;
        size_t k = sg->len - sg->off < n ? sg->len - sg->off : n;
        sg->off += k;
        n -= k;
        if (sg->off == sg->len) {
            q->head = sg->next;
            if (!q->head) {
                q->tail = NULL;
            }
            CFT_DEBUG_ADD(sq_bytes, -(sizeof(*sg) + sg->cap));
            free(sg);
        }
    }
}

size_t cft_sq_read(cft_squeue_t *q, uint8_t *buf, size_t len)
{
    size_t done = 0;
    for (cft_seg_t *sg = q->head; sg && done < len; sg = sg->next) {
        size_t k = sg->len - sg->off < len - done ? sg->len - sg->off : len - done;
        memcpy(buf + done, sg->data + sg->off, k);
        done += k;
    }
    cft_sq_consume(q, done);
    return done;
}

void cft_sq_free(cft_squeue_t *q)
{
    while (q->head) {
        cft_seg_t *next = q->head->next;
        CFT_DEBUG_ADD(sq_bytes, -(sizeof(*q->head) + q->head->cap));
        free(q->head);
        q->head = next;
    }
    q->tail = NULL;
    q->pending = 0;
}
