class_name WebRetailPicker
extends RefCounted
## The web build's PLAY RETAIL folder picker (ADR 0049). A browser has no
## native folder dialog, so the shell page's staging module
## (godot/web/opennova_stage.js) shows its own: the player picks their Joint
## Operations folder, the browser reads it locally (nothing is uploaded), the
## module copies it into the in-memory filesystem and answers with the mount
## path, or "" when the player cancels.

# The JavaScript callback must stay referenced until the page calls it.
static var _pending: JavaScriptObject


## False (and `done` never runs) outside the web build or when the page carries
## no staging module; otherwise `done` receives the mount path or "".
static func request(done: Callable) -> bool:
	if not OS.has_feature("web"):
		return false
	var stage := JavaScriptBridge.get_interface("opennovaStage")
	if stage == null:
		push_warning("WebRetailPicker: the page has no opennovaStage module")
		return false
	_pending = JavaScriptBridge.create_callback(func(args: Array) -> void:
		_pending = null
		done.call(str(args[0]) if not args.is_empty() else ""))
	stage.pickRetail(_pending)
	return true


## Delete a staged pick that did not mount: the copy lives in the tab's memory.
static func discard(dir: String) -> void:
	var listing := DirAccess.open(dir)
	if listing == null:
		return
	for sub in listing.get_directories():
		discard(dir.path_join(sub))
	for file in listing.get_files():
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)
