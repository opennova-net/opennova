extends GutTest

## Shell-level coverage for the generic cross-workspace jump
## (EditorWorkstation.open_in_workspace) and the EditorWorkspace.focus_reference
## hook it forwards to. The jump resolves the target workspace by its declared
## open-resource kind — never by type — and a same-path jump is focus-only so it
## cannot drop unsaved edits.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")
const STRINGS_FIXTURE := "res://fixtures/strings/menu.bin"


func _make_mount() -> Node:
	var mount: Node = EditorWorkstationScene.instantiate()
	add_child_autofree(mount)
	return mount


func _strings_workspace(mount: Node) -> EditorWorkspace:
	return mount._get_workspace(EditorWorkstationScript.Workspace.STRINGS)


func test_unknown_kind_is_unavailable() -> void:
	var mount := _make_mount()
	assert_eq(mount.open_in_workspace("no-such-kind", "whatever.bin"), ERR_UNAVAILABLE,
		"a kind no workspace declares should be unavailable")


func test_empty_path_is_invalid() -> void:
	var mount := _make_mount()
	assert_eq(mount.open_in_workspace("strings", "   "), ERR_INVALID_PARAMETER,
		"a blank path should be rejected before any open")


func test_jump_opens_activates_and_focuses() -> void:
	var mount := _make_mount()
	var err: Error = mount.open_in_workspace("strings", STRINGS_FIXTURE, FocusPayload.for_key("BTN_NEW_GAME"))
	assert_eq(err, OK, "the strings fixture should open through the kind-resolved workspace")
	assert_eq(mount.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS,
		"the jump should activate the Strings workspace")
	var doc: StringsEditor = _strings_workspace(mount).get_document()
	assert_eq(doc.current_path, STRINGS_FIXTURE, "the table should be the open document")
	var expected := doc.string_table.find_entry_by_key("BTN_NEW_GAME")
	assert_true(expected >= 0, "the fixture should contain the focused key")
	assert_eq(doc.selected_index, expected, "focus_reference should select the key's entry")


func test_same_path_jump_is_focus_only() -> void:
	var mount := _make_mount()
	assert_eq(mount.open_in_workspace("strings", STRINGS_FIXTURE), OK)
	var doc: StringsEditor = _strings_workspace(mount).get_document()
	doc.add_entry("OPEN_IN_WS_TMP", "tmp", 0, Vector2i())
	assert_true(doc.is_dirty, "the added entry should dirty the table")
	assert_eq(mount.open_in_workspace("strings", STRINGS_FIXTURE, FocusPayload.for_key("BTN_NEW_GAME")), OK)
	assert_true(doc.is_dirty,
		"a same-path jump must be focus-only, not a destructive reopen")


func test_open_strings_workspace_forwarder_keeps_contract() -> void:
	var mount := _make_mount()
	assert_eq(mount.open_strings_workspace(STRINGS_FIXTURE, "BTN_NEW_GAME"), OK,
		"the legacy strings cross-jump should ride open_in_workspace unchanged")
	assert_eq(mount.get_active_workspace_id(), EditorWorkstationScript.Workspace.STRINGS)


func test_focus_reference_default_is_ok() -> void:
	var workspace := EditorWorkspace.new()
	assert_eq(workspace.focus_reference(FocusPayload.for_key("x")), OK,
		"the base hook accepts any payload and focuses nothing")
