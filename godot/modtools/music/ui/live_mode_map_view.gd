extends "res://modtools/music/ui/live_mode_section.gd"

# MusicLiveMode's section-map view cluster (W4-6a): map rebuild, layered
# layout + fit, logic badges and edge colours, active-node highlight,
# track-chip preview, and the map-node input/selection handlers. The
# director's _on_section transition handler lives here too: its job is
# the map refresh + follow-live re-drill, so it moves with the cluster it
# drives. Verbatim motion from live_mode.gd; state stays on the mount,
# reached through `_lm`; cross-cluster calls go through mount delegates.


# --- Director signal handlers ------------------------------------------

func _on_section(section_name: StringName) -> void:
	# A self-loop section (e.g. the gamescript's `Missionnull { enter Missionnull }`)
	# re-fires section_entered ~60/sec. libs/mus + the director stay byte-faithful;
	# we collapse the repeats here. Same section as last time = an idle tick: bump
	# the counter and refresh the now-playing suffix only. No new log row, no graph
	# rebuild (which would otherwise thrash 60/sec).
	if section_name == _lm._current_section and _lm._current_section != &"":
		_lm._idle_ticks += 1
		if _lm._idle_ticks == 1:
			_lm._refresh_now_playing()
		return
	# Real transition: reset idle, log it, rebuild the map + now-playing, and -- while
	# auto-following and already drilled into a state's program -- follow the VM into the
	# entered state's program so the live highlight tracks where it actually is.
	_lm._current_section = section_name
	_lm._idle_ticks = 0
	_refresh_map()
	_lm._refresh_now_playing()
	if _lm._follow_live and _lm._program_view != null and _lm._program_view.visible:
		_lm._drill_into(String(section_name), false)
	_lm._log_typed(_lm.EvType.SECTION, "section -> %s" % section_name)


