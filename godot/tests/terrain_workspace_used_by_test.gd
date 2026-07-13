extends GutTest

# The terrain workspace's editor-feature wiring: the asset dock's "Used by"
# strip (which missions sit on this terrain, riding the shell's reference index
# through the workspace's service injection — the fonts/strings adopters'
# pattern), the View grid/axes guides (the grid toggle drives the
# surface-following sector overlay; a flat y=0 grid would be buried under
# sculpted heights), and the See-in-game launch note (TER-1: terrain reaches
# the game through Export into the launch dir). Pins the lazy-scan contract
# (no query until the explicit ask), the key spellings, the mission jump, and
# the shell-less fallback.

const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")
const TerrainWorkspaceScript = preload("res://modtools/terrain/terrain_workspace.gd")


class UsedByShell:
	extends Node

	class IndexStub:
		extends RefCounted
		var queries: Array = []
		func referrers_of(name: String) -> Array:
			queries.append(name)
			return [{"source_path": "mis01.bms", "source_kind": "mission",
				"site": "header.terrain", "target_kind": "terrain"}]
		func is_built() -> bool:
			return false

	var index := IndexStub.new()
	# A real root (ReferenceStrip.resolve_source_path probes it) over a
	# cache-dir fixture holding the referrer file the index stub reports.
	var root: NovaResourceRoot
	var opened: Array = []

	func get_reference_index() -> IndexStub:
		return index

	func get_resource_root() -> NovaResourceRoot:
		return root

	func open_in_workspace(kind: String, path: String, _focus: FocusPayload = null) -> Error:
		opened.append([kind, path])
		return OK


func _fixture_trn() -> String:
	return ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5/Dvxi5.trn")


func test_asset_dock_offers_used_by_without_triggering_index_build() -> void:
	var root_dir := OS.get_cache_dir().path_join("opennova_test_trn_used_by")
	DirAccess.make_dir_recursive_absolute(root_dir)
	var ref_file := FileAccess.open(root_dir.path_join("mis01.bms"), FileAccess.WRITE)
	ref_file.store_string("existence is what resolve_file probes")
	ref_file.close()

	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame
	assert_eq(editor.open_trn(_fixture_trn()), OK, "the dvxi5 fixture opens")

	var workspace = autofree(TerrainWorkspaceScript.new(editor))
	var shell: UsedByShell = add_child_autofree(UsedByShell.new())
	shell.root = NovaResourceRoot.new()
	assert_eq(shell.root.set_root_dir(root_dir), OK)
	workspace.set_editor_shell(shell)

	var host := Control.new()
	add_child_autofree(host)
	workspace.set_asset_dock(host)
	await get_tree().process_frame

	var dock = host.get_node_or_null("TerrainAssetDock")
	assert_not_null(dock, "the workspace builds its dock into the host")
	if dock == null:
		return
	var strip = dock.find_child("TerrainUsedByStrip", true, false)
	assert_not_null(strip, "with a shell the Properties tab mounts the Used-by strip")
	if strip == null:
		return
	assert_eq(shell.index.queries, [],
		"opening a terrain must never trigger the whole-root reference scan")
	assert_true(strip.find_button.visible, "the strip offers the explicit Find-uses ask instead")

	strip.find_button.pressed.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	var terrain_name: String = editor.get_terrain_name_value()
	assert_false(terrain_name.is_empty(), "the opened fixture carries a terrain name")
	assert_eq(shell.index.queries, [terrain_name, terrain_name + ".trn"],
		"the explicit ask queries both spellings (missions reference the header name bare)")

	# A referrer row click resolves the VFS-logical source name to a real path
	# before jumping — missions open from disk.
	var rows: Array = strip.rows.get_children().filter(func(c): return not c.is_queued_for_deletion())
	assert_eq(rows.size(), 1, "the stub's one referrer renders one row")
	if rows.size() == 1:
		(rows[0] as Button).pressed.emit()
		assert_eq(shell.opened.size(), 1, "the row click jumps once")
		assert_eq(String(shell.opened[0][0]), "mission")
		var opened_path := String(shell.opened[0][1])
		assert_true(FileAccess.file_exists(opened_path),
			"the jump carries the resolved disk path, not the bare logical name")

	workspace.set_asset_dock(null)
	DirAccess.remove_absolute(root_dir.path_join("mis01.bms"))
	DirAccess.remove_absolute(root_dir)


