extends GutTest

# B4: object undo — the shadow-step funnel. Mutations reach the editor through
# NovaObjectData's DEFERRED object_changed signal, so steps record there
# against the cached pre-mutation baseline (the B3 native edit-state blob);
# inspector call sites need zero changes. The native byte roundtrip and
# mismatched-geometry rejection are pinned in object_editor_test.gd.

const ObjectEditorScript = preload("res://modtools/object/object_editor.gd")
const ObjectWorkspaceScript = preload("res://modtools/object/object_workspace.gd")

const BIRD_FIXTURE := "res://../fixtures/3dp/Bird1/Bird1.3di"
const ARMRY_FIXTURE := "res://../fixtures/3dp/armry01/Armry01.3di"


func after_each() -> void:
	# Mirrors object_editor_test.gd: workspaces are RefCounted, but the editor
	# nodes they spawn can land under the test root.
	var object_children := []
	for child in get_children():
		if String(child.name).begins_with("ObjectEditor"):
			object_children.append(child)
	for child in object_children:
		if is_instance_valid(child):
			remove_child(child)
			child.free()


func _open_bird() -> ObjectEditor:
	var editor: ObjectEditor = add_child_autofree(ObjectEditorScript.new())
	assert_eq(editor.open_object(ProjectSettings.globalize_path(BIRD_FIXTURE)), OK)
	return editor


func _alpha(editor: ObjectEditor) -> int:
	return int(editor.object_data.get_material_info(0).get("alpha_test", -1))


func test_field_edit_records_one_step_and_roundtrips() -> void:
	var editor := _open_bird()
	assert_false(editor.can_undo(), "A fresh open starts with no history.")
	var original := _alpha(editor)

	editor.object_data.set_material_alpha_threshold(0, 0.73)
	await get_tree().process_frame  # the deferred object_changed funnel
	var mutated := _alpha(editor)
	assert_ne(mutated, original)
	assert_true(editor.can_undo(), "The funnel recorded the field edit.")

	editor.undo()
	assert_eq(_alpha(editor), original, "Undo restores the field.")
	assert_true(editor.can_redo())
	# The undo's own deferred signal must be consumed silently: waiting a
	# frame must not record a new step (which would wipe the redo stack).
	await get_tree().process_frame
	assert_true(editor.can_redo(), "The suspend guard ate the apply's signal.")
	editor.redo()
	assert_eq(_alpha(editor), mutated, "Redo reapplies the field.")


func test_part_anim_add_and_delete_restore() -> void:
	var editor := _open_bird()
	var base_count: int = editor.object_data.get_part_anim_count(0)

	editor.object_data.add_part_anim(0, 0)
	await get_tree().process_frame
	assert_eq(editor.object_data.get_part_anim_count(0), base_count + 1)
	assert_true(editor.can_undo())
	editor.undo()
	assert_eq(editor.object_data.get_part_anim_count(0), base_count,
		"Undo realloc-restores the part-anim count.")

	editor.object_data.add_part_anim(0, 0)
	await get_tree().process_frame
	assert_true(editor.object_data.delete_part_anim(0, base_count))
	await get_tree().process_frame
	assert_eq(editor.object_data.get_part_anim_count(0), base_count)
	editor.undo()
	assert_eq(editor.object_data.get_part_anim_count(0), base_count + 1,
		"Undo restores the deleted animation.")


func test_workspace_derives_object_undo() -> void:
	var workspace = ObjectWorkspaceScript.new()
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(BIRD_FIXTURE)), OK)
	var editor: ObjectEditor = workspace.object_editor
	var original := _alpha(editor)
	editor.object_data.set_material_alpha_threshold(0, 0.42)
	await get_tree().process_frame
	assert_true(workspace.can_undo(), "Workspace can_undo derives from the editor document.")
	workspace.undo()
	assert_eq(_alpha(editor), original, "Workspace undo routes to the editor document.")
	if editor != null and is_instance_valid(editor) and editor.get_parent() == null:
		editor.free()


func test_open_and_geometry_swap_clear_history() -> void:
	var editor := _open_bird()
	editor.object_data.set_material_alpha_threshold(0, 0.9)
	await get_tree().process_frame
	assert_true(editor.can_undo())

	assert_eq(editor.open_object(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_false(editor.can_undo(), "Opening a document drops the old history.")
	assert_false(editor.is_dirty, "A fresh open starts clean.")