func _refresh_map() -> void:
	if _lm._map == null:
		return
	_lm._map.clear_connections()
	for c in _lm._map.get_children():
		if c is GraphNode:
			c.free()
	_lm._refresh_empty_state()
	if _lm._document == null or not _lm._document.script_loaded():
		_lm._refresh_states_list()
		return
	var script_name: StringName = StringName(_lm._document.mus_script.get_default_script_name())
	# Model-driven (opcode-level), not string-parsed: edges are correct, switch
	# fan-out is captured, and self-looping idle states are flagged.
	var model: Array = _lm.MusicSectionGraphClass.build(_lm._document.mus_script, script_name)
	if model.is_empty():
		return
	var names: Array = _bank_names()
	# Per-section logic glyph summary (◇if ⋔switch ✎var ƒcall) so the map node
	# surfaces the logic that isn't a track chip -- the part that used to be
	# invisible until you opened the cramped right list. Keyed by section index.
	var logic_by_index: Dictionary = _build_logic_summaries(script_name)
	var incoming_by_index: Dictionary = _incoming_by_index(model)
	var node_by_index: Dictionary = {}
	for section in model:
		var idx: int = int(section.get("index", -1))
		var section_name := String(section.get("name", ""))
		var gn := GraphNode.new()
		# Floor the node width so play-chip names (e.g. "JOMEN602A") read instead of
		# clipping to "soun"; long names still ellipsize with a full-name tooltip.
		# Wider for the blueprint look + the logic badge row.
		gn.custom_minimum_size = Vector2(240, 0)
		gn.add_theme_font_size_override("title_font_size", 15)
		gn.name = "S_%d" % idx
		gn.set_meta("section", section_name)
		gn.gui_input.connect(_lm._on_node_gui_input.bind(section_name))
		gn.title = section_name
		if bool(section.get("is_entry", false)):
			gn.title += "   ★ start"
			# Be honest about the fixed entry: the first-declared state is always the
			# start, and there is no set-start / reorder affordance yet.
			gn.tooltip_text = "Start state: playback begins here. The first state is always the start (choosing a different start state, or reordering states, isn't supported yet)."
		if bool(section.get("is_idle_loop", false)):
			gn.title += "   ↻ idle"
		if _section_is_unlinked_in_model(section, incoming_by_index):
			gn.title += "   unlinked"
			gn.tooltip_text = _lm.UNLINKED_STATE_TOOLTIP
		# Body: up to 3 track chips; the full list shows in the right inspector.
		var plays: Array = section.get("plays", [])
		var shown: int = mini(plays.size(), 3)
		for p in range(shown):
			var play: Dictionary = plays[p]
			var track: int = int(play.get("track", -1))
			var chip: MusicTrackChip = _lm.MusicTrackChipClass.new()
			gn.add_child(chip)
			chip.setup(track, _track_name(names, track), bool(play.get("wait", false)), false)
			chip.preview_requested.connect(_lm._preview_track)
		if plays.size() > shown:
			var more := Label.new()
			more.text = "  +%d more" % (plays.size() - shown)
			more.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
			gn.add_child(more)
		# Logic badge: the if/switch/var/call summary the chips can't show. Only
		# when the state actually runs logic; reads as "open me to see the program".
		var badge_text: String = String(logic_by_index.get(idx, ""))
		if badge_text != "":
			var badge := Label.new()
			badge.text = badge_text
			badge.add_theme_color_override("font_color", Color(0.72, 0.74, 0.82))
			badge.tooltip_text = "Logic this state runs (if blocks, choose-by-value, variable edits, function calls). Double-click to open its program."
			gn.add_child(badge)
		if gn.get_child_count() == 0:
			# Slot 0 needs a row; an idle / branch-only section plays nothing.
			var spacer := Label.new()
			spacer.text = " "
			gn.add_child(spacer)
		gn.add_child(_open_program_button(section_name))
		# Output pin coloured by the dominant outgoing edge kind so a switch fan-out
		# (cyan) reads apart from a plain transition (blue) / branch (gold). Input pin
		# stays a muted neutral. GraphEdit tints each wire by its source pin colour.
		gn.set_slot(0, true, 0, Color(0.45, 0.50, 0.60), true, 0, _section_edge_color(section))
		_lm._map.add_child(gn)
		node_by_index[idx] = gn
	_layout_map(model, node_by_index)
	for section in model:
		var from_idx: int = int(section.get("index", -1))
		for e in section.get("edges", []):
			var to_idx: int = int(e.get("to", -1))
			if to_idx == from_idx:
				continue  # self-loop is the ↻ idle badge, not a drawn connection
			if node_by_index.has(from_idx) and node_by_index.has(to_idx):
				_lm._map.connect_node("S_%d" % from_idx, 0, "S_%d" % to_idx, 0)
	_highlight_active_node()
	# Fit the graph to the viewport, but only when the topology actually changed
	# (a different script / node count) -- not on the per-transition rebuilds that
	# keep the same sections, which would otherwise yank the view mid-playback.
	var sig: String = "%s:%d" % [String(script_name), node_by_index.size()]
	if sig != _lm._fit_signature:
		_lm._fit_signature = sig
		_fit_map_to_view()
	_lm._refresh_states_list()


func _open_program_button(section_name: String) -> Button:
	var b := Button.new()
	b.name = "OpenBlueprintButton"
	b.text = "Blueprint"
	b.tooltip_text = "Open this state's program."
	b.focus_mode = Control.FOCUS_NONE
	b.pressed.connect(func(): _lm._drill_into(section_name))
	return b


func _incoming_by_index(model: Array) -> Dictionary:
	var incoming: Dictionary = {}
	for section in model:
		incoming[int(section.get("index", -1))] = 0
	for section in model:
		var from_idx: int = int(section.get("index", -1))
		for e in section.get("edges", []):
			var to_idx: int = int(e.get("to", -1))
			if to_idx == from_idx:
				continue
			incoming[to_idx] = int(incoming.get(to_idx, 0)) + 1
	return incoming


func _section_is_unlinked_in_model(section: Dictionary, incoming_by_index: Dictionary) -> bool:
	if bool(section.get("is_entry", false)):
		return false
	var idx: int = int(section.get("index", -1))
	return int(incoming_by_index.get(idx, 0)) == 0


# Build {section_index -> compact logic-glyph summary} from the program AST, so a
# map node can show the if/switch/var/call shape it runs (the logic that isn't a
# track chip). One AST parse per rebuild; cheap for the shipped scripts.
func _build_logic_summaries(script_name: StringName) -> Dictionary:
	var out: Dictionary = {}
	if _lm._document == null or not _lm._document.script_loaded():
		return out
	var ast: Array = _lm._document.mus_script.get_program_ast(script_name)
	for sec in ast:
		out[int(sec.get("index", -1))] = _section_logic_badge(sec.get("statements", []))
	return out


