class_name ViewportMount
extends RefCounted

## Owns the create-once / reparent / free lifecycle of a workspace's viewport
## Control so each EditorWorkspace adapter shares one implementation of the
## mechanics instead of copying it. Domain wiring (binding the editor, toggling
## input, activating) stays in the adapter, which calls mount()/unmount()/
## release() and reads get_viewport_node()/is_mounted().

var _node: Control
var _node_name: StringName
var _factory: Callable


func _init(node_name: StringName, factory: Callable) -> void:
	_node_name = node_name
	_factory = factory


func mount(mount: Control) -> Control:
	if mount == null:
		return _node
	if _node == null:
		_node = _factory.call()
		if _node == null:
			return null
		_node.name = _node_name
		_node.set_anchors_preset(Control.PRESET_FULL_RECT)
		_node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_node.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _node.get_parent() == null:
		mount.add_child(_node)
		_node.set_anchors_preset(Control.PRESET_FULL_RECT)
	return _node


func unmount() -> void:
	if _node != null and _node.get_parent() != null:
		_node.get_parent().remove_child(_node)


func release() -> void:
	if _node == null:
		return
	if _node.get_parent() != null:
		_node.get_parent().remove_child(_node)
	_node.free()
	_node = null


func get_viewport_node() -> Control:
	return _node


func is_mounted() -> bool:
	return _node != null and _node.get_parent() != null
