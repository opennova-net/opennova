class_name ProbeDef
extends RefCounted

## One registered runtime probe (docs/mcp.md, ADR 0041): the name and
## description game_probe op=list shows, the JSON Schema of its typed
## arguments, the script that implements it under res://probes/, and the
## preconditions the runner checks before starting it. Typed record per
## ADR 0017; the schema is the wire's Dictionary shape by design.

const DEFAULT_TIMEOUT_MS := 600_000

var name := ""
var description := ""
## The GameProbe script, always under res://probes/ (never exported: a
## shipped build lists the probe as unavailable).
var script_path := ""
## JSON Schema (the ProbeSchema subset) for the probe's `args` object.
var input_schema: Dictionary = { "type": "object", "properties": {} }
## Refused under a headless DisplayServer.
var needs_window := false
## Refused unless a mission world is loaded.
var needs_mission := false
## The runner's watchdog budget: past it the run is cancelled.
var timeout_ms := DEFAULT_TIMEOUT_MS


static func make(p_name: String, p_description: String, p_script_path: String,
		properties: Dictionary = {}, required: Array = [], p_needs_window := false,
		p_needs_mission := false, p_timeout_ms := DEFAULT_TIMEOUT_MS) -> ProbeDef:
	var def := ProbeDef.new()
	def.name = p_name
	def.description = p_description
	def.script_path = p_script_path
	var schema := { "type": "object", "properties": properties }
	if not required.is_empty():
		schema["required"] = required
	def.input_schema = schema
	def.needs_window = p_needs_window
	def.needs_mission = p_needs_mission
	def.timeout_ms = p_timeout_ms
	return def


## The game_probe op=list wire entry (transport edge only).
func to_list_entry(available: bool) -> Dictionary:
	return {
		"name": name,
		"description": description,
		"input_schema": input_schema,
		"needs_window": needs_window,
		"needs_mission": needs_mission,
		"timeout_ms": timeout_ms,
		"available": available,
	}
