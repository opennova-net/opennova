extends GutTest

const CreditsEditorScene = preload("res://modtools/credits/credits_editor.tscn")
const CreditsEditorBlockCardScene = preload("res://modtools/credits/credits_editor_block_card.tscn")
const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")
const CreditsWorkspaceScript = preload("res://modtools/credits/credits_workspace.gd")
const ResourceDirSettings = preload("res://engine/resource_index/resource_dir_settings.gd")
const KDA_PATH := "res://../fixtures/cbin/nlist.reference.kda"
const CREDITS_FIXTURE_DIR := "res://../fixtures/cbin"
const MINIMAL_SOURCE := "[ENV]\nscroll_rate=1.25\nvertical_space=18\ncenter_x=360\n\n[TEXT]\nApplied from source\n"
const MALFORMED_SOURCE := "[ENV]\nscroll_rate=1.25\n\n[TEXT]\n~Fbad\n"

var _saved_resource_dir := ""

class RefIndexStub:
	extends RefCounted

	func resolve(_kind: String, name: String) -> Dictionary:
		return {"status": "found", "path": "C:/res/%s.fnt" % name}


class RefShell:
	extends Node

	var index := RefIndexStub.new()
	var picked_kind := ""
	var jumped: Array = []

	func get_reference_index() -> RefIndexStub:
		return index

	func open_kind_picker(kind: String, _title: String, on_pick: Callable) -> void:
		picked_kind = kind
		# Picks a DIFFERENT font than the entry already has - a same-value pick
		# would early-return in _commit and make the commit assertion vacuous.
		on_pick.call("C:/res/Gunpl27b.fnt")

	func open_in_workspace(kind: String, path: String, _focus: FocusPayload = null) -> Error:
		jumped.append([kind, path])
		return OK


func before_all() -> void:
	_saved_resource_dir = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir("")


func after_all() -> void:
	ResourceDirSettings.set_resource_dir(_saved_resource_dir)


func test_block_card_scene_instantiates() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	assert_not_null(card, "block card scene must instantiate (parse + load)")
	add_child_autofree(card)

func test_editor_scene_instantiates() -> void:
	var editor = CreditsEditorScene.instantiate()
	assert_not_null(editor, "editor scene must instantiate")
	add_child_autofree(editor)
	await get_tree().process_frame
	# Process one frame so @onready bindings fire and any %-marker resolution surfaces.

func test_editor_panes_clip_preview_and_list_overflow() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var left_pane: Control = editor.get_node("HSplit/LeftPane")
	var right_pane: Control = editor.get_node("HSplit/RightPane")
	var preview_mount: Control = editor.get_node("%PreviewMount")
	var player: NovaCreditsPlayer = editor.get_node("%Player")

	assert_true(left_pane.clip_contents, "left editor pane clips card overflow at the split")
	assert_true(right_pane.clip_contents, "right preview pane clips preview overflow at the split")
	assert_true(preview_mount.clip_contents, "preview mount clips its contents")
	assert_true(player.clip_contents, "player clips generated fixed-image overlays")

func test_editor_binds_document_and_loads_kda() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame
	# Keep this fixture test deterministic: otherwise cards resolve image/font
	# names through the user's persisted resource directory, which can point at a
	# large real game install and make headless runs environment-dependent.
	editor.set_resource_root(null)

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	editor.set_document(doc)
	var err: int = doc.open_kda(KDA_PATH)
	assert_eq(err, OK, "open_kda returns OK for the KDA fixture")
	await get_tree().process_frame
	assert_gt(doc.resource.get_entry_count(), 0, "fixture has entries")