# Compact WORD summary of the LOGIC a section runs ("2 if · 1 choose · 3 set"),
# counts elided when zero, empty when the section is just plays/transitions.
# Words, not glyphs: a glyph-only badge required memorizing the legend.
# Recurses if/switch bodies so nested logic still surfaces.
func _section_logic_badge(stmts: Array) -> String:
	var c := {"if": 0, "switch": 0, "var": 0, "call": 0}
	_count_logic(stmts, c)
	var parts := PackedStringArray()
	if c["if"] > 0:
		parts.append("%d if" % c["if"])
	if c["switch"] > 0:
		parts.append("%d choose" % c["switch"])
	if c["var"] > 0:
		parts.append("%d set" % c["var"])
	if c["call"] > 0:
		parts.append("%d call" % c["call"])
	return " · ".join(parts)


func _count_logic(stmts: Array, c: Dictionary) -> void:
	for s in stmts:
		match String(s.get("kind", "")):
			"if":
				c["if"] += 1
				_count_logic(s.get("then", []), c)
				_count_logic(s.get("else", []), c)
			"switch":
				c["switch"] += 1
			"assign":
				c["var"] += 1
				if bool(s.get("has_call", false)):
					c["call"] += 1
			"incdec":
				c["var"] += 1
			"expr":
				if bool(s.get("has_call", false)):
					c["call"] += 1


# Output-pin colour for a section, by its dominant outgoing edge kind: a switch
# fan-out reads cyan, a conditional branch gold, a plain transition blue.
func _section_edge_color(section: Dictionary) -> Color:
	var has_switch := false
	var has_branch := false
	for e in section.get("edges", []):
		var k := int(e.get("kind", 0))
		if k == _lm.MusicSectionGraphClass.KIND_SWITCH:
			has_switch = true
		elif k == _lm.MusicSectionGraphClass.KIND_BRANCH:
			has_branch = true
	if has_switch:
		return _lm.MusicSectionGraphClass.edge_color(_lm.MusicSectionGraphClass.KIND_SWITCH)
	if has_branch:
		return _lm.MusicSectionGraphClass.edge_color(_lm.MusicSectionGraphClass.KIND_BRANCH)
	return _lm.MusicSectionGraphClass.edge_color(_lm.MusicSectionGraphClass.KIND_TRANSITION)


# Center + zoom the section map so the whole graph fills the GraphEdit instead of
# clustering in the top-left with empty canvas to the right. Deferred one frame so
# the GraphNodes have sized themselves from their chip content before we measure.
func _fit_map_to_view() -> void:
	if _lm._map == null or not _lm.is_inside_tree() or _lm.get_tree() == null:
		return
	await _lm.get_tree().process_frame
	if _lm._map == null or not is_instance_valid(_lm._map):
		return
	var first: bool = true
	var min_x: float = 0.0
	var min_y: float = 0.0
	var max_x: float = 0.0
	var max_y: float = 0.0
	for c in _lm._map.get_children():
		if not (c is GraphNode):
			continue
		var gn: GraphNode = c
		var p: Vector2 = gn.position_offset
		var s: Vector2 = gn.size
		if first:
			min_x = p.x; min_y = p.y; max_x = p.x + s.x; max_y = p.y + s.y
			first = false
		else:
			min_x = minf(min_x, p.x); min_y = minf(min_y, p.y)
			max_x = maxf(max_x, p.x + s.x); max_y = maxf(max_y, p.y + s.y)
	if first:
		return  # no nodes
	var content := Vector2(max_x - min_x, max_y - min_y)
	if content.x <= 0.0 or content.y <= 0.0:
		return
	var view: Vector2 = _lm._map.size
	var margin := 80.0
	var zoom := minf((view.x - margin) / content.x, (view.y - margin) / content.y)
	zoom = clampf(zoom, 0.25, 1.0)
	_lm._map.zoom = zoom
	# scroll_offset is in zoomed pixels: a node at position_offset shows at
	# position_offset*zoom - scroll_offset, so center the content's midpoint.
	var center := Vector2((min_x + max_x) * 0.5, (min_y + max_y) * 0.5)
	_lm._map.scroll_offset = center * zoom - view * 0.5


