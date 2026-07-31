class_name GameMcpService
extends Node

## Ephemeral runtime MCP service for an editor-managed game process. A normal
## standalone game never starts it: both an opaque run id and an explicit
## descriptor path must be present. The descriptor is the one-way readiness
## handshake ONED watches before connecting its stable MCP proxy.

const RUN_ID_ARG := "--oned-run-id"
const DESCRIPTOR_ARG := "--oned-run-descriptor"
const LOG_PATH_ARG := "--oned-run-log"

var server: McpServer = null
var log_hub: McpLogHub = null
var tools: GameMcpTools = null
var game_adapter: GameMcpAdapter = null

var _run_id := ""
var _descriptor_path := ""
var _log_path := ""
var _shutdown_requested := false


static func launch_metadata() -> Dictionary:
	var args := OS.get_cmdline_args()
	args.append_array(OS.get_cmdline_user_args())
	var run_id := _arg_value(args, RUN_ID_ARG)
	var descriptor := _arg_value(args, DESCRIPTOR_ARG)
	if run_id.is_empty() or descriptor.is_empty():
		return {}
	if not is_launch_descriptor_safe(run_id, descriptor):
		return {}
	return {
		"run_id": run_id,
		"descriptor_path": descriptor,
		"log_path": _arg_value(args, LOG_PATH_ARG),
	}


static func should_start() -> bool:
	return not launch_metadata().is_empty()


func setup(adapter: GameMcpAdapter) -> Error:
	var metadata := launch_metadata()
	if metadata.is_empty():
		return ERR_UNAVAILABLE
	return setup_from_metadata(adapter, metadata)


## Start the ephemeral endpoint from already-parsed launch metadata. The game
## shell uses setup(); this seam keeps launch validation and endpoint lifecycle
## testable without mutating process command-line state.
func setup_from_metadata(adapter: GameMcpAdapter, metadata: Dictionary) -> Error:
	var run_id := String(metadata.get("run_id", ""))
	var descriptor_path := String(metadata.get("descriptor_path", ""))
	if adapter == null or not is_launch_descriptor_safe(run_id, descriptor_path):
		return ERR_INVALID_PARAMETER
	game_adapter = adapter
	_run_id = run_id
	_descriptor_path = descriptor_path
	_log_path = String(metadata.get("log_path", ""))
	log_hub = McpLogHub.new()
	if not _log_path.is_empty():
		log_hub.set_engine_log_path(ProjectSettings.globalize_path(_log_path))
	McpLogHub.instance = log_hub
	server = McpServer.new()
	server.name = "GameMcpServer"
	server.server_name = "opennova-game"
	server.server_title = "OpenNova Game Runtime"
	server.instructions = "Inspect and control the editor-launched real game. "
	server.context_factory = _make_context
	server.log_sink = _on_server_log
	add_child(server)
	tools = GameMcpTools.new(
			self, game_adapter, request_endpoint_shutdown)
	tools.register_all(server.registry)
	var err := server.start(0)
	if err != OK:
		log_hub.note("server", "error",
				"Runtime MCP failed to bind: %s" % error_string(err))
		return err
	log_hub.note_server("listening at %s" % server.get_url())
	err = _write_descriptor()
	if err != OK:
		server.stop()
		return err
	return OK


func is_running() -> bool:
	return server != null and server.is_running()


## Reserved editor-to-child control: retire only the ephemeral debug listener.
## The MainGame node and game process continue running untouched.
func request_endpoint_shutdown() -> void:
	if _shutdown_requested:
		return
	_shutdown_requested = true
	call_deferred("_shutdown_endpoint")


func _shutdown_endpoint() -> void:
	_remove_owned_descriptor()
	if server != null:
		server.stop()
	if McpLogHub.instance == log_hub:
		McpLogHub.instance = null


func _make_context(args: Dictionary) -> McpToolContext:
	var ctx := McpToolContext.new()
	ctx.tree = get_tree()
	ctx.args = args
	ctx.log_sink = _on_script_log
	return ctx


func _on_script_log(text: String) -> void:
	if log_hub != null:
		log_hub.note("script", "info", text)


func _on_server_log(text: String) -> void:
	if log_hub != null:
		log_hub.note_server(text)


func _write_descriptor() -> Error:
	var absolute := ProjectSettings.globalize_path(_descriptor_path)
	var dir_error := DirAccess.make_dir_recursive_absolute(
			absolute.get_base_dir())
	if dir_error != OK:
		return dir_error
	var temp := "%s.tmp.%d" % [absolute, OS.get_process_id()]
	var file := FileAccess.open(temp, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_string(JSON.stringify({
		"run_id": _run_id,
		"pid": OS.get_process_id(),
		"url": server.get_url(),
		"port": server.get_port(),
		"transport": "streamable-http",
		"log_path": _log_path,
		"version": str(ProjectSettings.get_setting(
				"application/config/version", "0.0.0")),
		"started_at": Time.get_datetime_string_from_system(),
	}, "\t"))
	file.flush()
	var write_error := file.get_error()
	file.close()
	if write_error != OK:
		DirAccess.remove_absolute(temp)
		return write_error
	if FileAccess.file_exists(absolute):
		DirAccess.remove_absolute(absolute)
	var rename_error := DirAccess.rename_absolute(temp, absolute)
	if rename_error != OK:
		DirAccess.remove_absolute(temp)
	return rename_error


func _remove_owned_descriptor() -> void:
	if _descriptor_path.is_empty():
		return
	var absolute := ProjectSettings.globalize_path(_descriptor_path)
	if not FileAccess.file_exists(absolute):
		return
	var file := FileAccess.open(absolute, FileAccess.READ)
	if file == null:
		return
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if parsed is Dictionary and String(parsed.get("run_id", "")) == _run_id \
			and int(parsed.get("pid", -1)) == OS.get_process_id():
		DirAccess.remove_absolute(absolute)


func _exit_tree() -> void:
	_shutdown_endpoint()


static func _arg_value(args: PackedStringArray, name: String) -> String:
	for i in range(args.size()):
		var arg := String(args[i])
		if arg == name and i + 1 < args.size():
			return String(args[i + 1]).strip_edges()
		if arg.begins_with(name + "="):
			return arg.get_slice("=", 1).strip_edges()
	return ""


## Whether a launch handshake path is confined to this project's user-data
## directory and exactly names its opaque run id.
static func is_launch_descriptor_safe(run_id: String, descriptor: String) -> bool:
	if run_id != run_id.get_file() or run_id != run_id.validate_filename():
		return false
	var absolute := ProjectSettings.globalize_path(descriptor).simplify_path()
	var user_root := ProjectSettings.globalize_path("user://").simplify_path()
	var canonical_dir := _canonical_path(absolute.get_base_dir())
	var canonical_root := _canonical_path(user_root)
	return canonical_dir == canonical_root \
			and absolute.get_file() == "oned-run-%s.json" % run_id


static func _canonical_path(
		path: String,
		case_insensitive: bool = OS.get_name() == "Windows") -> String:
	var normalized := path.replace("\\", "/").rstrip("/")
	return normalized.to_lower() if case_insensitive else normalized
