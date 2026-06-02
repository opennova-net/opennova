extends GutTest

const MusicWorkspaceAdapter = preload("res://modtools/editor/music_workspace.gd")
const RootScene = preload("res://modtools/music/ui/music_workspace_root.tscn")
const BANK_FIXTURE := "res://../fixtures/sbf/jo_gamemus.sbf"


func test_workspace_id_and_label():
	var ws = MusicWorkspaceAdapter.new()
	assert_eq(ws.get_workspace_id(), "music")
	assert_eq(ws.get_workspace_label(), "Music")


func test_open_uses_indexed_quick_open_browser():
	# "music" routes Open to the shared indexed resource browser (the resource
	# index resolves it to .sbf banks + .bin SCR0 scripts), not the native
	# FileDialog. An empty kind would mean the legacy file-dialog fallback.
	var ws = MusicWorkspaceAdapter.new()
	assert_eq(ws.get_open_resource_kind(), "music")
	assert_true(ws.can_open(), "Music workspace exposes an Open action")


func test_workflows_advertised():
	var ws = MusicWorkspaceAdapter.new()
	var workflows: Array = ws.get_workflows()
	assert_eq(workflows.size(), 3, "Bank + Script + Live")
	# Workflows are typed InspectorDef rows (the shell does `entry as InspectorDef`).
	assert_eq(workflows[0].label, "Bank")
	assert_eq(workflows[1].label, "Script")
	assert_eq(workflows[2].label, "Live")


func test_mount_unmount_does_not_crash():
	var ws = MusicWorkspaceAdapter.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.mount_viewport(host)
	assert_eq(host.get_child_count(), 1, "root mounted")
	ws.unmount_viewport(host)
	# queue_free is async; flush
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(host.get_child_count(), 0, "root unmounted")


func test_workflow_activation_switches_visibility():
	var ws = MusicWorkspaceAdapter.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.mount_viewport(host)
	var root: Control = host.get_child(0)
	ws.activate_workflow(0)  # BANK
	assert_true(root.get_node("Bank").visible)
	assert_false(root.get_node("Script").visible)
	ws.activate_workflow(1)  # SCRIPT
	assert_false(root.get_node("Bank").visible)
	assert_true(root.get_node("Script").visible)


func test_undo_hooks_surface_bank_history():
	# The workspace implements the framework can_undo/undo/redo hooks (like Mission
	# and MNU) by routing to the document's per-workflow history, so the already
	# built bank-edit undo is reachable through the shell instead of stranded on the
	# document. A fresh document has nothing to undo; a bank reorder becomes
	# undoable; Live mode (no editable history) reports nothing.
	var ws = MusicWorkspaceAdapter.new()
	assert_false(ws.can_undo(), "fresh document: nothing to undo")
	assert_false(ws.can_redo(), "fresh document: nothing to redo")

	assert_eq(ws.open_file(BANK_FIXTURE), OK, "bank opens")
	ws.activate_workflow(0)  # BANK
	ws._document.reorder_track(0, 1)
	assert_true(ws.can_undo(), "bank reorder is undoable through the workspace")
	ws.undo()
	assert_false(ws.can_undo(), "undo consumed the only history entry")
	assert_true(ws.can_redo(), "redo is available after undo")

	ws.activate_workflow(2)  # LIVE exposes no undo target
	assert_false(ws.can_undo(), "Live mode exposes no undo")