func test_asset_dock_without_shell_mounts_no_strip() -> void:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame

	var workspace = autofree(TerrainWorkspaceScript.new(editor))
	var host := Control.new()
	add_child_autofree(host)
	workspace.set_asset_dock(host)
	await get_tree().process_frame

	var dock = host.get_node_or_null("TerrainAssetDock")
	assert_not_null(dock, "the dock itself builds with or without a shell")
	if dock != null:
		assert_null(dock.find_child("TerrainUsedByStrip", true, false),
			"headless host: no shell, no reference index, no strip")
	workspace.set_asset_dock(null)


# Light stand-in for the note hook: the workspace only asks the editor for the
# terrain name and the last export directory.
class LaunchNoteEditorStub:
	extends Node
	var terrain_name := "dvxi5"
	var export_dir := ""

	func get_terrain_name_value() -> String:
		return terrain_name

	func get_last_export_dir() -> String:
		return export_dir


func test_game_launch_note_reports_export_staging() -> void:
	var stub: LaunchNoteEditorStub = autofree(LaunchNoteEditorStub.new())
	var workspace = autofree(TerrainWorkspaceScript.new(stub))

	var unstaged = workspace.get_game_launch_note("C:/games/jo")
	assert_not_null(unstaged, "terrain always speaks to the See-in-game gesture")
	if unstaged == null:
		return
	assert_false(unstaged.staged, "never exported = the authored terrain is not in the game yet")
	assert_string_contains(unstaged.detail, "Export your terrain",
		"the pointer says what to do first, in artist terms")

	stub.export_dir = "C:\\Games\\JO\\"
	var staged = workspace.get_game_launch_note("C:/games/jo")
	assert_true(staged.staged,
		"an export into the launch dir stages the terrain (separator- and case-insensitive)")
	assert_string_contains(staged.detail, "dvxi5", "the pointer names the exported terrain")
	assert_string_contains(staged.detail, "mission", "the gesture points at the mission list")

	stub.export_dir = "C:/games/jo/exports"
	var elsewhere = workspace.get_game_launch_note("C:/games/jo")
	assert_false(elsewhere.staged,
		"a subfolder export is NOT staged — the loose-file override never scans subfolders")

	var unbound = autofree(TerrainWorkspaceScript.new(null))
	assert_null(unbound.get_game_launch_note("C:/games/jo"),
		"no editor bound = no note (the launcher keeps its generic copy)")


func test_view_guides_drive_grid_guide_and_axes() -> void:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame

	var workspace = autofree(TerrainWorkspaceScript.new(editor))
	assert_true(workspace.shows_view_guides(), "terrain offers the View grid/axes toggles")

	workspace.set_grid_visible(true)
	assert_true(editor.is_grid_guide_visible(),
		"the grid toggle draws the neutral surface-following sector-line guide")
	assert_false(editor.is_sector_overlay_visible(),
		"...and NEVER the Layout workflow's colored sector diagnostic " +
		"(the shell pushes Show-grid on every activation - mapping it to the " +
		"overlay washed the whole map in sector tints)")
	workspace.set_grid_visible(false)
	assert_false(editor.is_grid_guide_visible())

	workspace.set_axes_visible(true)
	assert_true(editor.is_axes_visible(), "the axes toggle shows the origin gizmo")
	workspace.set_axes_visible(false)
	assert_false(editor.is_axes_visible())