func test_credits_workspace_injects_font_link_services_on_mount() -> void:
	var workspace = autofree(CreditsWorkspaceScript.new())
	var shell: RefShell = add_child_autofree(RefShell.new())
	var mount: Control = add_child_autofree(Control.new())
	workspace.set_editor_shell(shell)
	workspace.mount_viewport(mount)
	await get_tree().process_frame

	var editor: Node = mount.get_child(0)
	editor.set_resource_root(null)
	var doc: Object = workspace.get_editor_document()
	var entry := CbinTextEntry.new()
	entry.set_text("Hello")
	entry.set_font_name("Serpen24")
	doc.resource.add_entry(entry)
	await get_tree().process_frame
	await get_tree().process_frame

	var font_ref: Node = editor.find_child("FontRef", true, false)
	assert_not_null(font_ref, "text cards carry a font link row")
	if font_ref == null:
		return
	assert_eq(font_ref.get_value(), "Serpen24", "the row reads the entry's font name")
	assert_true(font_ref.browse_button.visible,
		"the shell's pick service reaches the card (services threaded down the chain)")

	font_ref.browse_button.pressed.emit()
	assert_eq(shell.picked_kind, "font", "browsing routes through the shell kind picker")
	assert_eq(entry.get_font_name(), "Gunpl27b",
		"a pick commits the picked font's basename back onto the entry")

	assert_true(font_ref.jump_button.visible and not font_ref.jump_button.disabled,
		"a resolvable font offers the jump")
	font_ref.jump_button.pressed.emit()
	assert_eq(shell.jumped, [["font", "C:/res/Gunpl27b.fnt"]],
		"the jump rides open_in_workspace with the resolved path (relay chain retired)")


func test_block_card_binds_text_entry() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinTextEntry.new()
	entry.set_text("Hello")
	# Pass the justify value through an int variable — the GDScript parser
	# rejects CbinEntry.CBIN_JUSTIFY_CENTER as a direct argument to set_justify()
	# because the scoped enum type (CbinEntry.CbinJustify) doesn't match the
	# globally-cast type (godot.CbinJustify). Assigning to int first is the
	# workaround.
	var center: int = CbinEntry.CBIN_JUSTIFY_CENTER
	entry.set_justify(center)
	card.bind(entry)
	assert_eq(card.get_entry(), entry)


func test_block_card_font_row_binds_and_commits_font_name() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinTextEntry.new()
	entry.set_text("Hello")
	entry.set_font_name("Serpen24")
	card.bind(entry)
	await get_tree().process_frame

	var font_ref: Node = card.find_child("FontRef", true, false)
	assert_not_null(font_ref, "text cards carry a font link row")
	if font_ref == null:
		return
	assert_eq(font_ref.get_value(), "Serpen24", "binding reads the entry's font silently")

	font_ref.name_edit.text = "Gunpl27b"
	font_ref.name_edit.text_submitted.emit("Gunpl27b")
	assert_eq(entry.get_font_name(), "Gunpl27b", "a typed commit writes the font name")

	font_ref.clear_button.pressed.emit()
	assert_eq(entry.get_font_name(), "", "clearing falls back to the game's default font")


func test_block_card_binds_newline_as_compact_spacer() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	card.bind(CbinNewlineEntry.new())
	await get_tree().process_frame

	var type_chip: Label = card.get_node("%TypeChip")
	assert_eq(type_chip.text, "SPACE",
		"Newline cards should read as compact spacing controls.")
	assert_true(card.custom_minimum_size.y > 0.0 and card.custom_minimum_size.y <= 32.0,
		"Newline cards should use a compact row height.")

	var spacer_panel := card.get_node_or_null("Row/SpacerPanel") as Control
	assert_not_null(spacer_panel, "Newline cards should expose a spacer panel.")
	if spacer_panel == null:
		return
	assert_true(spacer_panel.visible,
		"Spacer panel should be visible for newline entries.")

