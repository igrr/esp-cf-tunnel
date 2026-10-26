# esp-cf-tunnel: Cloudflare Tunnel connector for ESP-IDF

The device opens a tunnel connection to the Cloudflare edge itself and serves
the HTTP requests that arrive on it, without cloudflared or a proxy host. It
speaks cloudflared's HTTP/2 transport.

Status: works against the production edge from a Linux host and from an
ESP32-P4 over Ethernet: HTTP requests with streamed request and response
bodies, WebSockets, up to four redundant connections, and proxying to other
hosts on the network (HTTP, WebSockets, and TCP for `cloudflared access`).
Not supported: private network routing (WARP), the QUIC transport.

## Usage

Create a remotely managed tunnel in the Cloudflare dashboard, add a public
hostname to it, and pass the tunnel token to the component. By default the
device answers every request routed to the tunnel, whatever the hostname's
service URL; see [Reaching other devices](#reaching-other-devices) for
routing by service.

```c
#include "cf_tunnel_esp.h"

static void on_request(void *arg, cft_stream_t *s, const cft_request_t *req)
{
    cft_stream_respond(s, 200, "text/plain", "hello\n", 6);
}

cft_tunnel_config_t cfg = {
    .token = tunnel_token,
    .on_request = on_request,
};
cft_tunnel_handle_t tunnel;
ESP_ERROR_CHECK(cft_tunnel_start(&cfg, &tunnel));
```

`on_request` runs in the tunnel task as soon as the request headers arrive.
From there, a handler can:

- respond at once: `cft_stream_respond()`;
- collect the body first: `cft_stream_read_body(s, max_len, fn, ctx)`, which
  answers 413 to bodies over the limit;
- stream: set callbacks with `cft_stream_set_handlers()` (`on_body`,
  `on_body_end`, `on_writable`, `on_close`) and send the response with
  `cft_stream_send_headers()`, `cft_stream_write()` and `cft_stream_end()`.
  `on_writable` fires once the queued data has been passed on, which is the
  moment to write the next part of a large response;
- accept a WebSocket (`req->is_websocket`): `cft_ws_accept()`, then
  `on_ws_message` gets each message and `cft_ws_send()` sends. Fragmentation,
  pings and the close handshake are handled by the component.

```c
static void ws_message(cft_stream_t *s, void *ctx, cft_ws_opcode_t op,
                       const uint8_t *data, size_t len)
{
    cft_ws_send(s, op, data, len); /* echo */
}

    /* in on_request */
    if (req->is_websocket) {
        cft_stream_handlers_t h = {.on_ws_message = ws_message};
        cft_stream_set_handlers(s, &h, NULL);
        cft_ws_accept(s, NULL);
    }
```

All stream functions must be called from the tunnel task. To respond or send
WebSocket messages from another task, use `cft_tunnel_post()`, which runs a
function in the tunnel task within about 100 ms. A stream is valid until its
`on_close` callback. See [API.md](API.md) for the full API, and
`examples/tunnel_ethernet` and `test_apps/host/main.c` for complete handlers.

Connections: by default the tunnel keeps one connection to the edge
(`num_connections`, up to 4, spread over both edge regions). When the edge
drains a connection, a replacement is opened before the old one closes; a
lost connection is reconnected with backoff, and a connection that stops
answering HTTP/2 PINGs is dropped within about 25 s. On ESP-IDF the connector
ID is derived from the MAC address, so after a reboot the edge replaces the
old connections at once instead of routing requests to them until they time
out.

### Reaching other devices

Like cloudflared, the device can pass requests on to other hosts on its
network. With `use_ingress_rules` set, requests are routed by the public
hostnames configured for the tunnel in the dashboard, which the edge sends to
the device:

| Service in the dashboard | Handled by |
|---|---|
| `http://localhost:PORT`, `http://127.0.0.1:PORT` | `on_request` on the device (`req->service` tells which) |
| `http://HOST:PORT`, `ws://HOST:PORT` | HTTP/1.1 proxy to that host, WebSockets included |
| `ssh://`, `rdp://`, `smb://`, `tcp://HOST:PORT` | TCP over WebSocket, for `cloudflared access ssh/rdp/tcp` on the visitor's side |
| `http_status:CODE` | empty response with that status |

```c
cft_tunnel_config_t cfg = {
    .token = tunnel_token,
    .on_request = on_request,   /* for services on localhost */
    .use_ingress_rules = true,
};
```

Hostname wildcards (`*.example.com`), path regexes (POSIX extended syntax,
which covers the common cases of Go's RE2) and `httpHostHeader` work as in
cloudflared. Until the rules arrive, requests go to `on_request`. A handler
can also proxy a request itself with `cft_stream_proxy(s, "http://...")`.

Each proxied request opens its own connection to the origin, at most
`max_origin_connections` at a time (default 4; more get 503). The proxy
buffers at most about 16 KB in each direction per request, plus what the
edge has in flight: an origin that reads slowly pauses the visitor's upload
through HTTP/2 flow control (`cft_stream_pause_body()`, also available to
handlers, e.g. for writing uploads to flash). Not supported: `https://`
origins, unix sockets, cloudflared's built-in services, and private network
routing (WARP). Host names in service URLs are resolved on the tunnel task,
which blocks it briefly; IP addresses avoid that.

The device connects only to hosts named in the tunnel's rules (or by the
application), and anyone who can edit the tunnel in the dashboard can point
it at any host the device can reach. Use Cloudflare Access on the public
hostnames as you would with cloudflared.

### TLS and memory

The component needs `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y`: the edge refuses
TLS 1.2. The edge sends full 16 KB TLS records, but the device's own records
can be small: `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` with
`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` saves 12 KB.
(`CONFIG_MBEDTLS_DYNAMIC_BUFFER` saves more, but IDF 5.x does not allow it
together with TLS 1.3.)

Measured on ESP32-P4 (internal RAM, no PSRAM; IDF v6.2-dev; asymmetric TLS
buffers as above): one connected tunnel connection uses about 49 KB of heap
when idle (including the 8 KB task stack) and about 71 KB at peak, during a
WebSocket session followed by a burst of 50 concurrent requests. The tunnel
task used at most about 3 KB of its stack. Flash: 23 KB for this component and
79 KB for nghttp2.

Proxying to another host on the LAN, one request at a time, raised the peak
by about 50 KB for HTTP (a 5 MB download at about 450 KB/s, a 3 MB upload at
about 270 KB/s, an upload to an origin that stalls) and about 60 KB for a
`cloudflared access tcp` session. Most of it is data the edge may send before
the device's flow control stops it (64 KB per stream). These figures are for
IDF's default lwIP settings; see below for faster ones.

### Throughput

Each connection's speed is bounded by window / round-trip time, and with
IDF's defaults the bound is lwIP's: `CONFIG_LWIP_TCP_WND_DEFAULT` and
`CONFIG_LWIP_TCP_SND_BUF_DEFAULT` are 5760 bytes, about 0.4 MB/s at the
15-20 ms the device had to the edge. Settings, from the largest effect:

| Setting | Effect |
|---|---|
| `CONFIG_LWIP_TCP_WND_DEFAULT`, `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 32768 instead of 5760: downloads and uploads about 6-10 times faster. Raise `CONFIG_LWIP_TCP_RECVMBOX_SIZE` and `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` with them (64). 65535 was not faster. |
| `stream_window` | HTTP/2 window per stream, 64 KiB by default: uploads (request bodies) are bounded by it once lwIP's window is larger. 128 KiB about doubled proxied uploads; 256 KiB was a little faster still. |
| `proxy_buffer_size` | 16 KiB by default; 64 KiB made proxied downloads 10-15 % faster. |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | 16384 instead of 4096 was not faster, and used 12 KB more. |

Measured on the ESP32-P4 over 100 Mbit Ethernet with 5 MB transfers (3 MB
for proxied uploads), through the production edge from a visitor with
60 MB/s to Cloudflare. Rates depend on the edge location the device gets:
the same build was about 1.7 times faster through Prague (prg) than through
Frankfurt (fra).

| Configuration | Edge | Download | Upload | Proxied download | Proxied upload | Peak heap above idle |
|---|---|---|---|---|---|---|
| IDF defaults (lwIP 5760), component defaults | fra | 0.4 MB/s | 0.2-0.4 MB/s | 0.3 MB/s | 0.2 MB/s | 18 KB, proxied 51 KB |
| lwIP 32768 | prg | 3.3-4 MB/s | 0.9-2.7 MB/s | 3-3.2 MB/s | 0.8-0.9 MB/s | 75-82 KB, proxied 107 KB |
| lwIP 32768, `stream_window` 128 KiB (the example) | fra | 1.8-2.3 MB/s | 0.8-2 MB/s | 1.9-2 MB/s | 1.7-1.9 MB/s | 47 KB, proxied 107 KB |
| lwIP 32768, `stream_window` 256 KiB | fra | 2.1-2.5 MB/s | 1.3-2.2 MB/s | 1.9-2.1 MB/s | 1.9-2.1 MB/s | 47 KB, proxied 107 KB |
| lwIP 32768, `stream_window` 256 KiB | prg | 4-4.1 MB/s | 1.9-4.2 MB/s | 3.8 MB/s | 3.3 MB/s | 76 KB, proxied 107 KB |

Ranges span repeated runs; the first upload after connecting is the slowest
(TCP slow start).

Idle memory does not change with these settings; buffers grow during
transfers. A larger `stream_window` costs nothing while the receiver keeps
up, but a stream whose receiver is slow or paused can hold up to the whole
window: a proxied upload to a slow host, or a handler using
`cft_stream_pause_body()`. With `max_origin_connections` uploads at a time,
that is up to that many windows. `examples/tunnel_ethernet` uses lwIP 32768
and a 128 KiB window; both are in its `sdkconfig.defaults` and menuconfig.

## How it works

- The edge address comes from `region1.v2.argotunnel.com` /
  `region2.v2.argotunnel.com`, port 7844 (the targets of the
  `_v2-origintunneld._tcp.argotunnel.com` SRV record).
- TLS 1.3 with SNI `h2.cftunnel.com`. The certificate is signed by the
  Cloudflare Origin SSL ECC CA, which is embedded in `src/cft_ca.h`.
- The device is the TLS client but the HTTP/2 *server*: the edge opens the
  streams. The first one is the control stream, carrying Cap'n Proto RPC: the
  device calls `RegistrationServer.registerConnection` with the token's
  account tag, tunnel ID and secret (`src/cft_rpc.c`).
- The edge then sends configuration updates (JSON) and one stream per visitor
  request. Response headers are also sent base64-encoded in
  `cf-cloudflared-response-headers` (`src/cft_conn.c`).

`src/` holds the portable core: `cft_conn.c` handles one connection and does
no I/O, `cft_tunnel.c` supervises the connections over a `cft_transport_ops_t`
and routes requests (`cft_ingress.c`), and `cft_proxy.c` talks to other hosts
over non-blocking sockets in the same select loop (`cft_poll.c`).
`port/esp/` provides the esp-tls transport and runs the tunnel in a task.

### Edge behaviour worth knowing

- The edge's GOAWAY carries one of its own stream IDs as `last_stream_id`.
  nghttp2 rejects that, so `cft_conn.c` rewrites the field to 0 on receive.
- The edge does not queue requests when the connector's
  SETTINGS_MAX_CONCURRENT_STREAMS is reached: it sends GOAWAY and fails the
  pending requests with 502. cloudflared advertises no limit, so we don't
  either; requests over `max_concurrent_streams` get 503 from the connector.
- Remotely managed tunnels get a configuration push on connection index 0.
  The connector itself enforces the ingress rules: the edge sends it every
  request routed to the tunnel, whatever the hostname's service.

## Layout

- `include/`, `src/`, `port/esp/`: the component
- `examples/tunnel_ethernet/`: Ethernet example serving a status page with
  live updates over a WebSocket (pin defaults for the ESP32-P4 Function EV
  Board)
- `test_apps/host/`: Linux runner (OpenSSL), RPC test and mock server
- `tools/capnp_layout.py`: prints the Cap'n Proto struct layouts used in
  `src/cft_rpc.c`

## Example

`examples/tunnel_ethernet` reads the token from a `.env` file in the example
directory using [nvs-dotenv](https://components.espressif.com/components/igrr/nvs-dotenv):
the build turns `.env` into an image for the `dotenv` NVS partition, and
`idf.py flash` writes it together with the app.

```sh
cd examples/tunnel_ethernet
cp .env.example .env    # then set TUNNEL_TOKEN
idf.py build flash monitor
```

`.env` is ignored by git. `TUNNEL_TOKEN` is the variable cloudflared reads
the token from, too.

## Host runner

Needs OpenSSL and nghttp2 development files.

```sh
cmake -S test_apps/host -B test_apps/host/build -G Ninja
cmake --build test_apps/host/build
test_apps/host/build/cft_host --token-file tunnel.token
```

Requests routed to the tunnel are answered by the demo endpoints in
`test_apps/host/main.c` (`/echo`, `/upload`, `/pause`, `/big?size=N`,
`/later`, `/ws`, and a page showing the request for anything else). Ctrl-C
unregisters the connections. Options: `--connections N`, `--connector-id
UUID`, `--edge HOST[:PORT]`, `--plain` (no TLS, for the fake edge),
`--handler-delay-ms N`, `--max-streams N`, `--ingress` (route by the
dashboard's public hostnames, proxying to other hosts), `--max-origins N`,
`--stream-window N`, `--proxy-buffer N`, `-v`.

## Host tests

`test_apps/host/test_host.py` and `test_proxy.py` run the host tests:

- the registration RPC client against a mock server built on the reference
  Cap'n Proto implementation (pycapnp), with schemas in `test_apps/host/schemas`;
- the host runner against `fake_edge.py`, which plays the edge's side of the
  protocol (HTTP/2 client, control stream, registration, configuration
  pushes, drains, lost and unresponsive connections), covering streamed
  bodies and WebSockets too. The connector runs with AddressSanitizer and
  UndefinedBehaviorSanitizer, and a leak check at exit;
- proxying by ingress rules to HTTP, WebSocket and TCP origins on 127.0.0.2,
  standing in for other hosts.

```sh
pip install pytest pycapnp h2
pytest test_apps/host
```

## Contributing

1. Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/);
   [commitizen](https://commitizen-tools.github.io/commitizen/) (`cz commit`)
   helps with the format.
2. Install the pre-commit hooks before making changes: `pre-commit install`.
   They format C code with astyle and regenerate [API.md](API.md) from the
   public headers (needs doxygen).
3. Releases: `cz bump` updates CHANGELOG.md and tags the version; pushing a
   `v*` tag publishes the component to the ESP Component Registry.

## License

Apache 2.0, see [LICENSE.txt](LICENSE.txt). The Cap'n Proto schemas in
`test_apps/host/schemas` keep their own licenses (see the README there).
