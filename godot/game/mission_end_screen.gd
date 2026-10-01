class_name MissionEndScreen
extends Control

## The SP end-of-mission screen, at the witnessed shapes [orig: the round-end SP tail
## Server_ProcessRoundEnd @0x5164f0 -> Cinematic_EpilogUpdate @0x577950]:
## - WIN (winner 1): the epilog score screen — jo_Epil.tga backdrop + letterbox +
##   the four Epilog/STREPILOG_* counter lines, label + value as the engine
##   composes them (hud::epilog_score_lines over the SP score block)
##   [orig: Cine_EpilogStateMachineUpdate @0x576240 case 4].
## - LOSE (anything else): the MISSION FAILED screen — jo_Epil2.tga backdrop +
##   Overlays/STROVER_MISSION_FAILED + the WAC Lose banner line + the key hint
##   [orig: the Cinematic_EpilogUpdate g_CineMode==2 leg @0x5744fd..].
## Both exit on ESC or the 18600-tick (~300 s) timeout [orig: g_MissionExitReason=1
## via Input_HandleSpecialKeys @0x49c8e2 (ESC 0x1B) / the state-machine timeout
## @0x57621d — the main loop then pushes the "Post Menu" scene @0x526867].
## Stand-ins (ledgered D-AI-10, docs/divergence-ledger.md): no flyaway cine /
## .cne playback and a stacked line layout in place of the counters' witnessed
## columns; the screen alpha ramps over the cine fade pair's tick span. Witness
## record: docs/world/world-wac-ai-re.md §20.6.

signal exit_requested

# The ESC-less exit timeout: engine truth 297.6 s — 18600 ticks of the 62.5 Hz
# loop (world/world.h kEpilogExitTimeoutTicks carries the
# [orig: @0x57621d/@0x5744ea] witness). Deliberate correction: the old
# godot-side 300.0 assumed a 62 Hz tick; the engine value is adopted.
static var EXIT_TIMEOUT_S: float = Simulation.epilog_exit_timeout_seconds()
# The fade-in: engine truth 1.536 s, the 48+48-tick cine fade pair on the
# 62.5 Hz loop (world/world.h kEpilogFadeInTicks carries the witness).
static var FADE_IN_S: float = Simulation.epilog_fade_in_seconds()

var _age := 0.0
var _built := false


## `score` is the SP score block's epilog lines (Simulation.get_epilog_score, or
## EndRoundStatistics.make_epilog for a sim-less mount); only the WIN form reads it.
func setup(outcome: RoundOutcome, score: EndRoundStatistics, banner: String,
		root: ResourceRoot) -> void:
	# Full-rect dark backdrop + letterbox bars [orig: CCineEventLetterbox].
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	modulate.a = 0.0
	var dim := ColorRect.new()
	dim.color = Color(0.0, 0.0, 0.0, 0.62)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(dim)
	for top in [true, false]:
		var bar := ColorRect.new()
		bar.color = Color.BLACK
		bar.set_anchors_and_offsets_preset(
				Control.PRESET_TOP_WIDE if top else Control.PRESET_BOTTOM_WIDE)
		bar.custom_minimum_size = Vector2(0, 64)
		add_child(bar)

	var won := outcome.winner_team == 1
	_add_backdrop(root, "jo_Epil.tga" if won else "jo_Epil2.tga")

	var column := VBoxContainer.new()
	column.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	column.grow_horizontal = Control.GROW_DIRECTION_BOTH
	column.grow_vertical = Control.GROW_DIRECTION_BOTH
	column.alignment = BoxContainer.ALIGNMENT_CENTER
	column.add_theme_constant_override("separation", 14)
	add_child(column)

	if won:
		# The four counter lines in the witnessed order, each label with its
		# engine-formatted value (empty when retail draws none).
		var keys := score.label_keys if score != null else PackedStringArray()
		var values := score.values if score != null else PackedStringArray()
		for i in keys.size():
			_add_line(column, _epilog(keys[i]), values[i], 28)
	else:
		# MISSION FAILED + the WAC Lose cause [orig: Overlays/STROVER_MISSION_FAILED
		# at y=120, the g_BannerText line at y=230].
		_add_line(column,
				Strings.lookup_display(Strings.TABLE_GAMETEXT, Strings.SECTION_OVERLAYS, "STROVER_MISSION_FAILED"),
				"", 40)
		if not banner.is_empty():
			_add_line(column, banner, "", 26)

	var hint := _epilog("STREPILOG_KEYINFO")
	_add_line(column, hint, "", 20)
	_built = true


func _epilog(key: String) -> String:
	return Strings.lookup_display(Strings.TABLE_GAMETEXT, "Epilog", key)


func _add_line(column: VBoxContainer, text: String, value: String, size: int) -> void:
	var label := Label.new()
	label.text = text if value.is_empty() else "%s  %s" % [text, value]
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_size_override("font_size", size)
	column.add_child(label)


# The witnessed backdrop art, from the mounted resource root. Retail UI images
# force loose-first for this lookup; a missing/unparsable image degrades to the
# plain dim, never a load failure.
# [orig: CUIImage_LoadTextureFromFile @ 0x6541ba]
func _add_backdrop(root: ResourceRoot, image_name: String) -> void:
	var backdrop := TgaTexture.load_from_root(root, image_name, true)
	if backdrop == null:
		return
	var tex := TextureRect.new()
	tex.texture = backdrop
	tex.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	tex.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	tex.modulate.a = 0.85
	add_child(tex)
	move_child(tex, 1) # over the dim, under the letterbox/text


func _process(delta: float) -> void:
	if not _built:
		return
	_age += delta
	modulate.a = clampf(_age / FADE_IN_S, 0.0, 1.0)
	if _age >= EXIT_TIMEOUT_S:
		_built = false # one-shot
		exit_requested.emit()


## The shell routes ESC here while the screen is up [orig: ESC -> reason 1 @0x49c8e2].
func request_exit() -> void:
	if not _built:
		return
	_built = false
	exit_requested.emit()
