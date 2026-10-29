# Architecture and Testing

This document covers the internal protocol mechanics, module layout, and testing tools for `esp-cf-tunnel`. For user-facing configuration and API usage, see [README.md](../README.md) and [API.md](../API.md).

## Protocol and Architecture

### Connection Sequence

- The connector resolves edge addresses from `region1.v2.argotunnel.com` and `region2.v2.argotunnel.com` on port 7844 (the targets of the `_v2-origintunneld._tcp.argotunnel.com` SRV record).
- The connector establishes a TLS 1.3 session with SNI `h2.cftunnel.com`. The edge certificate is signed by the Cloudflare Origin SSL ECC CA, which is embedded in `src/cft_ca.h`.
- The device acts as the TLS client and the HTTP/2 server, while the edge acts as the HTTP/2 client and opens all streams. The first stream is the control stream, which carries Cap'n Proto RPC. On this stream, the device calls `RegistrationServer.registerConnection` with the token's account tag, tunnel ID, and secret (`src/cft_rpc.c`).
- After registration, the edge opens configuration update streams (carrying JSON) and one stream per visitor request. The connector serializes response headers with base64 into the `cf-cloudflared-response-headers` header (`src/cft_conn.c`).

### Module Layout

The `src/` directory contains the portable core:

- `cft_conn.c`: manages a single HTTP/2 connection without performing direct I/O.
- `cft_tunnel.c`: supervises edge connections over a `cft_transport_ops_t` interface and routes incoming requests through `cft_ingress.c`.
- `cft_proxy.c`: forwards requests to origin hosts over non-blocking sockets in the shared `select()` loop (`cft_poll.c`).
- `port/esp/`: implements the ESP-IDF transport (mbedTLS over a non-blocking lwIP socket) and runs the tunnel in a FreeRTOS task.

Repository directories:

- `include/`, `src/`, `port/esp/`: the ESP-IDF component.
- `examples/tunnel_ethernet/`: Ethernet example serving a status page with live updates over a WebSocket (pin defaults match the ESP32-P4 Function EV Board).
- `test_apps/common/`: request handlers shared by the host runner and the device test application.
- `test_apps/host/`: Linux runner (OpenSSL), RPC test, mock server, fake edge, and host test suite.
- `test_apps/device/`: device test application for stress testing (Ethernet, heap integrity checks, and simulated network outages).
- `test_apps/stress/`: stress test load generator, LAN origin services, and log analysis script.
- `tools/capnp_layout.py`: script that prints the Cap'n Proto struct layouts used in `src/cft_rpc.c`.

### Edge Protocol Quirks

- The edge sets `last_stream_id` in its `GOAWAY` frames to one of its own client-initiated stream IDs. Because RFC 9113 requires `last_stream_id` to reference a peer-initiated stream, `nghttp2` rejects these frames as a connection error. `cft_conn.c` rewrites the field to 0 before passing the frame to `nghttp2`.
- When a connector reaches its `SETTINGS_MAX_CONCURRENT_STREAMS` limit, the edge terminates the connection with `GOAWAY` and returns 502 to pending requests. Matching `cloudflared`, this connector does not advertise a stream limit in `SETTINGS`; instead, it rejects requests above `max_concurrent_streams` with HTTP 503.
- Remotely managed tunnels receive configuration updates on connection index 0. The edge routes all requests for the tunnel's public hostnames to the connector regardless of the configured service URL, and the connector enforces the ingress rules locally.
- Holding back the connection-level HTTP/2 receive window for paused streams would bound total buffered request data across a connection, but a few slow origins would then stall all other streams on that connection. `cft_conn.c` returns received bytes to the connection window immediately and withholds only the paused stream's window.

## Host Runner

Building the Linux host runner requires OpenSSL and `nghttp2` development headers.

```sh
cmake -S test_apps/host -B test_apps/host/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS=-fsanitize=address,undefined
cmake --build test_apps/host/build
test_apps/host/build/cft_host --token-file tunnel.token
```

The demo endpoints in `test_apps/host/main.c` (`/echo`, `/upload`, `/pause`, `/big?size=N`, `/later`, `/ws`, and a default request inspection page) handle requests routed to the tunnel. Pressing Ctrl-C unregisters the connections and exits.

Command-line options: `--connections N`, `--connector-id UUID`, `--edge HOST[:PORT]`, `--plain` (plain TCP without TLS, for testing against the fake edge), `--handler-delay-ms N`, `--max-streams N`, `--ingress` (route requests using dashboard ingress rules and proxy to other hosts), `--max-origins N`, `--stream-window N`, `--proxy-buffer N`, and `-v`.

## Host Tests

`test_apps/host/test_host.py` and `test_apps/host/test_proxy.py` run the automated host test suite:

- Registration RPC client tests against `mock_registration_server.py`, which uses the reference Cap'n Proto implementation (`pycapnp`) and the schemas in `test_apps/host/schemas`.
- End-to-end connector tests against `fake_edge.py`, which implements the edge side of the HTTP/2 protocol (control stream, registration, configuration updates, connection draining, connection drops, and unresponsive connections) across HTTP requests, streamed bodies, and WebSockets. With the build above, the connector runs under AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer, and the tests fail on any sanitizer report.
- Ingress routing and proxy tests against HTTP, WebSocket, and TCP origins listening on `127.0.0.2`.
- Randomized stress tests in `test_chaos.py`, which run concurrent operations for 30 seconds (`CFT_CHAOS_SECONDS` overrides the duration) while visitors disconnect mid-transfer and edge connections drop, drain, or stall. When the run finishes, all streams must close cleanly. A second pass injects random allocation failures (`fault.c`) to verify that the connector recovers without memory leaks or stuck streams.

```sh
pip install pytest pycapnp h2
pytest test_apps/host
```

To verify thread safety on the `cft_tunnel_post()` path, build with `-DCMAKE_C_FLAGS=-fsanitize=thread` in a separate build directory and set `CFT_HOST_BUILD` to that path.

## Stress Test

`test_apps/stress/load.py` generates sustained load against `test_apps/device` through the production Cloudflare edge. The workload mixes concurrent request bursts, large and abandoned downloads and uploads, slow readers, WebSockets (echo, server push, long-lived idle connections, and abrupt disconnects), cross-thread responses, stream resets, proxied HTTP and `cloudflared access tcp` transfers to `origin.py` on a LAN host (including failing origins), and simulated Ethernet outages.

The load generator verifies every response, classifies failures into outage-related errors, 503 load-shedding responses, and edge errors, and records device heap statistics during quiet checkpoints to detect memory leaks. The device application builds with heap poisoning, stack overflow protection, and internal memory counters (`sdkconfig.psram` enables the PSRAM configuration). `analyze.py` correlates test failures against the device serial log.

```sh
# on another LAN host
python3 test_apps/stress/origin.py 192.168.1.10
# device: set TUNNEL_TOKEN and the LAN_* services in test_apps/device/.env
idf.py -C test_apps/device build flash monitor
# load, from anywhere
uv run --with httpx --with websockets test_apps/stress/load.py \
    --url https://your-hostname --duration 10800 --out stress-out
```
