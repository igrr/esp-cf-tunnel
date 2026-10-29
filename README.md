# esp-cf-tunnel: Cloudflare Tunnel connector for ESP-IDF

`esp-cf-tunnel` connects an ESP-IDF device directly to the Cloudflare edge over `cloudflared`'s HTTP/2 transport and serves incoming HTTP requests without a separate `cloudflared` host.

Supported features include HTTP requests with streamed request and response bodies, WebSockets, up to four redundant edge connections, and proxying to other hosts on the local network (HTTP, WebSockets, and TCP for `cloudflared access`). Private network routing (WARP) and the QUIC transport are not supported.

## Usage

Create a remotely managed tunnel in the Cloudflare dashboard, configure a public hostname for it, and pass the tunnel token to the component. By default, the device handles every request routed to the tunnel regardless of the hostname's configured service URL; see [Reaching other devices](#reaching-other-devices) to route requests by service.

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

`on_request` runs on the tunnel task as soon as the request headers arrive. From this callback, a handler can:

- Respond immediately with `cft_stream_respond()`.
- Buffer the request body with `cft_stream_read_body(s, max_len, fn, ctx)`, which returns HTTP 413 if the body exceeds `max_len`.
- Stream the request and response by registering callbacks with `cft_stream_set_handlers()` (`on_body`, `on_body_end`, `on_writable`, `on_close`) and sending response data with `cft_stream_send_headers()`, `cft_stream_write()`, and `cft_stream_end()`. `on_writable` runs when queued response data has been handed to the HTTP/2 session, allowing the handler to write the next chunk of a large response.
- Accept a WebSocket request (`req->is_websocket`) with `cft_ws_accept()`. Incoming messages arrive at `on_ws_message`, and `cft_ws_send()` sends outgoing messages. The component handles frame reassembly, pings, and the close handshake.

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

### Threading and Connections

All stream functions must be called from the tunnel task. To send responses or WebSocket messages from another task, call `cft_tunnel_post()`, which schedules a callback on the tunnel task within 100 ms. A stream pointer remains valid until its `on_close` callback returns. See [API.md](API.md) for the complete API reference, and `examples/tunnel_ethernet` and `test_apps/host/main.c` for full handler examples.

By default, the tunnel maintains one edge connection (`num_connections` supports 1 to 4 connections distributed across both edge regions). When the edge drains a connection, the supervisor opens a replacement before closing the draining connection. Disconnected slots reconnect with exponential backoff, and connections that stop answering HTTP/2 PING frames are closed after 25 seconds. On ESP-IDF, the connector derives a stable connector ID from the base MAC address and tunnel ID so that the edge immediately replaces connections from a previous boot after a restart.

### Reaching Other Devices

When `use_ingress_rules` is enabled, the connector routes incoming requests using the public hostname rules configured in the Cloudflare dashboard and pushed by the edge:

| Service in the dashboard | Handled by |
|---|---|
| `http://localhost:PORT`, `http://127.0.0.1:PORT` | `on_request` on the device (`req->service` contains the matched service URL) |
| `http://HOST:PORT`, `ws://HOST:PORT` | HTTP/1.1 proxy to that host, including WebSockets |
| `ssh://`, `rdp://`, `smb://`, `tcp://HOST:PORT` | TCP over WebSocket, compatible with `cloudflared access ssh/rdp/tcp` |
| `http_status:CODE` | Empty response with the specified HTTP status code |

```c
cft_tunnel_config_t cfg = {
    .token = tunnel_token,
    .on_request = on_request,   /* for services on localhost */
    .use_ingress_rules = true,
};
```

Hostname wildcards (`*.example.com`), path regular expressions (POSIX extended syntax), and `httpHostHeader` overrides work as in `cloudflared`. Before the initial configuration arrives from the edge, all requests go to `on_request`. A request handler can also forward an individual stream by calling `cft_stream_proxy(s, "http://...")`.

Each proxied request opens a separate TCP connection to the origin, up to `max_origin_connections` concurrent connections (default 4; additional requests receive HTTP 503). When an origin reads slowly, the proxy pauses the incoming request body through HTTP/2 stream flow control (`cft_stream_pause_body()`, which is also available to local request handlers). Origins using `https://` or `wss://`, Unix domain sockets, `cloudflared` built-in services, and private network routing (WARP) are not supported. Resolving hostnames in service URLs blocks the tunnel task during DNS lookup; using IP addresses avoids this delay.

The connector only connects to origins specified in the tunnel's ingress rules or passed to `cft_stream_proxy()`. Because anyone with permission to edit the tunnel in the Cloudflare dashboard can point rules at hosts on the device's local network, protect public hostnames with Cloudflare Access as you would with `cloudflared`.

## Configuration and Tuning

### TLS and Memory

The component requires `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` because the Cloudflare edge rejects TLS 1.2. While the edge sends full 16 KiB TLS records, outgoing records from the device are smaller: setting `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` and `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` reduces the mbedTLS output buffer per connection. (`CONFIG_MBEDTLS_DYNAMIC_BUFFER` cannot be combined with TLS 1.3 in ESP-IDF 5.x.)