# Deterministic BFS layered layout from the entry section, left to right, so the
# map doesn't reshuffle on every rebuild (GraphEdit.arrange_nodes is
# non-deterministic). Within each layer, nodes are ordered by the average row of
# their already-placed parents (barycenter) so a switch's targets sit beside the
# switch instead of sprawling, which cuts edge crossings. Sections no edge
# reaches (win/lose stings) trail in a final column.
func _layout_map(model: Array, node_by_index: Dictionary) -> void:
	var entry_idx: int = -1
	var adj: Dictionary = {}
	for section in model:
		var idx: int = int(section.get("index", -1))
		var outs: Array = []
		for e in section.get("edges", []):
			var t: int = int(e.get("to", -1))
			if t != idx:
				outs.append(t)
		adj[idx] = outs
		if bool(section.get("is_entry", false)):
			entry_idx = idx
	var layer: Dictionary = {}
	var queue: Array = []
	if entry_idx >= 0:
		layer[entry_idx] = 0
		queue.append(entry_idx)
	while not queue.is_empty():
		var n: int = queue.pop_front()
		for t in adj.get(n, []):
			if not layer.has(t):
				layer[t] = int(layer[n]) + 1
				queue.append(t)
	var max_layer: int = 0
	for v in layer.values():
		max_layer = maxi(max_layer, int(v))
	for section in model:
		var idx2: int = int(section.get("index", -1))
		if not layer.has(idx2):
			layer[idx2] = max_layer + 1
	# Reverse adjacency: who points at each node (self-loops already excluded).
	var incoming: Dictionary = {}
	for src in adj.keys():
		for dst in adj[src]:
			if not incoming.has(dst):
				incoming[dst] = []
			(incoming[dst] as Array).append(src)
	# Group nodes by layer, node-index order as the stable tiebreak.
	var members: Dictionary = {}
	var indices: Array = node_by_index.keys()
	indices.sort()
	var top_layer: int = 0
	for idx3 in indices:
		var l3: int = int(layer.get(idx3, 0))
		if not members.has(l3):
			members[l3] = []
		(members[l3] as Array).append(idx3)
		top_layer = maxi(top_layer, l3)
	# Place layer by layer; order each layer by the barycenter of its already-
	# placed parents. Parentless nodes (entry, unreached stings) keep index order.
	var assigned_row: Dictionary = {}
	for l4 in range(top_layer + 1):
		if not members.has(l4):
			continue
		var group: Array = (members[l4] as Array).duplicate()
		var bary: Dictionary = {}
		for idx4 in group:
			var total: float = 0.0
			var cnt: int = 0
			for src2 in incoming.get(idx4, []):
				if assigned_row.has(src2):
					total += float(assigned_row[src2])
					cnt += 1
			bary[idx4] = (total / cnt) if cnt > 0 else 1e9
		group.sort_custom(func(a, b):
			if bary[a] == bary[b]:
				return int(a) < int(b)
			return bary[a] < bary[b])
		# assigned_row stays the integer rank (drives the NEXT layer's barycenter
		# ordering); the visual y is centered on a shared mid-axis so a 1-node
		# layer lands at y=0 and a fan-out layer spreads symmetrically above and
		# below it. The linear chain then threads the middle of each fan instead
		# of pinning to the top with the fan hanging one-sidedly below.
		var row: int = 0
		var span: float = float(group.size() - 1) * 0.5
		for idx5 in group:
			assigned_row[idx5] = row
			var y: float = (float(row) - span) * 180.0
			(node_by_index[idx5] as GraphNode).position_offset = Vector2(l4 * 320, y)
			row += 1


# Tint the running VM's current section; clear the rest. Applied on each rebuild
# (step 3 turns this into an in-place glow that does not rebuild the map).
func _highlight_active_node() -> void:
	if _lm._map == null:
		return
	for gn in _lm._map.get_children():
		if gn is GraphNode:
			var active: bool = _lm._last_state == _lm.VM_RUNNING \
				and String(gn.get_meta("section", "")) == String(_lm._current_section)
			gn.modulate = Color(0.6, 1.0, 0.6) if active else Color(1, 1, 1)


# Resolve bank entry names so chips read "combat1" not "sound_3". Empty when no
# bank is loaded alongside the script.
func _bank_names() -> Array:
	if _lm._document == null or not _lm._document.bank_loaded():
		return []
	var out: Array = []
	for e in _lm._document.bank.get_entries():
		out.append(String(e.get("name", "")))
	return out


