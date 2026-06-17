class_name McpProtocol
extends RefCounted

## JSON-RPC 2.0 / MCP protocol helpers shared by the server and its tests:
## protocol-version negotiation, message classification, and response
## envelopes. Pure static functions — no transport, no state.
##
## The server targets MCP "2025-06-18" (Streamable HTTP, single JSON response
## per POST, no SSE) and echoes "2025-03-26" when a client asks for it; any
## unknown version gets our latest and the client decides whether to proceed.

const PROTOCOL_LATEST := "2025-06-18"
const PROTOCOL_ACCEPTED: Array[String] = ["2025-06-18", "2025-03-26"]

const PARSE_ERROR := -32700
const INVALID_REQUEST := -32600
const METHOD_NOT_FOUND := -32601
const INVALID_PARAMS := -32602
const INTERNAL_ERROR := -32603
const REQUEST_CANCELLED := -32800

enum MessageKind { INVALID, REQUEST, NOTIFICATION, RESPONSE }


static func negotiate_version(requested: String) -> String:
	return requested if requested in PROTOCOL_ACCEPTED else PROTOCOL_LATEST


static func result_envelope(id: Variant, result: Variant) -> Dictionary:
	return { "jsonrpc": "2.0", "id": id, "result": result }


static func error_envelope(id: Variant, code: int, message: String, data: Variant = null) -> Dictionary:
	var error := { "code": code, "message": message }
	if data != null:
		error["data"] = data
	return { "jsonrpc": "2.0", "id": id, "error": error }


## JSON.parse gives every number back as a float; echoing 1.0 for a client's
## id 1 breaks strict clients. Fold integral floats back to ints; leave
## strings (and anything else) untouched.
static func normalize_id(id: Variant) -> Variant:
	if id is float and is_finite(id) and id == floorf(id):
		return int(id)
	return id


## Classify a decoded JSON-RPC message. A request carries method + id, a
## notification method only, a response result/error (+ id). Anything else —
## including a missing/wrong jsonrpc tag — is INVALID.
static func classify(message: Variant) -> MessageKind:
	if not (message is Dictionary):
		return MessageKind.INVALID
	if String(message.get("jsonrpc", "")) != "2.0":
		return MessageKind.INVALID
	var with_method: bool = message.has("method") and message.get("method") is String
	if with_method:
		return MessageKind.REQUEST if message.has("id") else MessageKind.NOTIFICATION
	if message.has("result") or message.has("error"):
		return MessageKind.RESPONSE
	return MessageKind.INVALID
