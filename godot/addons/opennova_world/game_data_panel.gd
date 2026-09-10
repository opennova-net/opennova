@tool
extends MarginContainer
## The selected scene supplies the data. Only machine-local output/install paths
## live in editor metadata; format and process work belongs to godot/tools.

signal run_game_requested
signal run_retail_requested(retail_directory: String)
signal pack_requested(output_directory: String)
signal stop_requested

@onready var _source: Label = %Source
@onready var _retail: LineEdit = %RetailDirectory
@onready var _output: LineEdit = %OutputDirectory
@onready var _run: Button = %RunGame
@onready var _stage: Button = %StageRetail
@onready var _pack: Button = %Pack
@onready var _stop: Button = %Stop
@onready var _status: Label = %Status
var _settings: EditorSettings
var _folder: EditorFileDialog
var _selecting_retail := false
var _directory := ""
var _blocked := "Open a configured GameWorld scene."
var _pack_blocked := ""
var _retail_blocked := ""
var _stopping := false


func _ready() -> void:
	_run.pressed.connect(func() -> void: run_game_requested.emit())
	_stage.pressed.connect(func() -> void: run_retail_requested.emit(_retail.text.strip_edges()))
	_pack.pressed.connect(func() -> void: pack_requested.emit(_output.text.strip_edges()))
	_stop.pressed.connect(func() -> void: stop_requested.emit())
	(%BrowseRetail as Button).pressed.connect(_browse.bind(true))
	(%BrowseOutput as Button).pressed.connect(_browse.bind(false))
	_retail.text_changed.connect(_paths_changed)
	_output.text_changed.connect(_paths_changed)
	_folder = EditorFileDialog.new()
	_folder.file_mode = EditorFileDialog.FILE_MODE_OPEN_DIR
	_folder.access = EditorFileDialog.ACCESS_FILESYSTEM
	_folder.dir_selected.connect(_folder_selected)
	add_child(_folder)


func setup(settings: EditorSettings) -> void:
	_settings = settings
	_retail.text = str(settings.get_project_metadata("opennova_world", "retail_runtime_directory", ""))
	_output.text = str(settings.get_project_metadata("opennova_world", "pack_output_directory", ""))
	_update_actions()


func set_context(source: WorldSource, directory: String, blocked: String,
		running: bool, stopping: bool) -> void:
	_directory = directory
	_blocked = blocked
	_stopping = stopping
	_source.text = "%s | %s" % [source.mission_name, directory] if source != null else "No world selected"
	_source.tooltip_text = _source.text
	_pack_blocked = "Packing requires a loose source world. Archive-backed data can be staged for retail."
	_retail_blocked = "Retail staging supports Joint Operations worlds."
	if source != null:
		if source.source_kind == WorldSource.LOOSE_SOURCE:
			_pack_blocked = ""
		if source.game_code.strip_edges().to_lower() in ["", "jo", "jodemo"] and source.expansion.is_empty():
			_retail_blocked = ""
	_stop.disabled = not running and not stopping
	_update_actions()


func show_status(message: String) -> void:
	_status.text = message


func _update_actions() -> void:
	var blocked := "Wait for the running game to stop." if _stopping else _blocked
	_run.disabled = not blocked.is_empty()
	_run.tooltip_text = blocked if not blocked.is_empty() else "Save native changes and open the game menu using this world's data."
	var retail_error := blocked
	if retail_error.is_empty():
		retail_error = _retail_blocked
	if retail_error.is_empty() and not Process.supports_working_directory():
		retail_error = "Retail launch requires Windows."
	if retail_error.is_empty():
		retail_error = GamePacker.retail_install_error(_retail.text.strip_edges())
	_stage.disabled = not retail_error.is_empty()
	_stage.tooltip_text = retail_error if not retail_error.is_empty() else "Save, stage into the project cache, and open the retail game menu."
	var pack_error := blocked if not blocked.is_empty() else _pack_blocked
	if pack_error.is_empty():
		pack_error = GamePacker.output_directory_error(_directory, _output.text)
	_pack.disabled = not pack_error.is_empty()
	_pack.tooltip_text = pack_error if not pack_error.is_empty() else "Save and write localres.pff with required loose files into this output folder."


func _paths_changed(_text: String) -> void:
	if _settings != null:
		_settings.set_project_metadata("opennova_world", "retail_runtime_directory", _retail.text.strip_edges())
		_settings.set_project_metadata("opennova_world", "pack_output_directory", _output.text.strip_edges())
	_update_actions()


func _browse(retail: bool) -> void:
	_selecting_retail = retail
	_folder.title = "Joint Operations installation" if retail else "Packed game data output"
	_folder.current_dir = _retail.text if retail else _output.text
	_folder.popup_centered_ratio(0.65)


func _folder_selected(path: String) -> void:
	if _selecting_retail:
		_retail.text = path
	else:
		_output.text = path
	_paths_changed(path)
