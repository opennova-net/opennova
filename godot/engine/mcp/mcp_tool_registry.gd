class_name McpToolRegistry
extends RefCounted

## The MCP server's curated tool table, registered by its owner service at boot.
## Owns the tools/list payload and the one place a tool handler actually runs.
##
## call_tool never directly awaits a handler bare: handlers run fire-and-forget
## into a shared state dict and call_tool polls it with a deadline. Script
## errors abort a handler but do resume the awaiter with null (4.6 behavior),
## so those surface immediately; the deadline covers handlers that genuinely
## cannot finish (awaiting a signal that never fires, runaway loops with
## awaits) so a stuck tool degrades to an error result instead of wedging the
## connection and the serialized tool queue.

# name -> { def: McpToolDef, handler: Callable, source: String, order: int }
var _tools := {}
var _order_counter := 0


## Register a tool. def: an McpToolDef (name, description, JSON-Schema args,
## serial/timeout policy). handler: func(args: Dictionary, ctx: McpToolContext),
## may be a coroutine; its return is wrapped via McpToolResult.json unless it
## already returns a McpToolResult. Re-registering the same name replaces the
## entry (keeping its list position); a non-builtin may not shadow a builtin.
func register(def: McpToolDef, handler: Callable, source := "builtin") -> Error:
	if def == null or def.name.is_empty() or not handler.is_valid():
		return ERR_INVALID_PARAMETER
	var name := def.name
	if _tools.has(name) and _tools[name]["source"] == "builtin" and source != "builtin":
		return ERR_ALREADY_EXISTS
	var order: int = _tools[name]["order"] if _tools.has(name) else _order_counter
	if not _tools.has(name):
		_order_counter += 1
	_tools[name] = { "def": def, "handler": handler, "source": source, "order": order }
	return OK


func has_tool(name: String) -> bool:
	return _tools.has(name)


## Whether a tool must run through the server's serialized FIFO queue (true
## for anything that touches editor state) or may run immediately alongside a
## queued job (read-only monitors like get_logs opt out with serial = false).
func is_serial(name: String) -> bool:
	if not _tools.has(name):
		return true
	return (_tools[name]["def"] as McpToolDef).serial


func source_of(name: String) -> String:
	return String(_tools[name]["source"]) if _tools.has(name) else ""


## The tools/list payload: builtins first, then any non-builtin owner
## registrations, each group in registration order.
func list_tools() -> Array:
	var entries := _tools.values()
	entries.sort_custom(func(a, b):
		if (a["source"] == "builtin") != (b["source"] == "builtin"):
			return a["source"] == "builtin"
		return int(a["order"]) < int(b["order"]))
	var out := []
	for entry in entries:
		out.append((entry["def"] as McpToolDef).to_list_entry())
	return out


## Run a tool to completion (coroutine; await it). Always returns a
## McpToolResult — unknown tools, handler errors, and timeouts come back as
## error results, never as a hang or an exception.
func call_tool(name: String, args: Dictionary, ctx: McpToolContext) -> McpToolResult:
	if not _tools.has(name):
		return McpToolResult.error("Unknown tool: %s. Call tools/list for the catalog." % name)
	var entry: Dictionary = _tools[name]
	var def: McpToolDef = entry["def"]
	var state := { "done": false, "value": null }
	_invoke(entry["handler"], args, ctx, state)
	if not state["done"]:
		var deadline := Time.get_ticks_msec() + def.timeout_ms
		var scene_tree := ctx.main_tree()
		while not state["done"] and scene_tree != null and Time.get_ticks_msec() < deadline:
			await scene_tree.process_frame
	if not state["done"]:
		ctx.cancelled = true
		return McpToolResult.error(
			"Tool '%s' did not finish within its %d ms budget — it timed out or hit a script error mid-await. Check get_logs." % [name, def.timeout_ms])
	var value: Variant = state["value"]
	var result: McpToolResult = value if value is McpToolResult else McpToolResult.json(value)
	for attachment in ctx.images:
		_attach_image(result, attachment)
	return result


# Fire-and-forget bridge: runs the handler (awaiting it covers both plain and
# coroutine handlers) and flips the shared state when — if — it completes.
func _invoke(handler: Callable, args: Dictionary, ctx: McpToolContext, state: Dictionary) -> void:
	state["value"] = await handler.call(args, ctx)
	state["done"] = true


# Encode a ctx.image() attachment (Image, or anything with get_image()) as a
# PNG content block; non-images are ignored with a note in the result.
func _attach_image(result: McpToolResult, attachment: Variant) -> void:
	var img: Image = null
	if attachment is Image:
		img = attachment
	elif attachment != null and is_instance_valid(attachment) and attachment.has_method("get_image"):
		img = attachment.get_image()
	if img == null or img.is_empty():
		result.add_text("(ctx.image attachment was not a usable Image — pass an Image or Texture2D)")
		return
	var bytes := img.save_png_to_buffer()
	if bytes.is_empty():
		result.add_text("(ctx.image attachment failed to encode as PNG)")
		return
	result.add_image(bytes, "image/png")
