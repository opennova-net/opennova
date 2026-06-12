class_name McpDynamicTools
extends RefCounted

## Agent-defined MCP tools: define_tool turns a GDScript snippet into a named,
## persistent tool. Each tool is one generated .gd under user://oned_mcp/tools/
## (safe to hand-edit or delete) carrying its metadata as constants; boot
## reloads every file so tools agents craft survive editor restarts. A file
## that no longer compiles is reported and skipped, never fatal.
##
## Tool code runs through McpScriptRunner with the same ctx as execute_script,
## as `func run(ctx, args)` — args being the JSON arguments of the call. The
## advertised input_schema is documentation for the calling agent; it is not
## enforced server-side.

const TOOLS_DIR := "user://oned_mcp/tools"
const NAME_PATTERN := "^[a-z][a-z0-9_]{2,47}$"
const RUN_TIMEOUT_MS := 60000

var registry: McpToolRegistry

# name -> { description, path, loaded_ok, calls }
var _meta := {}


func _init(tool_registry: McpToolRegistry) -> void:
	registry = tool_registry


## Load every persisted tool file and register the ones that compile.
## Returns { loaded: PackedStringArray, failed: [{ file, errors }] }.
func load_all() -> Dictionary:
	var loaded := PackedStringArray()
	var failed: Array = []
	var dir := ProjectSettings.globalize_path(TOOLS_DIR)
	if not DirAccess.dir_exists_absolute(dir):
		return { "loaded": loaded, "failed": failed }
	for file in DirAccess.get_files_at(dir):
		if not file.ends_with(".gd"):
			continue
		var path := dir.path_join(file)
		var outcome := _load_file(path)
		if outcome["ok"]:
			loaded.append(String(outcome["name"]))
		else:
			failed.append({ "file": file, "errors": outcome["errors"] })
	return { "loaded": loaded, "failed": failed }


## Create (or with overwrite=true, replace) a tool: validate, compile FIRST —
## nothing persists on a compile error — then write the file and register it
## live. Returns { ok, path? , errors? }.
func define(name: String, description: String, input_schema: Dictionary, code: String, overwrite := false) -> Dictionary:
	if RegEx.create_from_string(NAME_PATTERN).search(name) == null:
		return { "ok": false, "errors": ["Tool name must match %s (lowercase snake_case, 3..48 chars)." % NAME_PATTERN] }
	if registry.has_tool(name) and registry.source_of(name) == "builtin":
		return { "ok": false, "errors": ["'%s' is a built-in tool name; pick another." % name] }
	if registry.has_tool(name) and not overwrite:
		return { "ok": false, "errors": ["Custom tool '%s' already exists — pass overwrite: true to replace it." % name] }
	if description.strip_edges().is_empty():
		return { "ok": false, "errors": ["A description is required — it is the prompt text agents see in tools/list."] }
	var schema := input_schema if not input_schema.is_empty() else { "type": "object" }
	var source := _generate_source(name, description, schema, code)
	var compiled := McpScriptRunner.compile(source)
	if not compiled["ok"]:
		return { "ok": false, "errors": compiled["compile_errors"] }
	var dir := ProjectSettings.globalize_path(TOOLS_DIR)
	DirAccess.make_dir_recursive_absolute(dir)
	var path := dir.path_join("%s.gd" % name)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return { "ok": false, "errors": ["Could not write %s (%s)." % [path, error_string(FileAccess.get_open_error())]] }
	file.store_string(source)
	file.close()
	_register(name, description, schema, compiled["script"], path)
	return { "ok": true, "path": path }


## Unregister and delete a custom tool. ERR_DOES_NOT_EXIST for unknown names;
## built-ins are untouchable.
func delete(name: String) -> Error:
	if not _meta.has(name):
		return ERR_DOES_NOT_EXIST
	if registry.source_of(name) == "custom":
		registry.unregister(name)
	var path := String(_meta[name]["path"])
	_meta.erase(name)
	if FileAccess.file_exists(path):
		return DirAccess.remove_absolute(path)
	return OK


## Metadata for every known custom tool (loaded or failed), for the
## list_custom_tools tool.
func list() -> Array:
	var out: Array = []
	var names := _meta.keys()
	names.sort()
	for name in names:
		var meta: Dictionary = _meta[name]
		out.append({
			"name": name,
			"description": meta["description"],
			"path": meta["path"],
			"loaded_ok": meta["loaded_ok"],
			"calls": meta["calls"],
		})
	return out


func _load_file(path: String) -> Dictionary:
	var source := FileAccess.get_file_as_string(path)
	if source.is_empty():
		return { "ok": false, "errors": ["Empty or unreadable file."] }
	var compiled := McpScriptRunner.compile(source)
	if not compiled["ok"]:
		return { "ok": false, "errors": compiled["compile_errors"] }
	var script: GDScript = compiled["script"]
	var constants: Dictionary = script.get_script_constant_map()
	var name := String(constants.get("TOOL_NAME", ""))
	if RegEx.create_from_string(NAME_PATTERN).search(name) == null:
		return { "ok": false, "errors": ["Missing or invalid TOOL_NAME constant."] }
	if registry.has_tool(name) and registry.source_of(name) == "builtin":
		return { "ok": false, "errors": ["TOOL_NAME '%s' collides with a built-in tool." % name] }
	var description := String(constants.get("TOOL_DESCRIPTION", ""))
	var schema_json := JSON.new()
	var schema := { "type": "object" }
	if schema_json.parse(String(constants.get("TOOL_SCHEMA_JSON", ""))) == OK and schema_json.data is Dictionary:
		schema = schema_json.data
	_register(name, description, schema, script, path)
	return { "ok": true, "name": name }


func _register(name: String, description: String, schema: Dictionary, script: GDScript, path: String) -> void:
	registry.register({
		"name": name,
		"description": description,
		"input_schema": schema,
		"timeout_ms": RUN_TIMEOUT_MS + 5000,
	}, Callable(self, "_run_custom").bind(name, script), "custom")
	_meta[name] = { "description": description, "path": path, "loaded_ok": true, "calls": 0 }


func _run_custom(args: Dictionary, ctx: McpToolContext, tool_name: String, script: GDScript) -> McpToolResult:
	if _meta.has(tool_name):
		_meta[tool_name]["calls"] = int(_meta[tool_name]["calls"]) + 1
	var outcome: Dictionary = await McpScriptRunner.execute(script, ctx, RUN_TIMEOUT_MS, [args])
	return McpScriptRunner.result_from_outcome(outcome)


# The persisted tool file. Metadata rides as constants the loader reflects
# back out; description and schema are embedded as JSON-escaped literals so
# any content (quotes, newlines, unicode) round-trips.
static func _generate_source(name: String, description: String, schema: Dictionary, code: String) -> String:
	var body: String
	if McpScriptRunner._defines_top_level_run(code):
		body = code
	else:
		body = "func run(ctx, args):\n" + McpScriptRunner._indent_body(code)
	return "\n".join(PackedStringArray([
		"# ONED MCP custom tool — generated by define_tool. Safe to delete; reloaded at boot.",
		"extends RefCounted",
		McpScriptRunner.RELAXED_WARNINGS,
		"",
		"const TOOL_NAME := %s" % JSON.stringify(name),
		"const TOOL_DESCRIPTION := %s" % JSON.stringify(description),
		"const TOOL_SCHEMA_JSON := %s" % JSON.stringify(JSON.stringify(schema)),
		"",
		"",
		body,
		"",
	]))
