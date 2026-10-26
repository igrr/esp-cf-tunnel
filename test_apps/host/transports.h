#pragma once

#include "cf_tunnel.h"

/* TLS over a non-blocking socket using OpenSSL, for host builds */
extern const cft_transport_ops_t cft_transport_openssl;

/* Plain TCP, for tests against fake_edge.py */
extern const cft_transport_ops_t cft_transport_plain;
