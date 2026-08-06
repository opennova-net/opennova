class_name McpSettings
extends RefCounted

## Persistence + launch flags for the embedded MCP server. Lives in the shared
## editor/runtime config (user://terrain_editor_state.cfg) under [mcp],
## following the NovaResourceDirSettings pattern. The server is ON by default
## wherever ONED runs (project decision; the Settings toggle is the off
## switch, and a future release may flip the packaged default to opt-in) but
## never in headless runs — tests and CI must not open sockets implicitly.
##
## Flags (engine + user args, either `--flag value` or `--flag=value`):
##   --mcp-port N   listen on N this launch (also forces the server on)
##   --mcp-off      do not start the server this launch

const CONFIG_PATH := "user://terrain_editor_state.cfg"
const SECTION := "mcp"
const ENABLED_KEY := "enabled"
const PORT_KEY := "port"
const DEFAULT_PORT := 8975


static func get_enabled() -> bool:
	return bool(NovaConfigStore.read(CONFIG_PATH, SECTION, ENABLED_KEY, true))


static func set_enabled(value: bool) -> void:
	NovaConfigStore.write(CONFIG_PATH, SECTION, ENABLED_KEY, value)


static func get_port() -> int:
	return clampi(int(NovaConfigStore.read(CONFIG_PATH, SECTION, PORT_KEY, DEFAULT_PORT)), 1024, 65535)


static func set_port(value: int) -> void:
	NovaConfigStore.write(CONFIG_PATH, SECTION, PORT_KEY, clampi(value, 1024, 65535))


## The --mcp-port flag value, or -1 when absent/invalid.
static func flag_port() -> int:
	var args := _all_args()
	for i in args.size():
		var arg := args[i].to_lower()
		if arg == "--mcp-port" and i + 1 < args.size() and args[i + 1].is_valid_int():
			return clampi(args[i + 1].to_int(), 1024, 65535)
		if arg.begins_with("--mcp-port="):
			var value := arg.get_slice("=", 1)
			if value.is_valid_int():
				return clampi(value.to_int(), 1024, 65535)
	return -1


static func flag_off() -> bool:
	for arg in _all_args():
		if arg.to_lower() == "--mcp-off":
			return true
	return false


## Whether the server should auto-start this launch:
## headless never; --mcp-off never; --mcp-port forces on; else the persisted
## toggle (default ON).
static func resolve_enabled() -> bool:
	if DisplayServer.get_name() == "headless":
		return false
	if flag_off():
		return false
	if flag_port() > 0:
		return true
	return get_enabled()


static func resolve_port() -> int:
	var flagged := flag_port()
	return flagged if flagged > 0 else get_port()


static func _all_args() -> PackedStringArray:
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	return args
