class_name McpHttpConnection
extends RefCounted

## One accepted MCP client socket: pumps bytes into its McpHttpParser, hands
## complete requests to the server, and writes responses. While `busy` (a
## request's tool job is in flight) the connection buffers but does not parse,
## so pipelined requests wait their turn — one in-flight request per
## connection.
##
## Writes use put_data (write-it-all semantics). Peers are loopback and
## responses are bounded by the server's payload budget, so a blocking write is
## acceptable; revisit if the perf pane ever shows it.

const IDLE_TIMEOUT_MS := 120000
const MID_REQUEST_TIMEOUT_MS := 10000
const READ_CHUNK := 65536

const _STATUS_TEXT := {
	200: "OK", 202: "Accepted", 400: "Bad Request", 403: "Forbidden",
	404: "Not Found", 405: "Method Not Allowed", 411: "Length Required",
	413: "Content Too Large", 431: "Request Header Fields Too Large",
	500: "Internal Server Error", 503: "Service Unavailable",
}

var peer: StreamPeerTCP = null
var parser := McpHttpParser.new()
var busy := false

var _last_activity_ms := 0
var _mid_request := false


func setup(socket: StreamPeerTCP, now_ms: int) -> void:
	peer = socket
	peer.set_no_delay(true)
	_last_activity_ms = now_ms


## Pump the socket and return the next complete request, or {} (also {} while
## busy or after a parser error — the server checks parser_error_status()).
func poll(now_ms: int) -> Dictionary:
	if peer == null:
		return {}
	peer.poll()
	if peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
		return {}
	var available := peer.get_available_bytes()
	while available > 0:
		var chunk := peer.get_partial_data(mini(available, READ_CHUNK))
		if int(chunk[0]) != OK:
			break
		var bytes: PackedByteArray = chunk[1]
		if bytes.is_empty():
			break
		parser.push(bytes)
		_mid_request = true
		_last_activity_ms = now_ms
		available = peer.get_available_bytes()
	if busy or parser.has_error():
		return {}
	var request := parser.next_request()
	if not request.is_empty():
		_mid_request = false
		_last_activity_ms = now_ms
	return request


func parser_error_status() -> int:
	return parser.error_status()


func is_alive() -> bool:
	return peer != null and peer.get_status() == StreamPeerTCP.STATUS_CONNECTED


## Idle pruning: a connection mid-request (bytes but no complete head/body)
## gets a short leash; an idle keep-alive connection a long one. Never prunes
## while a tool job owns the connection.
func idle_too_long(now_ms: int) -> bool:
	if busy:
		return false
	var limit := MID_REQUEST_TIMEOUT_MS if _mid_request else IDLE_TIMEOUT_MS
	return now_ms - _last_activity_ms > limit


func respond_json(status: int, payload: Variant, extra_headers := {}, keep_alive := true) -> void:
	var headers := { "Content-Type": "application/json" }
	headers.merge(extra_headers)
	respond(status, headers, JSON.stringify(payload).to_utf8_buffer(), keep_alive)


func respond_empty(status: int, extra_headers := {}, keep_alive := true) -> void:
	respond(status, extra_headers, PackedByteArray(), keep_alive)


func respond(status: int, headers: Dictionary, body: PackedByteArray, keep_alive := true) -> void:
	busy = false
	if peer == null:
		return
	peer.poll()
	if peer.get_status() != StreamPeerTCP.STATUS_CONNECTED:
		close()
		return
	var text := "HTTP/1.1 %d %s\r\n" % [status, _STATUS_TEXT.get(status, "Status")]
	for name in headers:
		text += "%s: %s\r\n" % [name, headers[name]]
	text += "Content-Length: %d\r\n" % body.size()
	text += "Connection: %s\r\n\r\n" % ("keep-alive" if keep_alive else "close")
	var bytes := text.to_ascii_buffer()
	bytes.append_array(body)
	peer.put_data(bytes)
	if not keep_alive:
		close()
	_last_activity_ms = Time.get_ticks_msec()


func close() -> void:
	if peer != null:
		peer.disconnect_from_host()
		peer = null