func test_editor_chrome_uses_compact_command_and_preview_controls() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var toolbar := editor.get_node_or_null("%EditorToolbar") as PanelContainer
	assert_not_null(toolbar, "Credits editor should expose one defined top toolbar area.")
	if toolbar != null:
		assert_eq(toolbar.offset_right, 0.0,
			"credits toolbar should use the full preview width now that global buttons live in the shell top bar.")
		assert_true(toolbar.clip_contents,
			"credits toolbar should clip its own contents before they overflow the preview edge.")
		var margin := toolbar.get_node_or_null("ToolbarMargin") as MarginContainer
		assert_not_null(margin, "credits toolbar should have internal padding.")
		if margin != null:
			assert_gte(margin.get_theme_constant("margin_left"), 8,
				"toolbar controls should not touch the left border.")
			assert_gte(margin.get_theme_constant("margin_top"), 4,
				"toolbar controls should not touch the top border.")
		assert_not_null(toolbar.get_node_or_null("ToolbarMargin/ToolbarRow/ModeButtons/VisualButton"),
			"toolbar should contain the visual/source mode buttons.")
		assert_not_null(toolbar.get_node_or_null("ToolbarMargin/ToolbarRow/ScrollRateSpin"),
			"toolbar should contain scroll rate controls.")
		assert_not_null(toolbar.get_node_or_null("ToolbarMargin/ToolbarRow/Play"),
			"toolbar should contain preview playback controls.")
		assert_not_null(toolbar.get_node_or_null("ToolbarMargin/ToolbarRow/Speed"),
			"toolbar should contain preview speed controls.")

	var add_row_frame := editor.get_node("HSplit/LeftPane/ContentStack/BlockListMount/AddRowFrame") as PanelContainer
	assert_eq(add_row_frame.theme_type_variation, &"FlatPanel",
		"The add command strip should use the flat panel theme.")
	assert_null(editor.get_node_or_null("HSplit/RightPane/PreviewMount/Toolbar"),
		"preview controls should live in the editor toolbar instead of inside the preview pane.")

func test_missing_image_warning_is_not_shown_under_toolbar() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	var image := CbinImageEntry.new()
	image.set_texture_name("missing_credits_image.png")
	doc.resource.add_entry(image)
	editor.set_document(doc)
	await get_tree().process_frame

	var warning_bar := editor.get_node_or_null("%WarningBar") as Control
	assert_not_null(warning_bar, "warning bar node may remain for compatibility.")
	if warning_bar != null:
		assert_false(warning_bar.visible,
			"missing-image warning text should not appear under the credits toolbar.")

func test_block_card_missing_image_name_commits_on_focus_loss() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinImageEntry.new()
	card.bind(entry, ProjectSettings.globalize_path(CREDITS_FIXTURE_DIR))
	await get_tree().process_frame

	var path_edit: LineEdit = card.get_node("%ImagePathEdit")
	path_edit.text = "missing_credits_image.png"
	path_edit.focus_exited.emit()
	await get_tree().process_frame

	assert_eq(entry.get_texture_name(), "missing_credits_image.png", "missing filename is preserved for save/warnings")
	assert_null(entry.get_texture(), "missing image clears any stale texture")

func test_block_card_image_path_uses_case_and_extension_fallback() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinImageEntry.new()
	card.bind(entry, ProjectSettings.globalize_path(CREDITS_FIXTURE_DIR))
	await get_tree().process_frame

	var path_edit: LineEdit = card.get_node("%ImagePathEdit")
	path_edit.text = "CR1.PCX"
	path_edit.text_submitted.emit("CR1.PCX")
	await get_tree().process_frame

	assert_eq(entry.get_texture_name(), "CR1.PCX", "typed filename is preserved")
	assert_not_null(entry.get_texture(), "image editor resolves case/extension variants like the KDA loader")

func test_visual_source_buttons_are_exclusive() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var visual: Button = editor.get_node("%VisualButton")
	var source: Button = editor.get_node("%SourceButton")
	source.button_pressed = true
	await get_tree().process_frame

	assert_false(visual.button_pressed, "visual is unpressed when source is active")
	assert_true(source.button_pressed, "source is active")

	visual.button_pressed = true
	await get_tree().process_frame
	assert_true(visual.button_pressed, "visual is active")
	assert_false(source.button_pressed, "source is unpressed when visual is active")

