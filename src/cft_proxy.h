/*
 * Proxy to origins on other hosts (internal): HTTP/1.1, WebSockets, and TCP
 * over WebSocket, as cloudflared's httpService and tcpOverWSService do.
 */
#pragma once

#include "cf_tunnel.h"

/* cft_stream_proxy() with originRequest.httpHostHeader (NULL for none) */
int cft_proxy_start(cft_stream_t *s, const char *service, const char *host_header);
