#include <stdlib.h>

#include "cft_poll.h"

struct cft_poll_entry {
    struct cft_poll_entry *next;
    struct cft_poller *owner;
    int fd;
    unsigned want;
    int64_t deadline_ms;
    cft_poll_fn fn;
    void *arg;
    bool removed; /* freed by the next sweep, so callbacks can remove entries safely */
};

struct cft_poller {
    cft_poll_entry_t *entries;
    unsigned count;
    unsigned max;
};

cft_poller_t *cft_poller_new(unsigned max_entries)
{
    cft_poller_t *p = calloc(1, sizeof(cft_poller_t));
    if (p) {
        p->max = max_entries;
    }
    return p;
}

static void sweep(cft_poller_t *p)
{
    for (cft_poll_entry_t **pp = &p->entries; *pp;) {
        cft_poll_entry_t *e = *pp;
        if (e->removed) {
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

void cft_poller_free(cft_poller_t *p)
{
    if (!p) {
        return;
    }
    for (cft_poll_entry_t *e = p->entries; e; e = e->next) {
        e->removed = true;
    }
    sweep(p);
    free(p);
}

bool cft_poller_full(const cft_poller_t *p)
{
    return p->count >= p->max;
}

cft_poll_entry_t *cft_poll_add(cft_poller_t *p, int fd, cft_poll_fn fn, void *arg)
{
    if (p->count >= p->max) {
        return NULL;
    }
    cft_poll_entry_t *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->owner = p;
    e->fd = fd;
    e->fn = fn;
    e->arg = arg;
    e->next = p->entries;
    p->entries = e;
    p->count++;
    return e;
}

void cft_poll_want(cft_poll_entry_t *e, unsigned events)
{
    e->want = events & (CFT_POLL_READ | CFT_POLL_WRITE);
}

unsigned cft_poll_wanted(const cft_poll_entry_t *e)
{
    return e->want;
}

void cft_poll_deadline(cft_poll_entry_t *e, int64_t deadline_ms)
{
    e->deadline_ms = deadline_ms;
}

void cft_poll_remove(cft_poll_entry_t *e)
{
    if (!e->removed) {
        e->removed = true;
        e->owner->count--;
    }
}

void cft_poller_prepare(cft_poller_t *p, fd_set *rfds, fd_set *wfds, int *maxfd, int64_t *wake_ms)
{
    sweep(p);
    for (cft_poll_entry_t *e = p->entries; e; e = e->next) {
        if (e->want & CFT_POLL_READ) {
            FD_SET(e->fd, rfds);
        }
        if (e->want & CFT_POLL_WRITE) {
            FD_SET(e->fd, wfds);
        }
        if (e->want && e->fd > *maxfd) {
            *maxfd = e->fd;
        }
        if (e->deadline_ms && e->deadline_ms < *wake_ms) {
            *wake_ms = e->deadline_ms;
        }
    }
}

void cft_poller_dispatch(cft_poller_t *p, const fd_set *rfds, const fd_set *wfds, int64_t now_ms)
{
    /* New entries are added at the head, so they are not visited in this
     * round; removed ones stay in the list until the next sweep. */
    for (cft_poll_entry_t *e = p->entries; e; e = e->next) {
        if (e->removed) {
            continue;
        }
        unsigned ev = 0;
        if ((e->want & CFT_POLL_READ) && FD_ISSET(e->fd, rfds)) {
            ev |= CFT_POLL_READ;
        }
        if ((e->want & CFT_POLL_WRITE) && FD_ISSET(e->fd, wfds)) {
            ev |= CFT_POLL_WRITE;
        }
        if (e->deadline_ms && now_ms >= e->deadline_ms) {
            ev |= CFT_POLL_TIMEOUT;
            e->deadline_ms = 0;
        }
        if (ev) {
            e->fn(e->arg, ev);
        }
    }
}
