/*
 * Sockets other than the edge connections (origin connections of the proxy),
 * waited on in the tunnel's select loop (internal).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/select.h>

#define CFT_POLL_READ 1u
#define CFT_POLL_WRITE 2u
#define CFT_POLL_TIMEOUT 4u

typedef struct cft_poller cft_poller_t;
typedef struct cft_poll_entry cft_poll_entry_t;

/* Called on the tunnel's thread with CFT_POLL_* flags. May call any
 * cft_poll_* function, including removing this or other entries. */
typedef void (*cft_poll_fn)(void *arg, unsigned events);

/* At most max_entries sockets at a time */
cft_poller_t *cft_poller_new(unsigned max_entries);
/* Entries must have been removed */
void cft_poller_free(cft_poller_t *p);

/* True if cft_poll_add() would be refused for lack of room */
bool cft_poller_full(const cft_poller_t *p);

/* Starts watching fd; nothing is waited for until cft_poll_want(). Returns
 * NULL if the poller is full or memory runs out. */
cft_poll_entry_t *cft_poll_add(cft_poller_t *p, int fd, cft_poll_fn fn, void *arg);
/* Sets what to wait for (CFT_POLL_READ / CFT_POLL_WRITE) */
void cft_poll_want(cft_poll_entry_t *e, unsigned events);
/* Calls fn with CFT_POLL_TIMEOUT at deadline_ms (cft_port_now_ms() time); 0 for none */
void cft_poll_deadline(cft_poll_entry_t *e, int64_t deadline_ms);
/* fn is not called again; does not close the socket */
void cft_poll_remove(cft_poll_entry_t *e);

/* For the select loop: adds the sockets to the sets, raises *maxfd, and
 * lowers *wake_ms to the earliest deadline. */
void cft_poller_prepare(cft_poller_t *p, fd_set *rfds, fd_set *wfds, int *maxfd, int64_t *wake_ms);
/* Calls the callbacks for what select() reported and for expired deadlines */
void cft_poller_dispatch(cft_poller_t *p, const fd_set *rfds, const fd_set *wfds, int64_t now_ms);
