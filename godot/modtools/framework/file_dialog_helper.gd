class_name FileDialogHelper
extends RefCounted

## Reusable "open a file" dialog. Lazily creates one native FileDialog under a
## parent_node node, then on each open() rebinds the title/filters/dir and a one-shot
## file_selected callback (clearing any prior connection). Collapses the
## copy-pasted create-once / reconnect-one-shot / popup blocks in the terrain
## asset dock and tile inspector into one place.
##
## Usage:
##   var _files := FileDialogHelper.new(self)   # parent_node owns the dialog node
##   _files.open("Load tile layout", PackedStringArray(["*.til ; Tiles"]),
##       func(path): editor.load_tileinfo(path), editor.get_last_open_dir())

const _DEFAULT_MIN_SIZE := Vector2i(760, 520)

var _parent_node: Node
var _dialog: FileDialog


func _init(parent_node: Node) -> void:
	_parent_node = parent_node


func _ensure() -> FileDialog:
	if _dialog == null:
		_dialog = FileDialog.new()
		_dialog.use_native_dialog = true
		_dialog.access = FileDialog.ACCESS_FILESYSTEM
		_dialog.min_size = _DEFAULT_MIN_SIZE
		_parent_node.add_child(_dialog)
	return _dialog


## Configure the (cached) dialog to pick a file and pop it. on_pick fires once.
func open(title: String, filters: PackedStringArray, on_pick: Callable, current_dir: String = "") -> void:
	var dialog := _ensure()
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	dialog.title = title
	dialog.filters = filters
	dialog.current_file = ""
	if not current_dir.is_empty():
		dialog.current_dir = current_dir
	for sig in dialog.file_selected.get_connections():
		dialog.file_selected.disconnect(sig["callable"])
	dialog.file_selected.connect(on_pick, CONNECT_ONE_SHOT)
	dialog.popup_centered()


## Configure the (cached) dialog to pick one or more files. on_pick(PackedStringArray)
## fires once with the selected paths.
func open_files(title: String, filters: PackedStringArray, on_pick: Callable, current_dir: String = "") -> void:
	var dialog := _ensure()
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILES
	dialog.title = title
	dialog.filters = filters
	dialog.current_file = ""
	if not current_dir.is_empty():
		dialog.current_dir = current_dir
	for sig in dialog.files_selected.get_connections():
		dialog.files_selected.disconnect(sig["callable"])
	dialog.files_selected.connect(on_pick, CONNECT_ONE_SHOT)
	dialog.popup_centered()


## Configure the (cached) dialog to pick a directory and pop it. The same cached
## dialog serves both file and directory picks; on_pick fires once.
func open_dir(title: String, on_pick: Callable, current_dir: String = "") -> void:
	var dialog := _ensure()
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	dialog.title = title
	dialog.filters = PackedStringArray()
	dialog.current_file = ""
	if not current_dir.is_empty():
		dialog.current_dir = current_dir
	for sig in dialog.dir_selected.get_connections():
		dialog.dir_selected.disconnect(sig["callable"])
	dialog.dir_selected.connect(on_pick, CONNECT_ONE_SHOT)
	dialog.popup_centered()


## Configure the (cached) dialog to pick a destination file path (Save As). The
## name field is pre-filled with default_name. on_pick(String) fires once.
func save_file(title: String, filters: PackedStringArray, default_name: String, on_pick: Callable, current_dir: String = "") -> void:
	var dialog := _ensure()
	dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	dialog.title = title
	dialog.filters = filters
	if not current_dir.is_empty():
		dialog.current_dir = current_dir
	dialog.current_file = default_name
	for sig in dialog.file_selected.get_connections():
		dialog.file_selected.disconnect(sig["callable"])
	dialog.file_selected.connect(on_pick, CONNECT_ONE_SHOT)
	dialog.popup_centered()


func get_dialog() -> FileDialog:
	return _dialog
