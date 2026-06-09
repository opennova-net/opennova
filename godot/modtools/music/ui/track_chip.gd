class_name MusicTrackChip
extends HBoxContainer

# A compact chip for one `play <track>` in a section: a preview button, the
# resolved bank-entry name, and a "wait" marker for playw. Shown inside the
# section map's GraphNodes and the per-state inspector's play list.
#
# Read-only by default (preview only). The inspector turns on the edit
# affordances (remove / toggle-wait) once the structured-edit parity gate is
# green; until then editable stays false and only preview is wired.

signal preview_requested(track_index: int)
signal remove_requested(track_index: int)

var _track_index: int = -1
var _wait: bool = false
var _editable: bool = false


# track_index: SBF bank entry index from the play operand.
# display_name: resolved bank-entry name (or "sound_N" when no bank / out of range).
# wait: true for playw (the script blocks on the sound), false for play.
# editable: when true, show remove + wait-toggle affordances (gated by the
#   document's can_edit_plays parity check; the map view passes false).
func setup(track_index: int, display_name: String, wait: bool, editable: bool = false) -> void:
	_track_index = track_index
	_wait = wait
	_editable = editable
	for c in get_children():
		c.queue_free()

	var play := Button.new()
	play.text = "▶"
	play.flat = true
	play.tooltip_text = "Preview this sound"
	play.pressed.connect(func(): preview_requested.emit(_track_index))
	add_child(play)

	var name_label := Label.new()
	name_label.text = display_name
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_label.clip_text = true
	# Trim long names with an ellipsis (not a hard cut to "soun") and keep the
	# full name reachable on hover. The right inspector shows the untrimmed name.
	name_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	name_label.tooltip_text = display_name
	add_child(name_label)

	if _wait:
		var wait_label := Label.new()
		wait_label.text = "wait"
		wait_label.add_theme_color_override("font_color", Color(0.7, 0.7, 0.7))
		wait_label.tooltip_text = "playw: the script waits for this sound before continuing."
		add_child(wait_label)
	if _editable:
		var del := Button.new()
		del.text = "✕"
		del.flat = true
		del.tooltip_text = "Remove this play from the section"
		del.pressed.connect(func(): remove_requested.emit(_track_index))
		add_child(del)


func track_index() -> int:
	return _track_index
