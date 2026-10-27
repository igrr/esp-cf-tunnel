/*
 * Request handlers shared by the host runner and the device test app, for
 * the host tests and the stress test (test_apps/stress). Endpoints:
 *
 *   /                hello page showing the request
 *   /echo            POST: the body, streamed back as it arrives
 *   /upload          POST: whole body (16 KiB limit), then "received N bytes"
 *   /sink            POST: streamed body, then "N HASH" (FNV-1a 64, hex)
 *   /big?size=N      N bytes of 'a'..'z' repeating, written as the edge takes them
 *   /slow?size=N&rate=R  the same pattern, R bytes per tick (50 ms)
 *   /later?ms=N      "later" after N ms (default 200), answered from the ticker
 *   /pause           POST: pauses the body for a second, then "received N bytes"
 *   /reset?after=N   N bytes, then RST_STREAM
 *   /ws              WebSocket echo; the text message "close" closes
 *   /ws/push?n=N&ms=M  WebSocket: N messages "push I", one every M ms, then close
 *   /stats           counters as JSON
 *   /lan/...         proxied to cfg.lan_http (path unchanged)
 *   /landead/...     proxied to cfg.lan_dead (expected to fail with 502)
 *   /tcp             WebSocket: proxied to cfg.lan_tcp (TCP over WebSocket)
 *   /chaos/...       passed to cfg.chaos
 *
 * Everything runs on the tunnel's thread, except the ticker and poster
 * threads, which only call cft_tunnel_post().
 */
#pragma once

#include <stddef.h>

#include "cf_tunnel.h"

typedef struct {
    const char *lan_http; /* service URL for /lan/, or NULL */
    const char *lan_tcp;  /* service URL for /tcp, or NULL */
    const char *lan_dead; /* service URL for /landead/, or NULL */
    int handler_delay_ms; /* sleep in on_request (blocks the tunnel thread) */
    int poster_threads;   /* extra threads posting no-op work, to load the post queue */
    /* Appends platform figures to the /stats JSON object: ",\"name\":value..." */
    int (*platform_stats)(char *buf, size_t size);
    /* Handles /chaos/... requests; NULL answers 404 */
    void (*chaos)(cft_stream_t *s, const cft_request_t *req);
} th_config_t;

/* Call before the tunnel runs; cfg is copied (not the strings) */
void th_init(const th_config_t *cfg);
/* Starts the ticker and poster threads */
int th_start(cft_tunnel_t *tunnel);
/* Stops them; call before cft_tunnel_free() */
void th_stop(void);

/* on_request and on_event for cft_tunnel_config_t */
void th_on_request(void *arg, cft_stream_t *s, const cft_request_t *req);
void th_on_event(void *arg, const cft_event_t *ev);
