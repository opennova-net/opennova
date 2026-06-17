class_name McpHttpParser
extends RefCounted

## Incremental HTTP/1.1 request parser for the embedded MCP server. Socket-free
## by design: the connection layer push()es raw bytes as they arrive and asks
## next_request() for complete requests, which makes the parser the unit-test
## surface (chunk boundaries, pipelining, malformed input) without any network.
##
## Deliberately minimal: Content-Length framing only (Transfer-Encoding is
## rejected with 411), no multipart, no continuation (obs-fold) headers. Peers
## are loopback MCP clients posting small JSON-RPC bodies; hard caps keep a
## misbehaving client from growing the buffer unboundedly.

const MAX_HEAD_BYTES := 16384
const MAX_BODY_BYTES := 4 * 1024 * 1024

var _buffer := PackedByteArray()
var _scan_from := 0
var _error_status := 0


## Append raw socket bytes. No-op once the parser is in an error state (the
## connection is about to be closed anyway).
func push(bytes: PackedByteArray) -> void:
	if _error_status != 0:
		return
	_buffer.append_array(bytes)


func has_error() -> bool:
	return _error_status != 0


## The HTTP status the connection should respond with before closing:
## 400 malformed, 411 unsupported framing (chunked), 413 body too large,
## 431 head too large. 0 when no error.
func error_status() -> int:
	return _error_status


## The next complete request, or {} while incomplete (or errored — check
## has_error()). Consumes the request's bytes, so pipelined requests come out
## one call at a time. Shape: { method, target, http_version, headers (lower-
## cased keys, last wins), body: PackedByteArray, keep_alive, expects_continue }.
func next_request() -> Dictionary:
	if _error_status != 0:
		return {}
	_skip_leading_crlf()
	var head_end := _find_head_end()
	if head_end < 0:
		if _buffer.size() > MAX_HEAD_BYTES:
			_error_status = 431
		return {}
	if head_end > MAX_HEAD_BYTES:
		_error_status = 431
		return {}
	var head := _parse_head(_buffer.slice(0, head_end).get_string_from_ascii())
	if _error_status != 0:
		return {}
	var content_length: int = head["content_length"]
	var total := head_end + 4 + content_length
	if _buffer.size() < total:
		return {}
	var body := _buffer.slice(head_end + 4, total)
	_buffer = _buffer.slice(total)
	_scan_from = 0
	return {
		"method": head["method"],
		"target": head["target"],
		"http_version": head["http_version"],
		"headers": head["headers"],
		"body": body,
		"keep_alive": head["keep_alive"],
		"expects_continue": head["expects_continue"],
	}


# RFC 9112 allows (and recommends tolerating) CRLFs before the request line.
func _skip_leading_crlf() -> void:
	var start := 0
	while start + 1 < _buffer.size() and _buffer[start] == 13 and _buffer[start + 1] == 10:
		start += 2
	if start > 0:
		_buffer = _buffer.slice(start)
		_scan_from = 0


# Index of the first "\r\n\r\n" terminator, or -1. Resumes scanning where the
# last call left off so repeated polls on a slow body stay linear.
func _find_head_end() -> int:
	var from := _scan_from
	var i := _buffer.find(13, from)
	while i >= 0 and i + 3 < _buffer.size():
		if _buffer[i + 1] == 10 and _buffer[i + 2] == 13 and _buffer[i + 3] == 10:
			return i
		i = _buffer.find(13, i + 1)
	_scan_from = maxi(_buffer.size() - 3, 0)
	return -1


# Parse request line + headers; sets _error_status on malformed input and
# returns the parsed fields otherwise.
func _parse_head(text: String) -> Dictionary:
	var lines := text.split("\r\n")
	var parts := lines[0].split(" ")
	if parts.size() != 3 or parts[0].is_empty() or parts[1].is_empty() or not parts[2].begins_with("HTTP/"):
		_error_status = 400
		return {}
	var headers := {}
	for i in range(1, lines.size()):
		var line := lines[i]
		if line.is_empty():
			continue
		if line[0] == " " or line[0] == "\t":
			# obs-fold continuation lines are obsolete and ambiguous; reject.
			_error_status = 400
			return {}
		var colon := line.find(":")
		if colon <= 0:
			_error_status = 400
			return {}
		var name := line.substr(0, colon).strip_edges().to_lower()
		if name.is_empty() or name.contains(" "):
			_error_status = 400
			return {}
		headers[name] = line.substr(colon + 1).strip_edges()
	if headers.has("transfer-encoding"):
		_error_status = 411
		return {}
	var content_length := 0
	if headers.has("content-length"):
		var raw := String(headers["content-length"])
		if not raw.is_valid_int() or raw.to_int() < 0:
			_error_status = 400
			return {}
		content_length = raw.to_int()
		if content_length > MAX_BODY_BYTES:
			_error_status = 413
			return {}
	var version := parts[2]
	var connection := String(headers.get("connection", "")).to_lower()
	var keep_alive := connection != "close" if version == "HTTP/1.1" else connection == "keep-alive"
	return {
		"method": parts[0],
		"target": parts[1],
		"http_version": version,
		"headers": headers,
		"content_length": content_length,
		"keep_alive": keep_alive,
		"expects_continue": String(headers.get("expect", "")).to_lower() == "100-continue",
	}
