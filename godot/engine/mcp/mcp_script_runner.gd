class_name McpScriptRunner
extends RefCounted

## Compiles and runs agent-supplied GDScript inside the live editor — the
## execute_script tool and the body of every agent-defined tool.
##
## Compile: the agent sends either a bare body (wrapped into
## `func run(ctx):`), a class body that already defines `func run`, or a full
## script starting with `extends`. Parse errors are recovered from the engine
## log delta around reload() (GDScript prints them; it does not return them).
##
## Execute: run(ctx) is fired into a shared state dict and polled with a
## deadline rather than awaited directly. Script errors — before or after an
## await — abort run() but resume the awaiter with null (verified on 4.6), so
## they return promptly with the error text recovered from the engine log;
## the watchdog deadline covers what cannot resume at all: awaiting a signal
## that never fires, or deliberately long-running code. Hard limit, stated
## plainly: a tight loop that never awaits blocks the main thread and nothing
## can preempt it — the watchdog only runs between frames.

const DEFAULT_TIMEOUT_MS := 10000
const MIN_TIMEOUT_MS := 100
const MAX_TIMEOUT_MS := 300000

## Agent snippets are exploratory one-offs talking to duck-typed editor
## surfaces; relax the noisy WARN-level lints inside generated wrappers so
## they don't clutter the log. (inference_on_variant is ERROR-level and
## @warning_ignore cannot silence escalated warnings — that one is handled by
## the auto-typing retry in compile() instead.)
const RELAXED_WARNINGS := "@warning_ignore_start(\"unused_parameter\", \"unused_variable\", \"shadowed_variable\", \"return_value_discarded\")"
const COMPILE_RETRIES := 5


## Compile agent code into a script exposing run(...). Returns
## { ok, script?, compile_errors: Array[String], line_offset } — line_offset
## is how many generated lines precede the agent's first line.
##
## The natural agent idiom `var x := ctx.camera()` trips this project's
## inference_on_variant ERROR, and @warning_ignore cannot silence escalated
## warnings. So compile retries: when the parser flags "Cannot infer the
## type" on a line, that declaration is mechanically rewritten to
## `var x: Variant = ...` and the script recompiles — semantics-preserving
## (Variant is what an untyped declaration would be) and only touches lines
## the compiler named.
static func compile(code: String) -> Dictionary:
	var source: String
	var offset := 0
	if _is_full_script(code):
		source = code
	elif _defines_top_level_run(code):
		source = "extends RefCounted\n%s\n" % RELAXED_WARNINGS + code
		offset = 2
	else:
		source = "extends RefCounted\n%s\nfunc run(ctx):\n" % RELAXED_WARNINGS + _indent_body(code)
		offset = 3
	var hub := McpLogHub.instance
	var diagnostics: Array = []
	for attempt in range(COMPILE_RETRIES):
		var script := GDScript.new()
		script.source_code = source
		var mark := hub.engine_mark() if hub != null else -1
		var err := script.reload()
		if err == OK and script.can_instantiate():
			var probe: Variant = script.new()
			if probe == null or not probe.has_method("run"):
				return {
					"ok": false,
					"compile_errors": ["The script must define `func run(ctx):` (or `func run(ctx, args):` for a custom tool)."],
					"line_offset": offset,
				}
			return { "ok": true, "script": script, "compile_errors": [], "line_offset": offset }
		diagnostics = _collect_errors(hub, mark, offset)
		var retyped := _auto_type_inference_failures(source, diagnostics)
		if retyped == source:
			break
		source = retyped
	for i in range(diagnostics.size()):
		if String(diagnostics[i]).contains("Cannot infer the type"):
			diagnostics.append("Hint: this project escalates inference-from-Variant to an error — declare such variables explicitly: `var x: Variant = ...`.")
			break
	return { "ok": false, "compile_errors": diagnostics, "line_offset": offset }


## Run a compiled script's run() to completion under a watchdog. `extra_args`
## is appended to the run() call after ctx (custom tools pass their args).
## Returns { ok, result, logs, errors, duration_ms, timed_out, error_capture }.
static func execute(script: GDScript, ctx: McpToolContext, timeout_ms := DEFAULT_TIMEOUT_MS, extra_args: Array = []) -> Dictionary:
	var instance: Variant = script.new()
	if instance == null or not instance.has_method("run"):
		return _result(false, null, ctx, ["The script must define `func run(ctx):`."], 0, false, "none")
	var hub := McpLogHub.instance
	var mark := hub.engine_mark() if hub != null else -1
	var start := Time.get_ticks_msec()
	var state := { "done": false, "value": null }
	_invoke(instance, ctx, extra_args, state)
	if not state["done"]:
		var deadline := start + clampi(timeout_ms, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS)
		var tree := ctx.main_tree()
		while not state["done"] and tree != null and Time.get_ticks_msec() < deadline:
			await tree.process_frame
	var duration := Time.get_ticks_msec() - start
	var timed_out: bool = not state["done"]
	if timed_out:
		ctx.cancelled = true
	var errors: Array = []
	var capture := "unavailable"
	if hub != null and mark >= 0:
		capture = "engine_log"
		for block: Dictionary in hub.engine_delta(mark):
			if String(block["level"]) == "error":
				errors.append(String(block["text"]))
	if timed_out:
		errors.append("run(ctx) did not finish within %d ms — it timed out, or a script error after an await killed it (see any errors above)." % clampi(timeout_ms, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS))
	var ok: bool = not timed_out and errors.is_empty()
	return _result(ok, McpJson.sanitize(state["value"]), ctx, errors, duration, timed_out, capture)


