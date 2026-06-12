extends GutTest

# McpScriptRunner: the three compile modes, awaiting code, the watchdog
# (timeouts and post-await script deaths), and error capture via the engine
# log. Intentional script errors are declared with assert_engine_error so
# GUT's error tracker treats them as expected.


func before_each() -> void:
	McpLogHub.instance = McpLogHub.new()


func after_each() -> void:
	McpLogHub.instance = null


func _ctx() -> McpToolContext:
	return McpToolContext.new()


func test_body_mode_returns_value() -> void:
	var compiled := McpScriptRunner.compile("return 40 + 2")
	assert_true(compiled["ok"], "Bare body compiles into run(ctx).")
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx())
	assert_true(outcome["ok"])
	assert_eq(outcome["result"], 42)
	assert_false(outcome["timed_out"])


func test_bare_run_function_mode() -> void:
	var compiled := McpScriptRunner.compile("func run(_ctx):\n\treturn \"bare\"")
	assert_true(compiled["ok"])
	assert_eq(compiled["line_offset"], 2)
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx())
	assert_eq(outcome["result"], "bare")


func test_inference_idiom_auto_typed() -> void:
	# inference_on_variant is an ERROR at project level and cannot be
	# warning-ignored; compile() must auto-retype the flagged declaration to
	# `var x: Variant = ...` and retry so the natural agent idiom works.
	var compiled := McpScriptRunner.compile("var value := ctx.args.get(\"missing\", 7)\nreturn value")
	assert_true(compiled["ok"], str(compiled["compile_errors"]))
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx())
	assert_eq(outcome["result"], 7)
	_expect_parse_errors()


func test_multiple_inference_lines_auto_typed() -> void:
	var code := "var a := ctx.args.get(\"a\", 1)\nvar b := ctx.args.get(\"b\", 2)\nreturn a + b"
	var compiled := McpScriptRunner.compile(code)
	assert_true(compiled["ok"], str(compiled["compile_errors"]))
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx())
	assert_eq(outcome["result"], 3)
	_expect_parse_errors()


# Mark every tracked parse error expected — auto-typing retries print one per
# rejected compile pass, and the pass count is an implementation detail.
func _expect_parse_errors() -> void:
	for tracked in get_errors():
		if tracked.contains_text("Parse Error"):
			tracked.handled = true


func test_full_script_mode() -> void:
	var code := "extends RefCounted\n\nfunc helper() -> int:\n\treturn 5\n\nfunc run(_ctx):\n\treturn helper() * 2\n"
	var compiled := McpScriptRunner.compile(code)
	assert_true(compiled["ok"])
	assert_eq(compiled["line_offset"], 0)
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx())
	assert_eq(outcome["result"], 10)


func test_awaiting_code_completes() -> void:
	var compiled := McpScriptRunner.compile("await ctx.frames(2)\nreturn \"waited\"")
	assert_true(compiled["ok"])
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx())
	assert_true(outcome["ok"])
	assert_eq(outcome["result"], "waited")


func test_ctx_logs_and_args_reach_code() -> void:
	var ctx := _ctx()
	ctx.args = { "x": 9 }
	var compiled := McpScriptRunner.compile("ctx.log(\"probe\")\nreturn ctx.args[\"x\"]")
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], ctx)
	assert_eq(outcome["result"], 9)
	assert_eq(outcome["logs"], ["probe"])


func test_extra_args_passed_to_run() -> void:
	var compiled := McpScriptRunner.compile("func run(_ctx, args):\n\treturn args[\"n\"] * 2")
	assert_true(compiled["ok"])
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx(), 1000, [{ "n": 21 }])
	assert_eq(outcome["result"], 42)


func test_compile_error_reported_not_thrown() -> void:
	var compiled := McpScriptRunner.compile("func broken(:")
	assert_false(compiled["ok"])
	assert_false(compiled.has("script"))
	assert_gt((compiled["compile_errors"] as Array).size(), 0, "Diagnostics captured.")
	assert_engine_error("Parse Error")


func test_compile_diagnostics_rebase_line_numbers() -> void:
	# Body mode wraps with 2 generated lines; an error on body line 2 reports
	# generated line 4 — the diagnostic should point back at agent line 2.
	var compiled := McpScriptRunner.compile("var ok := 1\nvar broken := !!!\nreturn ok")
	assert_false(compiled["ok"])
	if McpLogHub.instance.engine_available():
		var joined := "\n".join(PackedStringArray(compiled["compile_errors"].map(func(e): return String(e))))
		assert_true(joined.contains("agent code line"), "Rebased line marker present: %s" % joined)
	assert_engine_error("Parse Error")


func test_script_without_run_rejected() -> void:
	var compiled := McpScriptRunner.compile("extends RefCounted\nvar x := 1")
	assert_false(compiled["ok"])
	assert_true(String(compiled["compile_errors"][0]).contains("func run"))


func test_sync_runtime_error_captured() -> void:
	var compiled := McpScriptRunner.compile("var n = null\nreturn n.foo()")
	assert_true(compiled["ok"], "Compiles fine; fails at runtime.")
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx())
	assert_false(outcome["timed_out"], "Pre-await errors return synchronously.")
	if String(outcome["error_capture"]) == "engine_log":
		assert_false(outcome["ok"])
		assert_gt((outcome["errors"] as Array).size(), 0)
	assert_engine_error("Invalid call")


func test_post_await_error_returns_with_captured_errors() -> void:
	var compiled := McpScriptRunner.compile("await ctx.frames(1)\nvar n = null\nn.foo()\nreturn 1")
	assert_true(compiled["ok"])
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx(), 150)
	# Godot 4.6: an erroring coroutine aborts but still resumes its awaiter
	# with null, so this returns promptly instead of riding out the watchdog.
	assert_false(outcome["timed_out"])
	assert_eq(outcome["result"], null)
	if String(outcome["error_capture"]) == "engine_log":
		assert_false(outcome["ok"])
		assert_gt((outcome["errors"] as Array).size(), 0)
	assert_engine_error("Invalid call")


func test_timeout_on_long_running_code() -> void:
	var code := "var deadline := Time.get_ticks_msec() + 400\nwhile Time.get_ticks_msec() < deadline:\n\tawait ctx.frames(1)\nreturn \"slow\""
	var compiled := McpScriptRunner.compile(code)
	assert_true(compiled["ok"])
	var outcome: Dictionary = await McpScriptRunner.execute(compiled["script"], _ctx(), 100)
	assert_true(outcome["timed_out"])
	assert_false(outcome["ok"])
	# Drain the orphaned coroutine so it does not bleed into later tests.
	for i in range(60):
		await get_tree().process_frame
