extends GutTest

const CreditsEditorScene = preload("res://modtools/credits/credits_editor.tscn")
const CreditsEditorBlockCardScene = preload("res://modtools/credits/credits_editor_block_card.tscn")
const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")
const CreditsWorkspaceScript = preload("res://modtools/editor/credits_workspace.gd")
const KDA_PATH := "res://../fixtures/cbin/nlist.reference.kda"
const CREDITS_FIXTURE_DIR := "res://../fixtures/cbin"
const MINIMAL_SOURCE := "[ENV]\nscroll_rate=1.25\nvertical_space=18\ncenter_x=360\n\n[TEXT]\nApplied from source\n"
const MALFORMED_SOURCE := "[ENV]\nscroll_rate=1.25\n\n[TEXT]\n~Fbad\n"

class FontOpenShell:
	extends Node

	var requested_font_name := ""

	func open_font_workspace(font_name: String) -> Error:
		requested_font_name = font_name
		return OK


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
	var preview_host: Control = editor.get_node("%PreviewHost")
	var player: NovaCreditsPlayer = editor.get_node("%Player")

	assert_true(left_pane.clip_contents, "left editor pane clips card overflow at the split")
	assert_true(right_pane.clip_contents, "right preview pane clips preview overflow at the split")
	assert_true(preview_host.clip_contents, "preview host clips its contents")
	assert_true(player.clip_contents, "player clips generated fixed-image overlays")

func test_editor_binds_document_and_loads_kda() -> void:
	var editor = CreditsEditorScene.instantiate()
	add_child_autofree(editor)
	await get_tree().process_frame

	var doc: CreditsEditorDocument = autofree(CreditsEditorDocument.new())
	editor.set_document(doc)
	var err: int = doc.open_kda(KDA_PATH)
	assert_eq(err, OK, "open_kda returns OK for the KDA fixture")
	await get_tree().process_frame
	assert_gt(doc.resource.get_entry_count(), 0, "fixture has entries")


func test_credits_workspace_forwards_font_edit_requests_to_editor_shell() -> void:
	var workspace = autofree(CreditsWorkspaceScript.new())
	var shell: FontOpenShell = add_child_autofree(FontOpenShell.new())
	var host: Control = add_child_autofree(Control.new())
	workspace.set_editor_shell(shell)
	workspace.mount_viewport(host)
	await get_tree().process_frame

	var editor: Node = host.get_child(0)
	editor.request_edit_font.emit("Serpen24")

	assert_eq(shell.requested_font_name, "Serpen24",
		"Credits workspace should forward edit-font requests into the Fonts workspace shell hook.")


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
	card.bind(entry, PackedStringArray())
	assert_eq(card.get_entry(), entry)


func test_block_card_requests_edit_for_selected_text_font() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	var entry := CbinTextEntry.new()
	entry.set_text("Hello")
	entry.set_font_name("Serpen24")
	card.bind(entry, PackedStringArray(["Serpen24"]))
	await get_tree().process_frame

	var requested := {"font_name": ""}
	card.connect("request_edit_font", func(font_name: String) -> void:
		requested["font_name"] = font_name
	)
	var edit_button: Button = card.get_node("%FontEditButton")
	assert_false(edit_button.disabled, "Text cards with a selected font should enable font editing.")
	edit_button.emit_signal("pressed")

	assert_eq(String(requested["font_name"]), "Serpen24", "Font edit action should emit the selected Nova font name.")


func test_block_card_binds_newline_as_compact_spacer() -> void:
	var card = CreditsEditorBlockCardScene.instantiate()
	add_child_autofree(card)
	await get_tree().process_frame

	card.bind(CbinNewlineEntry.new(), PackedStringArray())
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
		assert_lte(toolbar.offset_right, -128.0,
			"credits toolbar should reserve right-side room for global viewport buttons.")
		assert_true(toolbar.clip_contents,
			"credits toolbar should clip its own contents before they reach global viewport buttons.")
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

	var add_row_frame := editor.get_node("HSplit/LeftPane/ContentStack/BlockListHost/AddRowFrame") as PanelContainer
	assert_eq(add_row_frame.theme_type_variation, &"FlatPanel",
		"The add command strip should use the flat panel theme.")
	assert_null(editor.get_node_or_null("HSplit/RightPane/PreviewHost/Toolbar"),
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
	card.bind(entry, PackedStringArray(), ProjectSettings.globalize_path(CREDITS_FIXTURE_DIR))
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
	card.bind(entry, PackedStringArray(), ProjectSettings.globalize_path(CREDITS_FIXTURE_DIR))
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

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewHost/CodeEdit")
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

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewHost/CodeEdit")
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

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewHost/CodeEdit")
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

	var code_edit: CodeEdit = editor.get_node("HSplit/LeftPane/ContentStack/SourceViewHost/CodeEdit")
	code_edit.text = MALFORMED_SOURCE

	var err: int = editor.flush_pending_edits()
	assert_ne(err, OK, "flush_pending_edits should reject malformed source text.")
	assert_eq(doc.resource.get_entry_count(), 1, "failed source flush should preserve entries.")
	assert_eq((doc.resource.get_entry(0) as CbinTextEntry).get_text(), "Keep me",
		"failed source flush should preserve the original entry.")
