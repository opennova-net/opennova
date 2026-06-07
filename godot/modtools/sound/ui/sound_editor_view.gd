extends Control

## Center editing surface for the Sound workspace: a Tree of Sound Sets -> Layers
## -> Members with an add / remove / reorder / play toolbar. Selection is pushed
## to the SoundController; the workspace re-drives rebuild()/sync_selection() off
## the controller's structure_changed / selection_changed channels.

const SEL_NONE := 0
const SEL_SET := 1
const SEL_LAYER := 2
const SEL_MEMBER := 3

const MODE_NAMES := ["First", "Random", "Sequential", "Random sequential"]

var _ws  # SoundEditorWorkspace
var _doc: SoundController
var _tree: Tree
var _syncing: bool = false
var _item_by_key: Dictionary = {}


func set_workspace(workspace) -> void:
	_ws = workspace
	_doc = workspace.get_document()
	rebuild()
	sync_selection()


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var root_box := VBoxContainer.new()
	root_box.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(root_box)

	var toolbar := HBoxContainer.new()
	toolbar.name = "SoundToolbar"
	root_box.add_child(toolbar)
	_add_tool(toolbar, "Add Set", _on_add_set)
	_add_tool(toolbar, "Add Layer", _on_add_layer)
	_add_tool(toolbar, "Add Member", _on_add_member)
	_add_tool(toolbar, "Remove", _on_remove)
	_add_tool(toolbar, "Move Up", _on_move_up)
	_add_tool(toolbar, "Move Down", _on_move_down)
	_add_tool(toolbar, "Play", _on_play)

	_tree = Tree.new()
	_tree.name = "SoundTree"
	_tree.hide_root = true
	_tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.item_selected.connect(_on_item_selected)
	_tree.item_activated.connect(_on_play)
	root_box.add_child(_tree)

	if _doc != null:
		rebuild()
		sync_selection()


func _add_tool(parent: Control, text: String, handler: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.focus_mode = Control.FOCUS_NONE
	b.pressed.connect(handler)
	parent.add_child(b)
	return b


# --- Build ---

func rebuild() -> void:
	if _tree == null or _doc == null:
		return
	_syncing = true
	_tree.clear()
	_item_by_key.clear()
	var root := _tree.create_item()
	var data := _doc.data
	for si in data.get_set_count():
		var set_d := data.get_set(si)
		var set_item := _tree.create_item(root)
		set_item.set_text(0, "%s  (id %d)" % [_set_label(set_d), int(set_d.get("target_id", 0))])
		set_item.set_metadata(0, {"kind": SEL_SET, "set": si, "layer": -1, "member": -1})
		_item_by_key[_key(SEL_SET, si, -1, -1)] = set_item

		var layers: Array = set_d.get("layers", [])
		for li in layers.size():
			var layer_d: Dictionary = layers[li]
			var members: Array = layer_d.get("members", [])
			var layer_item := _tree.create_item(set_item)
			layer_item.set_text(0, "Layer %d — %s · %d member%s" % [
				li, _mode_name(int(layer_d.get("selection_mode", 0))),
				members.size(), "" if members.size() == 1 else "s"])
			layer_item.set_metadata(0, {"kind": SEL_LAYER, "set": si, "layer": li, "member": -1})
			_item_by_key[_key(SEL_LAYER, si, li, -1)] = layer_item

			for mi in members.size():
				var member_d: Dictionary = members[mi]
				var member_item := _tree.create_item(layer_item)
				member_item.set_text(0, "%s  ·  vol %d" % [_member_label(member_d), int(member_d.get("volume", 0))])
				member_item.set_metadata(0, {"kind": SEL_MEMBER, "set": si, "layer": li, "member": mi})
				_item_by_key[_key(SEL_MEMBER, si, li, mi)] = member_item
	_syncing = false


func sync_selection() -> void:
	if _tree == null or _doc == null:
		return
	var sel := _doc.get_selection()
	var key := _key(int(sel.get("kind", SEL_NONE)), int(sel.get("set", -1)), int(sel.get("layer", -1)), int(sel.get("member", -1)))
	if not _item_by_key.has(key):
		return
	_syncing = true
	(_item_by_key[key] as TreeItem).select(0)
	_syncing = false


# --- Selection ---

func _on_item_selected() -> void:
	if _syncing or _doc == null:
		return
	var item := _tree.get_selected()
	if item == null:
		return
	var meta: Dictionary = item.get_metadata(0)
	match int(meta.get("kind", SEL_NONE)):
		SEL_SET:
			_doc.select_set(int(meta["set"]))
		SEL_LAYER:
			_doc.select_layer(int(meta["set"]), int(meta["layer"]))
		SEL_MEMBER:
			_doc.select_member(int(meta["set"]), int(meta["layer"]), int(meta["member"]))


# --- Toolbar handlers (context-sensitive on the current selection) ---

func _on_add_set() -> void:
	if _doc != null:
		_doc.add_set()


func _on_add_layer() -> void:
	if _doc == null:
		return
	var sel := _doc.get_selection()
	var si := int(sel.get("set", -1))
	if si < 0 and _doc.data.get_set_count() > 0:
		si = 0
	if si >= 0:
		_doc.add_layer(si)


func _on_add_member() -> void:
	if _doc == null:
		return
	var sel := _doc.get_selection()
	var si := int(sel.get("set", -1))
	var li := int(sel.get("layer", -1))
	if si >= 0 and li >= 0:
		_doc.add_member(si, li)


func _on_remove() -> void:
	if _doc == null:
		return
	var sel := _doc.get_selection()
	match int(sel.get("kind", SEL_NONE)):
		SEL_MEMBER:
			_doc.remove_member(int(sel["set"]), int(sel["layer"]), int(sel["member"]))
		SEL_LAYER:
			_doc.remove_layer(int(sel["set"]), int(sel["layer"]))
		SEL_SET:
			_doc.remove_set(int(sel["set"]))


func _on_move_up() -> void:
	_move(-1)


func _on_move_down() -> void:
	_move(1)


func _move(delta: int) -> void:
	if _doc == null:
		return
	var sel := _doc.get_selection()
	match int(sel.get("kind", SEL_NONE)):
		SEL_MEMBER:
			_doc.move_member(int(sel["set"]), int(sel["layer"]), int(sel["member"]), int(sel["member"]) + delta)
		SEL_LAYER:
			_doc.move_layer(int(sel["set"]), int(sel["layer"]), int(sel["layer"]) + delta)
		SEL_SET:
			_doc.move_set(int(sel["set"]), int(sel["set"]) + delta)


func _on_play() -> void:
	if _doc == null or _ws == null:
		return
	var sel := _doc.get_selection()
	match int(sel.get("kind", SEL_NONE)):
		SEL_MEMBER:
			_ws.preview_member(int(sel["set"]), int(sel["layer"]), int(sel["member"]))
		SEL_LAYER, SEL_SET:
			_ws.preview_set(int(sel["set"]))


# --- Labels ---

func _set_label(set_d: Dictionary) -> String:
	var name := String(set_d.get("name", ""))
	return name if not name.is_empty() else "(unnamed set)"


func _member_label(member_d: Dictionary) -> String:
	var path := String(member_d.get("wav_path", ""))
	if not path.is_empty():
		return path.get_file()
	var name := String(member_d.get("name", ""))
	return name if not name.is_empty() else "(empty)"


func _mode_name(mode: int) -> String:
	return MODE_NAMES[mode] if mode >= 0 and mode < MODE_NAMES.size() else "First"


func _key(kind: int, si: int, li: int, mi: int) -> String:
	return "%d:%d:%d:%d" % [kind, si, li, mi]