func test_switching_source_to_visual_applies_valid_pending_source() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	editor.set_document(doc)
	await get_tree().process_frame

	var source: Button = editor.get_node("%SourceButton")
	source.button_pressed = true
	await get_tree().process_frame

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewMount/CodeEdit")
	code_edit.text = MINIMAL_SOURCE

	var visual: Button = editor.get_node("%VisualButton")
	visual.button_pressed = true
	await get_tree().process_frame

	assert_true(visual.button_pressed, "valid pending source should allow switching back to visual mode.")
	assert_false(source.button_pressed, "source mode should be inactive after valid source is applied.")
	assert_eq(doc.resource.get_entry_count(), 1, "valid source should replace entries before returning to visual mode.")
	if doc.resource.get_entry_count() != 1:
		return
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "Applied from source",
		"resource should contain the applied source text after mode switch.")


func test_switching_source_to_visual_with_parse_error_preserves_source_mode_and_resource() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	var entry := CbinTextEntry.new()
	entry.set_text("Keep me")
	doc.resource.add_entry(entry)
	editor.set_document(doc)
	await get_tree().process_frame

	var source: Button = editor.get_node("%SourceButton")
	source.button_pressed = true
	await get_tree().process_frame

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewMount/CodeEdit")
	code_edit.text = MALFORMED_SOURCE

	var visual: Button = editor.get_node("%VisualButton")
	visual.button_pressed = true
	await get_tree().process_frame

	assert_false(visual.button_pressed, "invalid pending source should block switching to visual mode.")
	assert_true(source.button_pressed, "source mode should stay active after parse failure.")
	assert_eq(doc.resource.get_entry_count(), 1, "failed mode switch should preserve existing resource entries.")
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "Keep me",
		"failed mode switch should preserve existing entry data.")

func test_preview_pause_resume_and_stop_show_first_entries() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	editor.set_size(Vector2(1280, 720))
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	var entry := CbinTextEntry.new()
	entry.set_text("Preview")
	doc.resource.add_entry(entry)
	editor.set_document(doc)
	await get_tree().process_frame
	await get_tree().process_frame

	var player: NovaCreditsPlayer = editor.get_node("%Player")
	var play: Button = editor.get_node("%Play")
	var pause: Button = editor.get_node("%Pause")
	var stop: Button = editor.get_node("%Stop")

	play.pressed.emit()
	await get_tree().process_frame
	assert_true(player.is_playing(), "play starts preview")

	pause.pressed.emit()
	await get_tree().process_frame
	assert_false(player.is_playing(), "pause stops active playback without resetting")
	assert_eq(play.text, "Resume", "play button becomes resume while paused")

	play.pressed.emit()
	await get_tree().process_frame
	assert_true(player.is_playing(), "resume continues playback")

	stop.pressed.emit()
	await get_tree().process_frame
	assert_false(player.is_playing(), "stop ends playback")
	assert_almost_eq(player.get_scroll_offset(), player.get_size().y, 1.0, "stop returns preview to the first entries")


func test_flush_pending_source_edits_applies_code_edit_to_resource() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	editor.set_document(doc)
	await get_tree().process_frame

	var source: Button = editor.get_node("%SourceButton")
	source.button_pressed = true
	await get_tree().process_frame

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewMount/CodeEdit")
	code_edit.text = MINIMAL_SOURCE

	var err: int = editor.flush_pending_edits()
	assert_eq(err, OK, "flush_pending_edits should apply valid source text.")
	assert_eq(doc.resource.get_entry_count(), 1, "valid source should replace entries.")
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "Applied from source",
		"resource should contain the pending source text.")
	assert_eq(doc.resource.get_vertical_space(), 18, "ENV values should be applied during source flush.")


func test_flush_pending_source_parse_error_preserves_resource() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	var entry := CbinTextEntry.new()
	entry.set_text("Keep me")
	doc.resource.add_entry(entry)
	editor.set_document(doc)
	await get_tree().process_frame

	var source: Button = editor.get_node("%SourceButton")
	source.button_pressed = true
	await get_tree().process_frame

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewMount/CodeEdit")
	code_edit.text = MALFORMED_SOURCE

	var err: int = editor.flush_pending_edits()
	assert_ne(err, OK, "flush_pending_edits should reject malformed source text.")
	assert_eq(doc.resource.get_entry_count(), 1, "failed source flush should preserve entries.")
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "Keep me",
		"failed source flush should preserve the original entry.")