## Shape an execute() outcome as the McpToolResult both execute_script and
## agent-defined tools return: one JSON payload, is_error when the run failed.
static func result_from_outcome(outcome: Dictionary) -> McpToolResult:
	var payload := {
		"ok": outcome["ok"],
		"result": outcome["result"],
		"logs": outcome["logs"],
		"duration_ms": outcome["duration_ms"],
	}
	if not (outcome["errors"] as Array).is_empty():
		payload["errors"] = outcome["errors"]
	if outcome["timed_out"]:
		payload["timed_out"] = true
	if String(outcome["error_capture"]) != "engine_log":
		payload["error_capture"] = outcome["error_capture"]
	var result := McpToolResult.json(payload)
	result.is_error = not bool(outcome["ok"])
	return result


static func _result(ok: bool, value: Variant, ctx: McpToolContext, errors: Array, duration_ms: int, timed_out: bool, capture: String) -> Dictionary:
	return {
		"ok": ok,
		"result": value,
		"logs": ctx.logs.duplicate(),
		"errors": errors,
		"duration_ms": duration_ms,
		"timed_out": timed_out,
		"error_capture": capture,
	}


# Fire-and-forget bridge; see the class doc for why execute never awaits this.
static func _invoke(instance: Variant, ctx: McpToolContext, extra_args: Array, state: Dictionary) -> void:
	var call_args: Array = [ctx]
	call_args.append_array(extra_args)
	state["value"] = await instance.callv("run", call_args)
	state["done"] = true


# A full script: the first meaningful (non-comment) line declares extends or
# class_name. Generated custom-tool files start with a comment header, so this
# looks past comments rather than at character zero.
static func _is_full_script(code: String) -> bool:
	for line in code.split("\n"):
		var meaningful := line.strip_edges()
		if meaningful.is_empty() or meaningful.begins_with("#"):
			continue
		return meaningful.begins_with("extends ") or meaningful.begins_with("class_name ")
	return false


# A top-level (column zero) `func run(` or `static func run(` definition.
static func _defines_top_level_run(code: String) -> bool:
	for line in code.split("\n"):
		if line.begins_with("func run(") or line.begins_with("static func run("):
			return true
	return false


static func _indent_body(code: String) -> String:
	var out := ""
	for line in code.split("\n"):
		out += "\t" + line + "\n"
	return out


# Parse/compile diagnostics from the engine log delta, with line numbers
# rebased to the agent's code where the gdscript:// frame exposes them.
static func _collect_errors(hub: McpLogHub, mark: int, offset: int) -> Array:
	var diagnostics: Array = []
	if hub != null and mark >= 0:
		for block: Dictionary in hub.engine_delta(mark):
			if String(block["level"]) != "error":
				continue
			diagnostics.append(_rebase_line_numbers(String(block["text"]), offset))
	if diagnostics.is_empty():
		diagnostics.append("The script failed to parse. (Engine file logging is unavailable, so the exact diagnostic could not be captured — check the editor console.)")
	return diagnostics


# Rewrite `var x := ...` to `var x: Variant = ...` on every generated-source
# line a "Cannot infer the type" diagnostic names. Returns the source
# unchanged when nothing was fixable (caller stops retrying).
static func _auto_type_inference_failures(source: String, diagnostics: Array) -> String:
	var line_regex := RegEx.create_from_string("gdscript://[^)]*\\.gd:(\\d+)")
	var decl_regex := RegEx.create_from_string("^(\\s*var\\s+[A-Za-z_][A-Za-z0-9_]*)\\s*:=")
	var lines := source.split("\n")
	var changed := false
	for diagnostic in diagnostics:
		var text := String(diagnostic)
		if not text.contains("Cannot infer the type"):
			continue
		var found := line_regex.search(text)
		if found == null:
			continue
		var index := found.get_string(1).to_int() - 1
		if index < 0 or index >= lines.size():
			continue
		var rewritten := decl_regex.sub(lines[index], "$1: Variant =")
		if rewritten != lines[index]:
			lines[index] = rewritten
			changed = true
	return "\n".join(lines) if changed else source


# Generated-source line N is agent-code line N - offset; surface that next to
# the raw `(gdscript://....gd:N)` frame so agents can map errors to their code.
static func _rebase_line_numbers(text: String, offset: int) -> String:
	if offset <= 0:
		return text
	var regex := RegEx.create_from_string("gdscript://[^)]*\\.gd:(\\d+)")
	var found := regex.search(text)
	if found == null:
		return text
	var user_line := found.get_string(1).to_int() - offset
	if user_line < 1:
		return text
	return text + "\n(agent code line %d)" % user_line