func _track_name(names: Array, track: int) -> String:
	if track >= 0 and track < names.size() and String(names[track]) != "":
		return String(names[track])
	return "track %d" % track


# Preview a bank track through our own MusicAudioPreview (a chip's ▶ button).
func _preview_track(track: int) -> void:
	if _lm._preview == null or _lm._document == null or not _lm._document.bank_loaded():
		return
	var stream: NovaSbfAudioStream = _lm._document.bank.get_stream_at(track)
	if stream != null:
		_lm._preview.play_stream(stream)


# A map node was clicked. While RUNNING this jumps the VM there. Double-click drills
# into the state's program; right-click opens the state menu. No-ops the jump while stopped.
func _on_map_node_selected(node: Node) -> void:
	if node == null:
		return
	var sec: String = String(node.get_meta("section", ""))
	if sec == "":
		return
	# Clicking the section that is currently playing keeps live auto-follow on;
	# clicking a different one pins away from it (Start/Stop, or clicking the live
	# state again, re-arms follow so a transition re-drills the program view).
	_lm._follow_live = (_lm._last_state == _lm.VM_RUNNING and sec == String(_lm._current_section))
	# While running, also jump the VM there.
	if _lm._last_state == _lm.VM_RUNNING:
		_lm._on_graph_section_pressed(StringName(sec))
	else:
		_lm._flash_start_warning(_lm.MAP_START_FIRST_TOOLTIP)


# A failed compile (Save, or the rare structured edit that doesn't round-trip)
# flashes the first diagnostic on the always-visible transport label. Structured
# edits are gated and roll back on failure, so this is a belt-and-suspenders surface.
func _on_compile_finished(success: bool, errors: Array) -> void:
	if success or errors.is_empty():
		return
	var first: Dictionary = errors[0] if errors[0] is Dictionary else {}
	var msg: String = String(first.get("message", "compile error"))
	var line: int = int(first.get("line", 0))
	if line > 0:
		msg = "line %d: %s" % [line, msg]
	_lm._flash_start_warning(msg)


# Double-clicking a state drills into its program. Single clicks fall
# through to GraphEdit's node_selected (-> _on_map_node_selected). Right-click opens
# the state context menu (rename / delete / open).
func _on_node_gui_input(event: InputEvent, section_name: String) -> void:
	if event is InputEventMouseButton and event.double_click \
			and event.button_index == MOUSE_BUTTON_LEFT:
		_lm._drill_into(section_name)
	elif event is InputEventMouseButton and event.pressed \
			and event.button_index == MOUSE_BUTTON_RIGHT:
		_lm._show_state_context_menu(section_name, event.global_position)


# Drag-to-add-play from the Tracks dock onto the program view. The document gates
# this on can_edit_plays() and recompiles; document.changed rebuilds the map and
# re-populates the drilled-in program view, so there's nothing else to refresh here.
func _on_inspector_add_play(section_name: StringName, track: int) -> void:
	if _lm._document == null or not _lm._document.has_method("insert_play"):
		return
	if _lm._document.insert_play(section_name, track):
		_lm._follow_live = false


# Variable picker list for the authoring popups: Var00..Var16 (all 17 int32
# slots in MUS_GLOBALS_BYTES = 68/4, matching the Game Dials panel) with
# friendly, per-script names where known (the same map the Variables tab + event
# log use). Var16 is the user global; offering it here keeps the assignment /
# expression picker in step with the dials, which already show it.
func _build_var_list() -> Array:
	var out: Array = []
	var sname := ""
	var profile := ""
	if _lm._document != null and _lm._document.script_loaded():
		sname = String(_lm._document.mus_script.get_default_script_name())
		if _lm._document.has_method("get_var_profile_path"):
			profile = _lm._document.get_var_profile_path()
	for i in range(17):
		var label: String = _lm.MusVarNames.label_for(sname, i, profile) if sname != "" else "Var%02d" % i
		out.append({"token": "Var%02d" % i, "label": label})
	return out


# A variable's display name changed (profile sidecar): rebuild the surfaces
# that cached the old label. Serialization is untouched (tokens only).
func _on_var_names_changed() -> void:
	_lm._refresh_var_labels()
	if _lm._program_view != null and _lm._program_view.visible and _lm._logic_section_name != "":
		_lm._populate_program_view(_lm._logic_section_name)
