class_name MusicInspectorPanel
extends VBoxContainer

# Per-state inspector for the section map's right dock. Given one section from
# the read-only structural model, it shows the state's name + entry/idle badges
# + a "came from" breadcrumb, the full list of tracks it plays (preview chips),
# and its outgoing edges (transition / switch / branch) each with a Go button to
# jump there. Read-only: the play list turns editable only behind the
# structured-edit parity gate (a later step).

const MusicTrackChipClass = preload("res://modtools/music/ui/track_chip.gd")
const MusicSectionGraphClass = preload("res://modtools/music/music_section_graph.gd")

signal preview_requested(track_index: int)
signal jump_requested(section_name: StringName)
signal advanced_requested(section_name: StringName)
signal add_play_requested(section_name: StringName, track_index: int)
signal remove_play_requested(section_name: StringName, track_index: int)

var _section_name: String = ""
var _editable: bool = false


func _ready() -> void:
	clear()


func clear() -> void:
	_section_name = ""
	for c in get_children():
		c.queue_free()
	var hint := Label.new()
	hint.text = "Select a state on the map to inspect it."
	hint.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(hint)


func show_section(section: Dictionary, bank_names: Array, came_from: String = "", editable: bool = false) -> void:
	_section_name = String(section.get("name", ""))
	_editable = editable
	for c in get_children():
		c.queue_free()

	var header := Label.new()
	var badges := ""
	if bool(section.get("is_entry", false)):
		badges += "   ★ start"
	if bool(section.get("is_idle_loop", false)):
		badges += "   ↻ idle"
	header.text = "%s   [state %d]%s" % [_section_name, int(section.get("index", -1)), badges]
	header.add_theme_color_override("font_color", Color(0.85, 0.92, 1.0))
	add_child(header)

	if came_from != "":
		var crumb := Label.new()
		crumb.text = "came from: %s" % came_from
		crumb.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
		add_child(crumb)

	var plays_header := Label.new()
	plays_header.text = "Plays"
	add_child(plays_header)
	var plays: Array = section.get("plays", [])
	if plays.is_empty():
		var none := Label.new()
		none.text = "  (this state plays nothing on its own)"
		none.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
		add_child(none)
	else:
		for play in plays:
			var track: int = int(play.get("track", -1))
			var chip := MusicTrackChipClass.new()
			add_child(chip)
			chip.setup(track, _track_name(bank_names, track), bool(play.get("wait", false)), _editable)
			chip.preview_requested.connect(func(t): preview_requested.emit(t))
			chip.remove_requested.connect(func(t): remove_play_requested.emit(StringName(_section_name), t))
	if _editable:
		var drop_hint := Label.new()
		drop_hint.text = "＋ drag a track here to add a play"
		drop_hint.add_theme_color_override("font_color", Color(0.5, 0.7, 0.5))
		drop_hint.tooltip_text = "Drop a track from the Tracks dock to add a play to this state."
		add_child(drop_hint)

	var edges: Array = section.get("edges", [])
	var real_edges: Array = []
	for e in edges:
		if String(e.get("to_name", "")) != _section_name:
			real_edges.append(e)
	if not real_edges.is_empty():
		var edges_header := Label.new()
		edges_header.text = "Goes to"
		add_child(edges_header)
		for e in real_edges:
			var to_name := String(e.get("to_name", ""))
			var row := HBoxContainer.new()
			var lbl := Label.new()
			lbl.text = "%s %s" % [MusicSectionGraphClass.edge_glyph(int(e.get("kind", 0))), to_name]
			lbl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.add_child(lbl)
			var go := Button.new()
			go.text = "Go"
			go.tooltip_text = "Jump the running script to %s" % to_name
			go.pressed.connect(func(): jump_requested.emit(StringName(to_name)))
			row.add_child(go)
			add_child(row)

	var adv := Button.new()
	adv.text = "Show raw script for this state"
	adv.tooltip_text = "Open the Advanced drawer at this state's faithful decompiled text."
	adv.pressed.connect(func(): advanced_requested.emit(StringName(_section_name)))
	add_child(adv)


func current_section() -> String:
	return _section_name


# Drop sink for the Tracks dock: accept a {kind:"mus_track", index} payload and
# ask to add a play to the shown state. Only while editable (the structured-edit
# parity gate is open) and a state is shown.
func _can_drop_data(_at_position: Vector2, data: Variant) -> bool:
	return _editable and _section_name != "" \
		and data is Dictionary and String((data as Dictionary).get("kind", "")) == "mus_track"


func _drop_data(_at_position: Vector2, data: Variant) -> void:
	if not (data is Dictionary):
		return
	var track := int((data as Dictionary).get("index", -1))
	if track >= 0:
		add_play_requested.emit(StringName(_section_name), track)


func _track_name(names: Array, track: int) -> String:
	if track >= 0 and track < names.size() and String(names[track]) != "":
		return String(names[track])
	return "sound_%d" % track
