class_name McpToolDef
extends RefCounted
## One MCP tool definition: the name/description the agent sees, the JSON
## Schema of its arguments, and the run policy (FIFO serialization, watchdog
## budget). Typed record per ADR 0017 — it replaces the {name, description,
## input_schema, serial?, timeout_ms?} def Dictionaries, and its one make()
## factory replaces the four identical _def helpers the editor tool catalogs
## carried. The tools/list wire payload is produced by to_list_entry(); the
## args a handler receives stay a Dictionary because they ARE the wire.

const DEFAULT_TIMEOUT_MS := 60000

var name := ""
var description := ""
## JSON Schema for the tool's arguments — MCP wire format, kept as the wire's
## Dictionary shape by design (transport edge).
var input_schema: Dictionary = { "type": "object" }
## Optional display title for tools/list; "" = omitted from the wire.
var title := ""
## True (default): runs through the server's serialized FIFO queue — anything
## that touches editor state. Read-only monitors (get_logs) opt out.
var serial := true
## call_tool watchdog budget for this tool.
var timeout_ms := DEFAULT_TIMEOUT_MS


static func make(p_name: String, p_description: String, properties: Dictionary = {},
		required: Array = [], p_serial := true, p_timeout_ms := DEFAULT_TIMEOUT_MS) -> McpToolDef:
	var def := McpToolDef.new()
	def.name = p_name
	def.description = p_description
	var schema := { "type": "object", "properties": properties }
	if not required.is_empty():
		schema["required"] = required
	def.input_schema = schema
	def.serial = p_serial
	def.timeout_ms = p_timeout_ms
	return def


## The tools/list wire entry (transport edge only).
func to_list_entry() -> Dictionary:
	var out := {
		"name": name,
		"description": description,
		"inputSchema": input_schema,
	}
	if not title.is_empty():
		out["title"] = title
	return out
