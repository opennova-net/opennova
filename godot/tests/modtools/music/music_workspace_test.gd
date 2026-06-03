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
	# Direction B: the three Bank/Script/Live tabs collapsed into one unified
	# screen, so the workspace advertises a single "Map" workflow.
	var ws = MusicWorkspaceAdapter.new()
	var workflows: Array = ws.get_workflows()
	assert_eq(workflows.size(), 1, "single unified Map workflow")
	# Workflows are typed InspectorDef rows (the shell does `entry as InspectorDef`).
	assert_eq(workflows[0].label, "Map")


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


func test_workflow_panels_coexist_on_one_screen():
	# Direction B: Bank / Script / Live are coexisting docks on one screen, not
	# mutually-exclusive tabs. After mount all three panels are present (Tracks
	# and the Advanced script panel are nested inside the embedded Live screen),
	# and activating the single workflow toggles nothing.
	var ws = MusicWorkspaceAdapter.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.mount_viewport(host)
	var root: Control = host.get_child(0)
	assert_not_null(root.find_child("Bank", true, false), "Tracks (Bank) dock present")
	assert_not_null(root.find_child("Script", true, false), "Advanced script (Script) panel present")
	assert_not_null(root.find_child("Live", true, false), "Live screen present")
	ws.activate_workflow(0)  # MAP -- no-op on the layout
	assert_true(root.find_child("Live", true, false).visible, "the unified screen stays visible")


func test_undo_hooks_surface_bank_history():
	# The workspace implements the framework can_undo/undo/redo hooks (like Mission
	# and MNU) by routing to the document's edit history. One unified screen means
	# one consolidated history reachable regardless of focus (no per-tab gating):
	# a fresh document has nothing to undo; a bank reorder becomes undoable and
	# redoable through the shell.
	var ws = MusicWorkspaceAdapter.new()
	assert_false(ws.can_undo(), "fresh document: nothing to undo")
	assert_false(ws.can_redo(), "fresh document: nothing to redo")

	assert_eq(ws.open_file(BANK_FIXTURE), OK, "bank opens")
	var before: String = String(ws._document.bank.get_entries()[0]["name"])
	ws._document.reorder_track(0, 1)
	assert_ne(String(ws._document.bank.get_entries()[0]["name"]), before, "reorder actually changed the order")
	assert_true(ws.can_undo(), "bank reorder is undoable through the workspace")
	ws.undo()
	# Assert the EFFECT, not just the history flag: the order is restored.
	assert_eq(String(ws._document.bank.get_entries()[0]["name"]), before, "workspace undo restored the track order")
	assert_false(ws.can_undo(), "undo consumed the only history entry")
	assert_true(ws.can_redo(), "redo is available after undo")


func test_bind_document_fans_out_to_panels():
	# Direction-B wiring: the root fans the shared document to every embedded
	# panel via find_child. Opening a bank must populate the Tracks (Bank) dock.
	var ws = MusicWorkspaceAdapter.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.mount_viewport(host)
	assert_eq(ws.open_file(BANK_FIXTURE), OK, "bank opens")
	await get_tree().process_frame
	var root: Control = host.get_child(0)
	var bank: Control = root.find_child("Bank", true, false)
	assert_not_null(bank, "Tracks (Bank) dock present")
	var tree: Tree = bank.get_node("%TrackTree")
	assert_not_null(tree.get_root(), "bank tree built from the fanned-out document")
	assert_gt(tree.get_root().get_child_count(), 0, "Tracks dock shows the bank's tracks")
