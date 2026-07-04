class_name ObjectInspector
extends WorkflowInspector

## Thin object-workspace base (B7): re-adds the object-coupled accessors and
## coordinator callbacks that WorkflowInspector dropped when it went generic.
## NOTE: ObjectListDetailInspector carries a documented copy of this block —
## it sits on the ListDetailInspector chain and GDScript has no mixins. Keep
## the two in sync.

var object_editor: ObjectEditor:
	get:
		return _ws.object_editor

var _preview: ObjectPreview:
	get:
		return _ws._preview


func _object_data():
	return object_editor.object_data if object_editor != null else null


func _control_registers() -> Array:
	var data = _object_data()
	if data != null and data.has_method("get_control_registers"):
		return data.get_control_registers()
	return []


func _rebuild_detail_dock() -> void:
	_ws._ensure_detail_dock().rebuild()


func _notify_shell() -> void:
	_ws._sync_shell()


func _add_ctrl_reg_row(parent: Control, node_name: String, label_text: String):
	return ObjectForms.add_ctrl_reg_row(parent, node_name, label_text, _control_registers())


func _add_detail_ctrl_reg_row(parent: Control, node_name: String, label_text: String):
	return ObjectForms.add_detail_ctrl_reg_row(parent, node_name, label_text, _control_registers())
