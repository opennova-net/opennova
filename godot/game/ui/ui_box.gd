class_name UiBox
extends RefCounted
# Lives in engine/ui (not modtools): shell-neutral by contract, so both the
# game's debug overlay and editor inspectors can use it.
# The margin + scroll + VBox column every inspector pane starts from. Extracted
# from the ~95%-identical copies in InspectorForms and MnuUiHelpers (which now
# delegate here); the only divergence was the box's node name, kept as a
# parameter because canvas code addresses it by path. Row builders deliberately
# stay per-domain: object's are read-only inspection rows, mnu's editable form
# rows — different semantics, not duplicates.

const PANEL_MARGIN := 10


static func make_inspector_box(mount: Control, box_name: String = "") -> VBoxContainer:
	var margin := MarginContainer.new()
	for side in ["margin_left", "margin_top", "margin_right", "margin_bottom"]:
		margin.add_theme_constant_override(side, PANEL_MARGIN)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	mount.add_child(margin)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(scroll)

	var box := VBoxContainer.new()
	if not box_name.is_empty():
		box.name = box_name
	box.add_theme_constant_override("separation", 8)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(box)
	return box
