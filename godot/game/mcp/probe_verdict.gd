class_name ProbeVerdict
extends RefCounted

## What a probe run concluded: a pass/fail bit, a one-line summary, and the
## measured data the caller reads back (docs/mcp.md, ADR 0041). Typed record
## per ADR 0017; `data` is the JSON-facing payload and stays a Dictionary.

var ok := false
var summary := ""
var data: Dictionary = {}


static func passed(p_summary: String, p_data: Dictionary = {}) -> ProbeVerdict:
	var verdict := ProbeVerdict.new()
	verdict.ok = true
	verdict.summary = p_summary
	verdict.data = p_data
	return verdict


static func failed(p_summary: String, p_data: Dictionary = {}) -> ProbeVerdict:
	var verdict := ProbeVerdict.new()
	verdict.ok = false
	verdict.summary = p_summary
	verdict.data = p_data
	return verdict


## The game_probe status wire shape (transport edge only).
func to_json_value() -> Dictionary:
	return { "ok": ok, "summary": summary, "data": McpJson.sanitize(data) }
