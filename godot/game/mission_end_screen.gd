class_name MissionEndScreen
extends Control

## The SP end-of-mission screen, at the witnessed shapes [orig: the round-end SP tail
## Server_ProcessRoundEnd @0x5164f0 -> Cinematic_EpilogUpdate @0x577950]:
## - WIN (winner 1): the epilog score screen — jo_Epil.tga backdrop + letterbox +
##   the Epilog/STREPILOG_* count lines fed by the kill-stat buckets
##   [orig: epilog_cinematic_state_machine_update @0x576240 case 4].
## - LOSE (anything else): the MISSION FAILED screen — jo_Epil2.tga backdrop +
##   Overlays/STROVER_MISSION_FAILED + the WAC Lose banner line + the key hint
##   [orig: the Cinematic_EpilogUpdate g_cine_mode==2 leg @0x5744fd..].
## Both exit on ESC or the 18600-tick (~300 s) timeout [orig: g_mission_exit_reason=1
## via Input_HandleSpecialKeys @0x49c8e2 (ESC 0x1B) / the state-machine timeout
## @0x57621d — the main loop then pushes the "Post Menu" scene @0x526867].
## Stand-ins (ledgered D-AI-10, docs/divergence-ledger.md): no flyaway cine /
## .cne playback, no score count-up
## animation, no end-music track switch; a timed fade-in stands in for the cine
## fade events. Witness record: docs/world/world-wac-ai-re.md §20.6.

signal exit_requested

const EXIT_TIMEOUT_S := 300.0 # [orig: 18600 ticks at 62 Hz @0x57621d/@0x5744ea]
const FADE_IN_S := 1.5        # stands in for the 48+48-tick cine fade pair [orig: @0x574512]

var _age := 0.0
var _built := false


func setup(outcome: Dictionary, banner: String, root) -> void:
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

	var won := int(outcome.get("winner_team", 0)) == 1
	_add_backdrop(root, "jo_Epil.tga" if won else "jo_Epil2.tga")

	var column := VBoxContainer.new()
	column.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	column.grow_horizontal = Control.GROW_DIRECTION_BOTH
	column.grow_vertical = Control.GROW_DIRECTION_BOTH
	column.alignment = BoxContainer.ALIGNMENT_CENTER
	column.add_theme_constant_override("separation", 14)
	add_child(column)

	if won:
		# The epilog score lines, in the witnessed order and count sources
		# [orig: @0x576240 case 4 — TEAMUNITS = by-player + by-others, enemy =
		# the summed buckets]. The objective-bonus counters are unmodeled (0).
		_add_line(column, _epilog("STREPILOG_OBJECTIVEBONUS"), "0", 28)
		var enemy := int(outcome.get("enemy_kills", 0)) + int(outcome.get("enemy_kills_by_others", 0))
		_add_line(column, _epilog("STREPILOG_ENEMYUNITS"), str(enemy), 28)
		var team := int(outcome.get("bluekills", 0)) + int(outcome.get("team_kills_by_others", 0))
		_add_line(column, _epilog("STREPILOG_TEAMUNITS"), str(team), 28)
		var friendly := int(outcome.get("greenkills", 0)) + int(outcome.get("friendly_kills_by_others", 0))
		_add_line(column, _epilog("STREPILOG_FRIENDLYUNITS"), str(friendly), 28)
	else:
		# MISSION FAILED + the WAC Lose cause [orig: Overlays/STROVER_MISSION_FAILED
		# at y=120, the g_banner_text line at y=230].
		_add_line(column,
				NovaStrings.lookup_display("gametext", "Overlays", "STROVER_MISSION_FAILED"),
				"", 40)
		if not banner.is_empty():
			_add_line(column, banner, "", 26)

	var hint := _epilog("STREPILOG_KEYINFO")
	_add_line(column, hint, "", 20)
	_built = true


func _epilog(key: String) -> String:
	return NovaStrings.lookup_display("gametext", "Epilog", key)


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
func _add_backdrop(root, image_name: String) -> void:
	if root == null or not root.has_method("read_file"):
		return
	var bytes: PackedByteArray = root.read_file(
			image_name, NovaResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)
	if bytes.is_empty():
		return
	var img := Image.new()
	if img.load_tga_from_buffer(bytes) != OK:
		return
	var tex := TextureRect.new()
	tex.texture = ImageTexture.create_from_image(img)
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
