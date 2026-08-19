extends SceneTree

# Message-feed screenshot probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Boots the game into a mission
# and drives the ported SYSTEM-ring surface (D-HUD-23) end to end on the HUD
# side: real gametext "Canned Msg" templates resolved from the mounted install,
# the engine formatters (`Simulation.format_feed_line` /
# `format_feed_camp_line`), and `HudOverlay.push_feed_line` with the witnessed
# per-case colors — so the capture shows the real ring geometry (HUDSYSTEXT
# anchor, three rows, 18-design-px downward step) and the real line palette.
# Only the wire leg (S2C 0x1E -> netsim fold) is bypassed; that half is pinned
# by ctest `feed_format` and needs a remote match to fire live.
#
#   NW_SP_MISSION=00TRa.bms NW_RESOURCE_DIR=<retail install> \
#   NW_PROBE_SHOTS=<dir> "$GODOT_BIN" --path godot --resolution 1600x900 \
#       -s res://tests/killfeed_probe.gd

const LOAD_TIMEOUT_WALL_SECONDS := 240.0

var _shots_dir := ""


func _initialize() -> void:
	_shots_dir = OS.get_environment("NW_PROBE_SHOTS")
	call_deferred("_run")


func _fail(msg: String) -> void:
	push_error("killfeed_probe: " + msg)
	quit(1)


func _find_hud(node: Node):
	if node.get_class() == "HudOverlay":
		return node
	for child in node.get_children():
		var found = _find_hud(child)
		if found != null:
			return found
	return null


func _run() -> void:
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		_fail("failed to load main_game.tscn")
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		_fail("main_game lacks a World child")
		return
	var deadline := Time.get_ticks_msec() + int(LOAD_TIMEOUT_WALL_SECONDS * 1000.0)
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		if Time.get_ticks_msec() > deadline:
			_fail("mission never produced a local player")
			return
		await process_frame
	var sim = world.get_sim()
	var hud = null
	while hud == null:
		if Time.get_ticks_msec() > deadline:
			_fail("HudOverlay never appeared")
			return
		hud = _find_hud(root)
		await process_frame
	var table: RtxtStringFile = Strings.get_table("gametext")
	if table == null:
		_fail("gametext table unavailable")
		return

	# Three lines, one per witnessed color class (the ring shows the newest
	# three): an uninvolved kill (grey 0xFFAFAFAF), a medic line (0xFF008CEE),
	# and a blue-team camp line (0xFF00AFFF) composed from the WPNames table.
	var kill_tmpl := table.get_string_in_section("Canned Msg", "STRCND04")
	hud.push_feed_line(sim.format_feed_line(kill_tmpl, "SPAGHETTI", "Belsman", "", ""),
			0xFFAFAFAF)
	var medic_tmpl := table.get_string_in_section("Canned Msg", "STRCND45")
	hud.push_feed_line(sim.format_feed_line(medic_tmpl, "A-99", "elk road", "", ""),
			0xFF008CEE)
	var camp_tmpl := table.get_string_in_section("Canned Msg", "STRCND_FULLYCAMPED_BLUE")
	var wpname := ""
	if table.has_string_in_section("WPNames", "STRWPNAME001"):
		wpname = table.get_string_in_section("WPNames", "STRWPNAME001")
	hud.push_feed_line(sim.format_feed_camp_line(camp_tmpl, wpname), 0xFF00AFFF)

	await process_frame
	await RenderingServer.frame_post_draw
	if not _shots_dir.is_empty():
		var img := root.get_texture().get_image()
		var path := _shots_dir.path_join("killfeed.png")
		var err := img.save_png(path)
		if err != OK:
			_fail("failed to save %s (err %d)" % [path, err])
			return
		print("killfeed_probe: saved ", path)
	quit(0)
