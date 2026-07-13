class_name McpSettings
extends RefCounted

## ONED's embedded MCP-server preferences and launch overrides. Persistence is
## delegated to NovaOnedSettings so the host-neutral MCP core in engine/ owns
## no editor product state.
##
## Flags (engine + user args, either --flag value or --flag=value):
##   --mcp-port N   listen on N this launch (also forces the server on)
##   --mcp-off      do not start the server this launch

const OnedSettings := preload("res://modtools/editor/oned_settings.gd")
const CONFIG_PATH := OnedSettings.CONFIG_PATH
const DEFAULT_PORT := OnedSettings.DEFAULT_MCP_PORT


static func get_enabled() -> bool:
	return OnedSettings.get_mcp_enabled()


static func set_enabled(value: bool) -> void:
	OnedSettings.set_mcp_enabled(value)


static func get_port() -> int:
	return OnedSettings.get_mcp_port()


static func set_port(value: int) -> void:
	OnedSettings.set_mcp_port(value)


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


## Whether the server should auto-start this launch: headless never;
## --mcp-off never; --mcp-port forces on; otherwise use the ONED preference.
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
