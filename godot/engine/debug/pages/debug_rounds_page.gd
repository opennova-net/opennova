class_name DebugRoundsPage
extends NovaDebugPage
## The round hit-test inspector: the RoundSim debug ring (libs/world
## round_sim.cpp), newest first — what each recently resolved round actually
## did (which entity, which COBJ section/face or reaction-bone/damage-zone
## pair, which material -> impact tag, husk state), with the face-miss fly-ons
## called out. Developer window into both retail narrow phases (item CFAC faces
## and person bone spheres), not a mimicked retail page. The world-geometry
## toggles ride the NovaDebugOptions registry: round trails, the hit meshes
## rounds actually test, and the object collision volumes + player capsule.

var _rnd_status_label: Label
var _rnd_list: ItemList


func page_id() -> StringName:
	return &"Rounds"


func page_title() -> String:
	return "Rounds & collision"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_rnd_status_label = Label.new()
	_rnd_status_label.name = "RoundsStatus"
	_rnd_status_label.text = "No rounds resolved yet."
	_rnd_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_rnd_status_label)

	_rnd_list = ItemList.new()
	_rnd_list.name = "RoundEvents"
	_rnd_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_rnd_list.focus_mode = Control.FOCUS_NONE
	add_child(_rnd_list)

	add_option_check(&"show_round_trails")
	add_option_check(&"show_hit_meshes")
	add_option_check(&"show_collision")


# Kind colors mirror RoundDebugView.kind_color so the list rows and the world
# markers read as one system.
static func _round_kind_color(kind: int) -> Color:
	match kind:
		0:
			return Color(0.2, 0.9, 1.0)    # organic
		1:
			return Color(0.3, 1.0, 0.45)   # item FACE hit
		2:
			return Color(1.0, 0.75, 0.2)   # item sphere stand-in
		3:
			return Color(0.75, 0.6, 0.4)   # terrain
		4:
			return Color(0.55, 0.55, 0.55) # expired
		5:
			return Color(1.0, 0.25, 0.2)   # face-miss fly-on
		_:
			return Color.MAGENTA


static func _round_bone_pair(ev: Dictionary) -> String:
	var primary := int(ev.get("section", -1))
	if bool(ev.get("fallback", false)):
		return "neutral fallback sphere  reaction stand-in %d" % primary
	var secondary := int(ev.get("secondary_section", -1))
	var secondary_text := "-" if secondary < 0 else str(secondary)
	return "reaction bone %d  damage zone %s" % [primary, secondary_text]


func refresh() -> void:
	var sim := _ctx.sim()
	if sim == null or not sim.has_method("get_round_debug"):
		_clear_pane("No round data.")
		return
	var debug: Dictionary = sim.get_round_debug()
	var events: Array = debug.get("events", [])
	if events.is_empty():
		_clear_pane("No rounds resolved yet.")
		return
	var face_hits := 0
	var person_hits := 0
	var person_fallbacks := 0
	var sphere_hits := 0
	var misses := 0
	for ev_v in events:
		match int((ev_v as Dictionary).get("kind", 4)):
			0:
				if bool((ev_v as Dictionary).get("fallback", false)):
					person_fallbacks += 1
				else:
					person_hits += 1
			1:
				face_hits += 1
			2:
				sphere_hits += 1
			5:
				misses += 1
	_rnd_status_label.text = "Last %d outcomes:  %d person bone hits   %d organic fallbacks   %d face hits   %d item sphere stand-ins   %d face-miss fly-ons" % [
		events.size(), person_hits, person_fallbacks, face_hits, sphere_hits, misses]

	_rnd_list.clear()
	# Newest first — the row you just shot is the row on top.
	for i in range(events.size() - 1, -1, -1):
		var ev: Dictionary = events[i]
		var kind := int(ev.get("kind", 4))
		var row := "t%d  %s" % [int(ev.get("tick", 0)), String(ev.get("kind_name", "?"))]
		var ent := int(ev.get("entity_handle", 0xFFFF))
		if ent != 0xFFFF:
			row += "  " + WireHandle.label(ent)
			var ent_name := String(ev.get("entity_name", ""))
			if not ent_name.is_empty():
				row += " " + ent_name
		if bool(ev.get("husk", false)):
			row += "  HUSK"
		match kind:
			0:
				row += "  %s  mat %d -> %s" % [_round_bone_pair(ev),
						int(ev.get("material", 0)),
						String(ev.get("effect_tag_name", ""))]
			1:
				row += "  sec %d face %d mat %d -> %s" % [int(ev.get("section", -1)),
						int(ev.get("face", -1)), int(ev.get("material", 0)),
						String(ev.get("effect_tag_name", ""))]
			2:
				row += "  -> %s" % String(ev.get("effect_tag_name", ""))
			5:
				row += "  graze t %.3f" % float(ev.get("t", 0.0))
			_:
				pass
		var idx := _rnd_list.add_item(row, null, false)
		_rnd_list.set_item_custom_fg_color(idx, _round_kind_color(kind))


func _clear_pane(message: String) -> void:
	_rnd_status_label.text = message
	if _rnd_list.item_count > 0:
		_rnd_list.clear()
