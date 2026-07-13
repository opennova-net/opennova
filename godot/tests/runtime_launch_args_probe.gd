extends SceneTree

## Real-process probe for the runtime command-line boundary. GUT covers the
## pure parser with injected arrays; this script proves Godot preserves the
## actual tokens after its argument separator, including a private ONED root
## containing spaces.
##
## scripts/test_godot.sh deliberately includes the retired /game token. The
## probe requires that token to reach the process, then verifies it creates
## neither state nor an API on NovaLaunchFlags.

const LaunchFlags := preload("res://engine/resource_index/nova_launch_flags.gd")
const EXPECTED_ROOT := "C:/OpenNova Loose Root"
const OK_SENTINEL := "OPENNOVA_RUNTIME_LAUNCH_ARGS_PROBE: OK"
const MODE_ENV := "OPENNOVA_RUNTIME_LAUNCH_ARGS_MODE"
const MODE_DIRECT := "direct"
const MODE_ONED := "oned"


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var mode := OS.get_environment(MODE_ENV).strip_edges().to_lower()
	var user_args := OS.get_cmdline_user_args()
	var observed_args := OS.get_cmdline_args()
	var expected_root := ""
	if mode == MODE_ONED:
		observed_args = user_args
		expected_root = EXPECTED_ROOT
	var parsed = LaunchFlags.from_process("saved-exp")
	var failures: Array[String] = []

	if mode != MODE_DIRECT and mode != MODE_ONED:
		failures.append("set %s to '%s' or '%s'" % [MODE_ENV, MODE_DIRECT, MODE_ONED])
	elif mode == MODE_DIRECT and not user_args.is_empty():
		failures.append("direct exported-game probe unexpectedly received Godot user args")
	elif mode == MODE_ONED and user_args.is_empty():
		failures.append("ONED probe did not receive arguments after Godot's separator")
	if _count_option(observed_args, "/d") != 1:
		failures.append("expected one /D token in observed process args")
	if _count_option(observed_args, "/exp") != 1:
		failures.append("expected one /EXP token in observed process args")
	if _count_option(observed_args, "/game") != 1:
		failures.append("the probe must receive the retired /game token")
	var actual_root := _value_after(observed_args, "--oned-resource-root")
	if actual_root != expected_root:
		failures.append(
			"private ONED root mismatch: expected '%s', got '%s' (args=%s)"
			% [expected_root, actual_root, observed_args]
		)

	if not parsed.loose_override:
		failures.append("/D did not enable the loose override")
	if parsed.expansion != "revx02" or not parsed.has_expansion_override:
		failures.append("/EXP revx02 was not honored as an explicit expansion")
	if parsed.oned_resource_root != expected_root:
		failures.append(
			"private ONED root was not honored with /D (got '%s')"
			% parsed.oned_resource_root
		)
	if _has_property(parsed, "game") or _has_script_method("game"):
		failures.append("/game still exposes runtime state or a parser API")

	if not failures.is_empty():
		for failure in failures:
			push_error("runtime_launch_args_probe: " + failure)
		print("OPENNOVA_RUNTIME_LAUNCH_ARGS_PROBE: FAIL")
		quit(1)
		return

	print("%s (%s)" % [OK_SENTINEL, mode])
	quit(0)


func _count_option(args: PackedStringArray, wanted: String) -> int:
	var count := 0
	for arg in args:
		if arg.to_lower() == wanted:
			count += 1
	return count


func _value_after(args: PackedStringArray, wanted: String) -> String:
	for i in range(args.size() - 1):
		if args[i].to_lower() == wanted:
			return args[i + 1]
	return ""


func _has_property(value: Object, wanted: String) -> bool:
	for property in value.get_property_list():
		if String(property.get("name", "")) == wanted:
			return true
	return false


func _has_script_method(wanted: String) -> bool:
	var instance := LaunchFlags.new()
	var script: Script = instance.get_script()
	for method in script.get_script_method_list():
		if String(method.get("name", "")) == wanted:
			return true
	return false
