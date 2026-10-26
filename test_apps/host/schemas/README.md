Cap'n Proto schemas used by `mock_registration_server.py` and
`tools/capnp_layout.py`:

- `tunnelrpc.capnp`, `go.capnp`: from
  [cloudflared](https://github.com/cloudflare/cloudflared) `tunnelrpc/proto/`,
  Apache-2.0.
- `rpc.capnp`, `capnp/c++.capnp`: from
  [go-capnproto2](https://github.com/capnproto/go-capnproto2) `std/capnp/`
  (copies of the Cap'n Proto standard schemas), MIT license; the license text
  is at the top of each file.