Besides the base TLS and HTTP/2 session state per connection, active streams allocate memory proportional to traffic:

- Each open stream (HTTP request or WebSocket) allocates headers and `nghttp2` stream state.
- Outgoing response data is queued in segments of at most 4 KiB, avoiding large contiguous allocations on a fragmented heap. Handlers streaming large responses should write chunks from `on_writable` instead of queuing the entire body at once.
- The edge can send up to `stream_window` bytes of a request body before flow control pauses the stream (`cft_stream_pause_body()` or a slow proxied origin). Only the paused stream stops; other streams on the connection continue receiving data.
- Each proxied request buffers up to `proxy_buffer_size` bytes in each direction, in addition to data buffered in the lwIP receive window (`CONFIG_LWIP_TCP_WND_DEFAULT`). If a proxied origin accepts no data for 30 seconds, the proxy aborts the transfer.

On chips where mbedTLS encrypts TLS records with the hardware AES DMA engine (such as the ESP32-P4), each record allocates DMA descriptors from internal RAM. If internal RAM is exhausted during peak traffic, the TLS write fails and the connection reconnects. To prevent internal RAM exhaustion:

- Enable PSRAM (as configured in `examples/tunnel_ethernet/sdkconfig.defaults`): allocations above 1 KiB, lwIP buffers, and mbedTLS buffers are placed in PSRAM, while `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` reserves 64 KiB of internal RAM for AES and Ethernet DMA descriptors.
- On boards without PSRAM, set `CONFIG_MBEDTLS_HARDWARE_AES=n` to perform AES in software without DMA descriptor allocations, at the cost of lower throughput. (Disabling only `CONFIG_MBEDTLS_HARDWARE_GCM` is insufficient with mbedTLS 4 in ESP-IDF 6, which still uses the AES DMA engine for GCM.)

When free heap drops below `CONFIG_CF_TUNNEL_MIN_FREE_HEAP` (32 KiB by default), the connector rejects new requests with HTTP 503 so existing connections and streams can complete. If queuing response data fails due to an allocation error, the stream is reset with `RST_STREAM`. Size `max_concurrent_streams`, `max_origin_connections`, `num_connections`, and `stream_window` to match available RAM.

### Throughput

Connection throughput is bounded by the TCP and HTTP/2 window sizes divided by the round-trip time to the edge. With ESP-IDF's default 5760-byte lwIP TCP windows, throughput is limited by TCP flow control. The following settings adjust transfer speed and buffer allocation:

| Setting | Effect |
|---|---|
| `CONFIG_LWIP_TCP_WND_DEFAULT`, `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | Increasing the TCP window and send buffer (for example to 32768) raises download and upload throughput. Increase `CONFIG_LWIP_TCP_RECVMBOX_SIZE` and `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` (for example to 64) alongside them. |
| `stream_window` | Per-stream HTTP/2 receive window (64 KiB by default). Bounds upload throughput once the lwIP TCP window is large enough, and sets the maximum request body data buffered for a paused stream. |
| `proxy_buffer_size` | Per-direction buffer for each proxied request (16 KiB by default). Larger values reduce read/write stalls on fast origins. |

Idle heap usage does not change with these window settings; buffers grow only while transfers are active. `examples/tunnel_ethernet` configures PSRAM, 32 KiB lwIP windows, and a 128 KiB `stream_window` in its `sdkconfig.defaults` and `Kconfig.projbuild`.

## Example

`examples/tunnel_ethernet` reads the tunnel token from a `.env` file in the example directory using [nvs-dotenv](https://components.espressif.com/components/igrr/nvs-dotenv). The build packs `.env` into an NVS partition image (`dotenv`), and `idf.py flash` writes it to the device alongside the application.

```sh
cd examples/tunnel_ethernet
cp .env.example .env    # then set TUNNEL_TOKEN
idf.py build flash monitor
```

`.env` is ignored by git. `TUNNEL_TOKEN` is the same environment variable name used by `cloudflared`.

## Development and Testing

For internal protocol details, repository layout, the Linux host runner, sanitizer and fault-injection tests, and the hardware stress test suite, see [docs/development.md](docs/development.md).

## Contributing

1. Format commit messages according to [Conventional Commits](https://www.conventionalcommits.org/); [commitizen](https://commitizen-tools.github.io/commitizen/) (`cz commit`) can generate them.
2. Install the pre-commit hooks before making changes: `pre-commit install`. The hooks format C source files with `astyle` and regenerate [API.md](API.md) from the public headers using Doxygen.
3. Run `cz bump` to update `CHANGELOG.md` and tag a release; pushing a `v*` tag triggers the GitHub Actions workflow that publishes the component to the ESP Component Registry.

## License

Apache 2.0, see [LICENSE.txt](LICENSE.txt). The Cap'n Proto schemas in `test_apps/host/schemas` retain their upstream licenses (see `test_apps/host/schemas/README.md`).
