# API Reference

## Header files

- [cf_tunnel.h](#file-cf_tunnelh)
- [cf_tunnel_esp.h](#file-cf_tunnel_esph)

## File cf_tunnel.h

## Structures and Types

| Type | Name |
| ---: | :--- |
| typedef void(\*) | [**cft\_event\_fn**](#typedef-cft_event_fn)  <br>_Called for connection events on the tunnel's thread._ |
| struct | [**cft\_event\_t**](#struct-cft_event_t) <br>_Connection event._ |
| enum  | [**cft\_event\_type\_t**](#enum-cft_event_type_t)  <br>_Connection events._ |
| struct | [**cft\_header\_t**](#struct-cft_header_t) <br>_HTTP header._ |
| typedef void(\*) | [**cft\_request\_fn**](#typedef-cft_request_fn)  <br>_Called when the headers of a new request have arrived._ |
| struct | [**cft\_request\_t**](#struct-cft_request_t) <br>_Request, as received from the edge._ |
| struct | [**cft\_stream\_handlers\_t**](#struct-cft_stream_handlers_t) <br>_Per-stream callbacks, see_ [_**cft\_stream\_set\_handlers()**_](#function-cft_stream_set_handlers) _._ |
| typedef struct cft\_stream | [**cft\_stream\_t**](#typedef-cft_stream_t)  <br>_One request and its response._ |
| struct | [**cft\_transport\_ops\_t**](#struct-cft_transport_ops_t) <br>_TLS client over a non-blocking socket._ |
| struct | [**cft\_tunnel\_config\_t**](#struct-cft_tunnel_config_t) <br>_Tunnel configuration._ |
| typedef struct cft\_tunnel | [**cft\_tunnel\_t**](#typedef-cft_tunnel_t)  <br>_Tunnel handle._ |
| enum  | [**cft\_ws\_opcode\_t**](#enum-cft_ws_opcode_t)  <br>_WebSocket message types._ |

## Functions

| Type | Name |
| ---: | :--- |
|  size\_t | [**cft\_stream\_buffered**](#function-cft_stream_buffered) (const [**cft\_stream\_t**](#typedef-cft_stream_t)\* s) <br>_Response bytes queued but not yet passed to the connection._ |
|  int | [**cft\_stream\_end**](#function-cft_stream_end) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s) <br>_Ends the response once the queued data has been sent._ |
|  const char \* | [**cft\_stream\_header**](#function-cft_stream_header) (const [**cft\_stream\_t**](#typedef-cft_stream_t)\* s, const char \* name) <br>_Value of a request header._ |
|  void | [**cft\_stream\_pause\_body**](#function-cft_stream_pause_body) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, bool pause) <br>_Stops or resumes the flow of request body data._ |
|  int | [**cft\_stream\_proxy**](#function-cft_stream_proxy) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, const char \* service) <br>_Passes the request on to another host, as cloudflared does._ |
|  int | [**cft\_stream\_read\_body**](#function-cft_stream_read_body) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, size\_t max\_len, void(\*)([**cft\_stream\_t**](#typedef-cft_stream_t)\*s, void \*ctx, const uint8\_t \*body, size\_t len) fn, void \* ctx) <br>_Collects the request body, then calls a function with it._ |
|  const [**cft\_request\_t**](#struct-cft_request_t)\* | [**cft\_stream\_request**](#function-cft_stream_request) (const [**cft\_stream\_t**](#typedef-cft_stream_t)\* s) <br>_Request of this stream; valid until on\_close._ |
|  void | [**cft\_stream\_reset**](#function-cft_stream_reset) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s) <br>_Aborts the stream (HTTP/2 RST\_STREAM); on\_close follows._ |
|  int | [**cft\_stream\_respond**](#function-cft_stream_respond) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, int status, const char \* content\_type, const void \* body, size\_t len) <br>_Sends a complete response._ |
|  int | [**cft\_stream\_send\_headers**](#function-cft_stream_send_headers) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, int status, const [**cft\_header\_t**](#struct-cft_header_t)\* headers, size\_t num\_headers) <br>_Starts the response._ |
|  void | [**cft\_stream\_set\_handlers**](#function-cft_stream_set_handlers) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, const [**cft\_stream\_handlers\_t**](#struct-cft_stream_handlers_t)\* handlers, void \* ctx) <br>_Sets the callbacks of a stream._ |
|  int | [**cft\_stream\_write**](#function-cft_stream_write) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, const void \* data, size\_t len) <br>_Queues response body data._ |
|  void | [**cft\_tunnel\_free**](#function-cft_tunnel_free) ([**cft\_tunnel\_t**](#typedef-cft_tunnel_t)\* tunnel) <br>_Frees a tunnel that is not running._ |
|  [**cft\_tunnel\_t**](#typedef-cft_tunnel_t)\* | [**cft\_tunnel\_new**](#function-cft_tunnel_new) (const [**cft\_tunnel\_config\_t**](#struct-cft_tunnel_config_t)\* cfg, const [**cft\_transport\_ops\_t**](#struct-cft_transport_ops_t)\* transport) <br>_Creates a tunnel._ |
|  int | [**cft\_tunnel\_post**](#function-cft_tunnel_post) ([**cft\_tunnel\_t**](#typedef-cft_tunnel_t)\* tunnel, void(\*)(void \*arg) fn, void \* arg) <br>_Runs a function on the tunnel's thread; thread-safe._ |
|  void | [**cft\_tunnel\_request\_stop**](#function-cft_tunnel_request_stop) ([**cft\_tunnel\_t**](#typedef-cft_tunnel_t)\* tunnel, uint32\_t stop\_timeout\_ms) <br>_Asks_ [_**cft\_tunnel\_run()**_](#function-cft_tunnel_run) _to stop; thread-safe._ |
|  void | [**cft\_tunnel\_run**](#function-cft_tunnel_run) ([**cft\_tunnel\_t**](#typedef-cft_tunnel_t)\* tunnel) <br>_Runs the tunnel on the calling thread._ |
|  int | [**cft\_ws\_accept**](#function-cft_ws_accept) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, const char \* subprotocol) <br>_Accepts a WebSocket request (req-&gt;is\_websocket)._ |
|  int | [**cft\_ws\_close**](#function-cft_ws_close) ([**cft\_stream\_t**](#typedef-cft_stream_t)\* s, uint16\_t code, const char \* reason) <br>_Closes a WebSocket._ |
|  int | [**cft\_ws\_send**](#function-cft_ws_send)  <br>_Sends a WebSocket message._ |


## Structures and Types Documentation

### typedef `cft_event_fn`

_Called for connection events on the tunnel's thread._
```c
typedef void(*) cft_event_fn(void *arg, const cft_event_t *ev);
```

### struct `cft_event_t`

_Connection event._
Variables:

-  union cft\_event\_t::@326025004314023150341333157070040003002331170274 @105237216204216326377146047101312210377172263351  <br>Event data, by type

-  uint8\_t conn_index  <br>Which connection (0..3)

-   type  <br>Event type

### enum `cft_event_type_t`

_Connection events._
```c
enum cft_event_type_t {
    CFT_EVENT_REGISTERED,
    CFT_EVENT_REGISTRATION_FAILED,
    CFT_EVENT_CONFIG_UPDATED,
    CFT_EVENT_DRAINING,
    CFT_EVENT_UNREGISTERED,
    CFT_EVENT_CLOSED
};
```

### struct `cft_header_t`

_HTTP header._
Variables:

-  const char \* name  <br>Header name; lower-case in requests

-  const char \* value  <br>Header value

### typedef `cft_request_fn`

_Called when the headers of a new request have arrived._
```c
typedef void(*) cft_request_fn(void *arg, cft_stream_t *s, const cft_request_t *req);
```

Respond right away ([**cft\_stream\_respond()**](#function-cft_stream_respond)), read the body ([**cft\_stream\_read\_body()**](#function-cft_stream_read_body)), accept a WebSocket ([**cft\_ws\_accept()**](#function-cft_ws_accept)), or set handlers ([**cft\_stream\_set\_handlers()**](#function-cft_stream_set_handlers)) and respond later. A stream with no response stays open until the visitor gives up.
### struct `cft_request_t`

_Request, as received from the edge._
Variables:

-  const char \* authority  <br>Host the visitor used

-  int64\_t content_length  <br>Request body length, or -1 if unknown

-  const [**cft\_header\_t**](#struct-cft_header_t)\* headers  <br>Visitor's headers, plus Cloudflare's (cf-connecting-ip, ...)

-  bool is_websocket  <br>The visitor asks for a WebSocket; see [**cft\_ws\_accept()**](#function-cft_ws_accept)

-  const char \* method  <br>e.g. "GET"

-  size\_t num_headers  <br>Number of entries in headers

-  const char \* path  <br>Path and query, e.g. "/api?x=1"

-  const char \* scheme  <br>"https" or "http"

-  const char \* service  <br>Ingress service the request matched (e.g. "http://localhost:80"), or NULL

### struct `cft_stream_handlers_t`

_Per-stream callbacks, see_ [_**cft\_stream\_set\_handlers()**_](#function-cft_stream_set_handlers) _._
All members are optional.
Variables:

-  void(\*) on_body  <br>Part of the request body

-  void(\*) on_body_end  <br>The request body is complete (also called for requests without a body)

-  void(\*) on_close  <br>The stream is gone: the response was sent, or the visitor or the connection went away. The stream must not be used afterwards; free ctx here. completed is true if the whole response was sent.

-  void(\*) on_writable  <br>Everything written so far has been passed on; write more now

-  void(\*) on_ws_message  <br>A complete WebSocket text or binary message (after [**cft\_ws\_accept()**](#function-cft_ws_accept))

### typedef `cft_stream_t`

_One request and its response._
```c
typedef struct cft_stream cft_stream_t;
```

### struct `cft_transport_ops_t`

_TLS client over a non-blocking socket._
Implemented by each platform; called from the tunnel's thread.
Variables:

-  void(\*) close  <br>Closes the connection and frees the handle

-  int(\*) connect  <br>Advances the connection and handshake: 1 done, 0 in progress, -1 failed

-  void \* ctx  <br>Passed to [**open()**](#variable-open)

-  int(\*) get_fd  <br>Socket to wait on

-  size\_t(\*) local_ip  <br>Local address of the socket; returns its length (4 or 16) or 0

-  void \*(\*) open  <br>Starts connecting; returns a connection handle or NULL. May block for DNS.

-  size\_t(\*) pending  <br>Decrypted bytes buffered in the transport, readable without waiting

-  long(\*) read  <br>Returns &gt;0 bytes read, 0 if nothing is available now, &lt;0 if closed or failed

-  long(\*) write  <br>Returns &gt;=0 bytes written (0: try later), &lt;0 if failed. After a short write, pass the rest again.

### struct `cft_tunnel_config_t`

_Tunnel configuration._
Variables:

-  const char \* arch  <br>Reported to the edge; NULL for "unknown"

-  const uint8\_t \* connector_id  <br>16 bytes, or NULL for a random one. Keep it stable across restarts: the edge then replaces the previous run's connections right away.

-  const char \* edge_hosts  <br>Edge addresses, one per region; NULL for Cloudflare's

-  uint16\_t edge_port  <br>0 means 7844

-  void \* event_arg  <br>Passed to on\_event

-  uint32\_t max_concurrent_streams  <br>Open streams per connection, WebSockets included; more get 503. 0 means 16

-  uint8\_t max_origin_connections  <br>Open connections to other hosts; more get 503. 0 means 4

-  size\_t max_request_body  <br>Default limit of [**cft\_stream\_read\_body()**](#function-cft_stream_read_body)and of WebSocket messages; 0 means 16 KiB

-  uint8\_t num_connections  <br>1..4; 0 means 1

-   on_event  <br>Optional

-   on_request  <br>Called for each request

-  size\_t proxy_buffer_size  <br>Data buffered per proxied request in each direction, see [**cft\_stream\_proxy()**](#function-cft_stream_proxy). 0 means 16 KiB

-  void \* request_arg  <br>Passed to on\_request

-  uint32\_t stream_window  <br>HTTP/2 receive window per stream: how much request body the edge sends ahead. Bounds upload speed (window / round-trip time) and what a paused stream can still receive. 0 means 64 KiB

-  const char \* token  <br>Tunnel token; copied

-  bool use_ingress_rules  <br>Route requests by the public hostnames configured in the dashboard: services on localhost go to on\_request (if set), others to [**cft\_stream\_proxy()**](#function-cft_stream_proxy). Without it, or until the edge sends the rules, all requests go to on\_request.

-  const char \* version  <br>Reported to the edge; NULL for the component's

### typedef `cft_tunnel_t`

_Tunnel handle._
```c
typedef struct cft_tunnel cft_tunnel_t;
```

### enum `cft_ws_opcode_t`

_WebSocket message types._
```c
enum cft_ws_opcode_t {
    CFT_WS_TEXT = 1,
    CFT_WS_BINARY = 2,
    CFT_WS_CLOSE = 8,
    CFT_WS_PING = 9,
    CFT_WS_PONG = 10
};
```


## Functions Documentation

### function `cft_stream_buffered`

_Response bytes queued but not yet passed to the connection._
```c
size_t cft_stream_buffered (
    const cft_stream_t * s
) 
```

### function `cft_stream_end`

_Ends the response once the queued data has been sent._
```c
int cft_stream_end (
    cft_stream_t * s
) 
```

**Returns:**

0 on success, -1 on error
### function `cft_stream_header`

_Value of a request header._
```c
const char * cft_stream_header (
    const cft_stream_t * s,
    const char * name
) 
```

**Parameters:**


* `s` Stream 
* `name` Header name, lower-case 


**Returns:**

The value, or NULL if the header is absent
### function `cft_stream_pause_body`

_Stops or resumes the flow of request body data._
```c
void cft_stream_pause_body (
    cft_stream_t * s,
    bool pause
) 
```

While paused, the edge can still send what it was already allowed to (up to 64 KiB per stream), which on\_body receives as usual; then it waits. Use it when the body goes somewhere slower than the network, e.g. flash.



**Parameters:**


* `s` Stream 
* `pause` true to stop, false to resume
### function `cft_stream_proxy`

_Passes the request on to another host, as cloudflared does._
```c
int cft_stream_proxy (
    cft_stream_t * s,
    const char * service
) 
```

Services:
* [http://host](http://host)[:port], ws://host[:port]: HTTP/1.1 proxy, WebSockets included;
* ssh://, rdp://, smb://, tcp://host[:port]: TCP over WebSocket, the form `cloudflared access`uses on the visitor's side;
* http\_status:CODE: an empty response with that status.

Not supported: [https://](https://)and wss:// origins, unix sockets, and cloudflared's built-in services (hello\_world, bastion, socks5).

Each request gets its own connection to the origin. The tunnel then owns the stream; do not set handlers. If the origin cannot be reached, the visitor gets 502; over max\_origin\_connections, 503. Resolving a host name blocks the tunnel's thread; IP addresses do not.



**Parameters:**


* `s` Stream, from on\_request 
* `service` Service URL; copied 


**Returns:**

0 on success (a response follows), -1 if the service is not supported
### function `cft_stream_read_body`

_Collects the request body, then calls a function with it._
```c
int cft_stream_read_body (
    cft_stream_t * s,
    size_t max_len,
    void(*)( cft_stream_t *s, void *ctx, const uint8_t *body, size_t len) fn,
    void * ctx
) 
```

Replaces the on\_body and on\_body\_end handlers. Bodies over max\_len get a 413 response, and fn is not called.



**Parameters:**


* `s` Stream 
* `max_len` Largest accepted body; 0 for the tunnel's max\_request\_body 
* `fn` Called with the complete body (NULL and 0 if there is none) 
* `ctx` Passed to fn 


**Returns:**

0 on success, -1 if out of memory
### function `cft_stream_request`

_Request of this stream; valid until on\_close._
```c
const cft_request_t * cft_stream_request (
    const cft_stream_t * s
) 
```

### function `cft_stream_reset`

_Aborts the stream (HTTP/2 RST\_STREAM); on\_close follows._
```c
void cft_stream_reset (
    cft_stream_t * s
) 
```

### function `cft_stream_respond`

_Sends a complete response._
```c
int cft_stream_respond (
    cft_stream_t * s,
    int status,
    const char * content_type,
    const void * body,
    size_t len
) 
```

**Parameters:**


* `s` Stream 
* `status` HTTP status 
* `content_type` Content-Type, or NULL 
* `body` Body; copied 
* `len` Body length 


**Returns:**

0 on success, -1 on error
### function `cft_stream_send_headers`

_Starts the response._
```c
int cft_stream_send_headers (
    cft_stream_t * s,
    int status,
    const cft_header_t * headers,
    size_t num_headers
) 
```

Follow with [**cft\_stream\_write()**](#function-cft_stream_write)and[**cft\_stream\_end()**](#function-cft_stream_end). Include content-length if known; otherwise the edge sends the body chunked.



**Parameters:**


* `s` Stream 
* `status` HTTP status 
* `headers` Response headers; copied 
* `num_headers` Number of headers 


**Returns:**

0 on success, -1 on error (e.g. a response was already started)
### function `cft_stream_set_handlers`

_Sets the callbacks of a stream._
```c
void cft_stream_set_handlers (
    cft_stream_t * s,
    const cft_stream_handlers_t * handlers,
    void * ctx
) 
```

**Parameters:**


* `s` Stream 
* `handlers` Callbacks; copied 
* `ctx` Passed to the callbacks
### function `cft_stream_write`

_Queues response body data._
```c
int cft_stream_write (
    cft_stream_t * s,
    const void * data,
    size_t len
) 
```

The data is copied. To limit memory use, write more from on\_writable rather than all at once.



**Returns:**

0 on success, -1 on error
### function `cft_tunnel_free`

_Frees a tunnel that is not running._
```c
void cft_tunnel_free (
    cft_tunnel_t * tunnel
) 
```

### function `cft_tunnel_new`

_Creates a tunnel._
```c
cft_tunnel_t * cft_tunnel_new (
    const cft_tunnel_config_t * cfg,
    const cft_transport_ops_t * transport
) 
```

**Parameters:**


* `cfg` Configuration; copied, except for the strings it points to 
* `transport` Transport; copied 


**Returns:**

The tunnel, or NULL if the token is invalid or memory runs out
### function `cft_tunnel_post`

_Runs a function on the tunnel's thread; thread-safe._
```c
int cft_tunnel_post (
    cft_tunnel_t * tunnel,
    void(*)(void *arg) fn,
    void * arg
) 
```

Use it to answer requests or send WebSocket messages from other threads. The function runs within about 100 ms. A stream may close in the meantime: keep track of on\_close.



**Returns:**

0 on success, -1 if out of memory
### function `cft_tunnel_request_stop`

_Asks_ [_**cft\_tunnel\_run()**_](#function-cft_tunnel_run) _to stop; thread-safe._
```c
void cft_tunnel_request_stop (
    cft_tunnel_t * tunnel,
    uint32_t stop_timeout_ms
) 
```

**Parameters:**


* `tunnel` Tunnel 
* `stop_timeout_ms` How long to wait for the connections to unregister
### function `cft_tunnel_run`

_Runs the tunnel on the calling thread._
```c
void cft_tunnel_run (
    cft_tunnel_t * tunnel
) 
```

Returns after [**cft\_tunnel\_request\_stop()**](#function-cft_tunnel_request_stop), once the connections have unregistered or the stop timeout has passed.
### function `cft_ws_accept`

_Accepts a WebSocket request (req-&gt;is\_websocket)._
```c
int cft_ws_accept (
    cft_stream_t * s,
    const char * subprotocol
) 
```

Messages then arrive in on\_ws\_message. Pings are answered and the close handshake is handled; on\_close follows.



**Parameters:**


* `s` Stream 
* `subprotocol` Sec-WebSocket-Protocol to confirm, or NULL 


**Returns:**

0 on success, -1 if the request is not a valid WebSocket request
### function `cft_ws_close`

_Closes a WebSocket._
```c
int cft_ws_close (
    cft_stream_t * s,
    uint16_t code,
    const char * reason
) 
```

**Parameters:**


* `s` Stream 
* `code` Close status, e.g. 1000 
* `reason` Reason text, or NULL 


**Returns:**

0 on success, -1 on error
### function `cft_ws_send`

_Sends a WebSocket message._
```c
int cft_ws_send (
    cft_stream_t * s,
    cft_ws_opcode_t opcode,
    const void * data,
    size_t len
) 
```

**Parameters:**


* `s` Stream 
* `opcode` CFT\_WS\_TEXT, CFT\_WS\_BINARY, CFT\_WS\_PING or CFT\_WS\_PONG 
* `data` Payload; copied 
* `len` Payload length 


**Returns:**

0 on success, -1 on error


## File cf_tunnel_esp.h

## Structures and Types

| Type | Name |
| ---: | :--- |
| typedef struct cft\_tunnel\_esp \* | [**cft\_tunnel\_handle\_t**](#typedef-cft_tunnel_handle_t)  <br>_Handle of a tunnel started by_ [_**cft\_tunnel\_start()**_](#function-cft_tunnel_start) _._ |

## Functions

| Type | Name |
| ---: | :--- |
|  [**cft\_tunnel\_t**](#typedef-cft_tunnel_t)\* | [**cft\_tunnel\_get**](#function-cft_tunnel_get)  <br>_The tunnel run by the task, e.g. for_ [_**cft\_tunnel\_post()**_](#function-cft_tunnel_post) _._ |
|  esp\_err\_t | [**cft\_tunnel\_start**](#function-cft_tunnel_start) (const [**cft\_tunnel\_config\_t**](#struct-cft_tunnel_config_t)\* cfg, [**cft\_tunnel\_handle\_t**](#typedef-cft_tunnel_handle_t)\* out) <br>_Creates a tunnel and runs it in a new task._ |
|  esp\_err\_t | [**cft\_tunnel\_stop**](#function-cft_tunnel_stop)  <br>_Unregisters from the edge and stops the task._ |


## Structures and Types Documentation

### typedef `cft_tunnel_handle_t`

_Handle of a tunnel started by_ [_**cft\_tunnel\_start()**_](#function-cft_tunnel_start) _._
```c
typedef struct cft_tunnel_esp* cft_tunnel_handle_t;
```


## Functions Documentation

### function `cft_tunnel_get`

_The tunnel run by the task, e.g. for_ [_**cft\_tunnel\_post()**_](#function-cft_tunnel_post) _._
```c
cft_tunnel_t * cft_tunnel_get (
    cft_tunnel_handle_t tunnel
) 
```

**Parameters:**


* `tunnel` Handle from [**cft\_tunnel\_start()**](#function-cft_tunnel_start)


**Returns:**

The tunnel
### function `cft_tunnel_start`

_Creates a tunnel and runs it in a new task._
```c
esp_err_t cft_tunnel_start (
    const cft_tunnel_config_t * cfg,
    cft_tunnel_handle_t * out
) 
```

Defaults that differ from [**cft\_tunnel\_new()**](#function-cft_tunnel_new):
* connector\_id NULL: derived from the base MAC address and the tunnel ID, so the device keeps its connector ID across reboots;
* arch NULL: CONFIG\_IDF\_TARGET.

Task stack size and priority come from Kconfig (CONFIG\_CF\_TUNNEL\_TASK\_\*). Request handlers run in this task.



**Parameters:**


* `cfg` Configuration 
* `out` Handle of the running tunnel 


**Returns:**

ESP\_OK, ESP\_ERR\_INVALID\_ARG (e.g. invalid token) or ESP\_ERR\_NO\_MEM
### function `cft_tunnel_stop`

_Unregisters from the edge and stops the task._
```c
esp_err_t cft_tunnel_stop (
    cft_tunnel_handle_t tunnel,
    uint32_t timeout_ms
) 
```

**Parameters:**


* `tunnel` Handle from [**cft\_tunnel\_start()**](#function-cft_tunnel_start)
* `timeout_ms` How long to wait for the connections to unregister 


**Returns:**

ESP\_OK, or ESP\_ERR\_TIMEOUT if the task did not stop


