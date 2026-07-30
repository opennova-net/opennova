class_name McpToolResult
extends RefCounted

## The result of one MCP tool call, as the content blocks the protocol sends
## back: text, images (base64 + mime), and an optional structuredContent
## object. Tool handlers either build one of these directly (screenshots,
## errors with guidance) or return any Variant and let the registry wrap it
## via json().

var content: Array = []
var is_error := false
var structured: Variant = null


static func text(message: String) -> McpToolResult:
	var result := McpToolResult.new()
	result.add_text(message)
	return result


## Sanitize any Variant into a pretty-printed JSON text block; dictionaries
## also ride along as structuredContent for clients that consume it.
static func json(value: Variant) -> McpToolResult:
	var result := McpToolResult.new()
	var safe: Variant = McpJson.sanitize(value)
	result.add_text(JSON.stringify(safe, "\t"))
	if safe is Dictionary:
		result.structured = safe
	return result


static func image(bytes: PackedByteArray, mime: String, caption := "") -> McpToolResult:
	var result := McpToolResult.new()
	result.add_image(bytes, mime)
	if not caption.is_empty():
		result.add_text(caption)
	return result


## A failed tool call. Per MCP, execution failures are result.isError = true
## (so the model sees them), not JSON-RPC protocol errors. Keep messages
## actionable: name the precondition and the tool that fixes it.
static func error(message: String, details: Variant = null) -> McpToolResult:
	var result := McpToolResult.new()
	result.is_error = true
	result.add_text(message)
	if details != null:
		result.add_text(JSON.stringify(McpJson.sanitize(details), "\t"))
	return result


## Rehydrate a tools/call result received from another MCP endpoint. The
## editor's game-session proxy uses this to forward text, image, error and
## structured blocks without decoding/re-encoding them or flattening the
## child's result into JSON text.
static func from_payload(payload: Variant) -> McpToolResult:
	if not (payload is Dictionary):
		return error("The remote MCP tool returned an invalid result payload.")
	var result := McpToolResult.new()
	var blocks: Variant = payload.get("content", [])
	if blocks is Array:
		for block in blocks:
			if block is Dictionary:
				result.content.append((block as Dictionary).duplicate(true))
	result.is_error = bool(payload.get("isError", false))
	if payload.has("structuredContent"):
		result.structured = payload["structuredContent"]
	if result.content.is_empty() and result.structured == null:
		result.add_text("(The remote MCP tool returned no content.)")
	return result


func add_text(message: String) -> void:
	content.append({ "type": "text", "text": message })


func add_image(bytes: PackedByteArray, mime: String) -> void:
	content.append({ "type": "image", "data": Marshalls.raw_to_base64(bytes), "mimeType": mime })


## The tools/call result payload ({ content, isError [, structuredContent] }).
func to_payload() -> Dictionary:
	var payload := { "content": content, "isError": is_error }
	if structured != null:
		payload["structuredContent"] = structured
	return payload
