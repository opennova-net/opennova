class_name FileDialogHelper
extends RefCounted

## Reusable "open a file" dialog. Lazily creates one native FileDialog under a
## host node, then on each open() rebinds the title/filters/dir and a one-shot
## file_selected callback (clearing any prior connection). Collapses the
## copy-pasted create-once / reconnect-one-shot / popup blocks in the terrain
## asset dock and tile inspector into one place.
##
## Usage:
##   var _files := FileDialogHelper.new(self)   # host owns the dialog node
##   _files.open("Load tile layout", PackedStringArray(["*.til ; Tiles"]),
##       func(path): editor.load_tileinfo(path), editor.get_last_open_dir())

const _DEFAULT_MIN_SIZE := Vector2i(760, 520)

var _host: Node
var _dialog: FileDialog


func _init(host: Node) -> void:
	_host = host


func _ensure() -> FileDialog:
	if _dialog == null:
		_dialog = FileDialog.new()
		_dialog.use_native_dialog = true
		_dialog.access = FileDialog.ACCESS_FILESYSTEM
		_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
		_dialog.min_size = _DEFAULT_MIN_SIZE
		_host.add_child(_dialog)
	return _dialog


## Configure the (cached) dialog for one pick and pop it. on_pick fires once.
func open(title: String, filters: PackedStringArray, on_pick: Callable, current_dir: String = "") -> void:
	var dialog := _ensure()
	dialog.title = title
	dialog.filters = filters
	if not current_dir.is_empty():
		dialog.current_dir = current_dir
	for sig in dialog.file_selected.get_connections():
		dialog.file_selected.disconnect(sig["callable"])
	dialog.file_selected.connect(on_pick, CONNECT_ONE_SHOT)
	dialog.popup_centered()


func get_dialog() -> FileDialog:
	return _dialog
