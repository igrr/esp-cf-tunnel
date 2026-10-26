# esp-cf-tunnel: Cloudflare Tunnel connector for ESP-IDF

The device opens a tunnel connection to the Cloudflare edge itself and serves
the HTTP requests that arrive on it, without cloudflared or a proxy host. It
speaks cloudflared's HTTP/2 transport.

Status: works against the production edge from a Linux host and from an
ESP32-P4 over Ethernet. Not yet supported: WebSockets, streaming request or
response bodies, a second (redundant) connection.

## Usage

Create a remotely managed tunnel in the Cloudflare dashboard, add a public
hostname to it (the service URL is not used; the device answers every
request routed to the tunnel), and pass the tunnel token to the component:

```c
#include "cf_tunnel_esp.h"

static void handler(void *arg, const cft_request_t *req, cft_response_t *resp)
{
    cft_response_add_header(resp, "Content-Type", "text/plain");
    cft_response_set_body(resp, "hello\n", 6);
}

cft_tunnel_config_t cfg = {
    .token = tunnel_token,
    .handler = handler,
};
cft_tunnel_handle_t tunnel;
ESP_ERROR_CHECK(cft_tunnel_start(&cfg, &tunnel));
```

The handler runs in the tunnel task once the full request has arrived.
Request bodies over `max_request_body` (16 KiB by default) get 413.

The component needs `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y`: the edge refuses
TLS 1.2. The edge sends full 16 KB TLS records, but the device's own records
can be small: `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` with
`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` saves 12 KB.
(`CONFIG_MBEDTLS_DYNAMIC_BUFFER` saves more, but IDF 5.x does not allow it
together with TLS 1.3.)

Measured on ESP32-P4 (internal RAM, no PSRAM; IDF v6.2-dev): the connected
tunnel uses about 55 KB of heap when idle (including the 8 KB task stack) and
about 90 KB at peak during a burst of 50 concurrent requests. The tunnel task
used at most about 3 KB of its stack. Flash: 17 KB for this component and 79 KB
for nghttp2.

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

`src/` holds the portable core, which does no I/O; `port/esp/` connects it to
esp-tls and runs it in a task.

### Edge behaviour worth knowing

- The edge's GOAWAY carries one of its own stream IDs as `last_stream_id`.
  nghttp2 rejects that, so `cft_conn.c` rewrites the field to 0 on receive.
- The edge does not queue requests when the connector's
  SETTINGS_MAX_CONCURRENT_STREAMS is reached: it sends GOAWAY and fails the
  pending requests with 502. cloudflared advertises no limit, so we don't
  either; requests over `max_concurrent_streams` get 503 from the connector.
- Remotely managed tunnels get a configuration push on connection index 0.
  The connector itself enforces the ingress rules, so every hostname routed
  to the tunnel reaches the handler.

## Layout

- `include/`, `src/`, `port/esp/`: the component
- `examples/tunnel_ethernet/`: Ethernet example serving a status page
  (pin defaults for the ESP32-P4 Function EV Board)
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

Every request routed to the tunnel is answered by the demo handler in
`test_apps/host/main.c`. Ctrl-C unregisters the connection.

## Host tests

`test_apps/host/test_host.py` runs the host tests, including the
registration RPC client against a mock server built on the reference Cap'n
Proto implementation (pycapnp). The schemas it needs are in
`test_apps/host/schemas`.

```sh
pip install pytest pycapnp
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
