extends GutTest

## The HUD viewport's device headless through the editor's wire seam (the editor deep-integration
## plan's DI-20): hudpos.def opened shows its HUD through the runtime's own HudOverlay
## (authoring/hud_viewport_applier) in an offscreen SubViewport, configured from the layout as the
## project's files hold it; the device says where each element of the HUD's walk drew, which the
## viewport's items and hit read (the frame and the health bar at their hudpos places); the screen the
## options name lays the HUD out at that size (the health bar where the design space scales it to);
## the weapon the options name installs its art (its clip graphic and silhouette draw); the game's own
## view effects ride the overlay (the goggles' mask up for night vision, the damage vignette for a
## hit); an edit of the layout's text configures the HUD again.

const EDITOR_SCENE := "res://editor/editor_root.tscn"
const EditorSeam := preload("res://tests/authoring/editor_seam.gd")
const PreviewBackgroundChecks := preload("res://tests/authoring/preview_background_checks.gd")
const LAYOUT := "defs/hudpos.def"

var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null


func before_each() -> void:
	var packed := load(EDITOR_SCENE) as PackedScene
	assert_not_null(packed, "the editor scene loads (the editor-enabled variant is loaded)")
	if packed == null:
		return
	_app = packed.instantiate()
	_seam = EditorSeam.new(_app)
	var settings_dir := OS.get_cache_dir().path_join("opennova editor hud %d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(settings_dir), OK)
	_dirs.append(settings_dir)
	_app.set("settings_path", settings_dir.path_join("editor_settings.json"))
	add_child_autofree(_app)


func after_each() -> void:
	_app = null
	for dir in _dirs:
		TestFs.remove_dir_recursive(dir)
	_dirs.clear()
	await get_tree().process_frame


func _write(path: String, bytes: PackedByteArray) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(path.get_base_dir()), OK)
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "wrote %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


## A `side` x `side` uncompressed 32-bit TGA, every texel opaque white.
func _tga(side: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(18)
	bytes[2] = 2
	bytes[12] = side
	bytes[14] = side
	bytes[16] = 32
	for _k in side * side:
		bytes.append_array(PackedByteArray([255, 255, 255, 255]))
	return bytes


## A project holding a soldier panel's layout, its art, a weapon and the damage vignette. The panel's
## HUDDECLUT rows show the health bar and the weapon group: without them the game hides both (D-HUD-54).
func _project() -> String:
	var dir := OS.get_cache_dir().path_join("opennova editor hud project %d" % Time.get_ticks_usec())
	_dirs.append(dir)
	assert_true(_seam.new_project(dir, "HUD Viewport"))
	_write(dir.path_join(LAYOUT), TestFs.crlf("""// The soldier panel
StaticFrame frame.tga 6,586
HUDHEALTH 25,741,177,751
HUDSTANCEPOS 30,639
HUDSTANCE 0 0 0 stance0.tga STAND
HUDSTANCE 1 0 0 stance1.tga CROUCH
HUDSTANCE 2 0 0 stance2.tga PRONE
HUDSTANCE 3 0 0 stance3.tga SITTING
HUDSTANCE 4 0 0 stance4.tga EMPLACED
HUDSTANCE 5 0 0 stance5.tga PARACHUTE
HUDCLIP 14,648
HUDWPNICON 14,606
alphafade 40 70 3
HUDDECLUT_DMGBAR 1 1 1 1
HUDDECLUT_WPNGRP 1 1 1 1
""").to_utf8_buffer())
	for name in ["frame.tga", "stance0.tga", "stance1.tga", "stance2.tga", "stance3.tga", "stance4.tga",
			"stance5.tga", "h_clip.tga", "h_rnd.tga", "h_icon.tga", "vignette.tga", "NVG.tga"]:
		_write(dir.path_join("textures").path_join(name), _tga(8))
	_write(dir.path_join("defs/weapon.def"), TestFs.crlf("""weapon "W_TEST"
	clipsize 30
	hudicon h_icon.tga
	hudclipgfx 0 0 h_clip.tga
	hudrndgfx 6 0 4 0 1 h_rnd.tga
end
""").to_utf8_buffer())
	_seam.request({"kind": "rescan"})
	return dir


func _viewport() -> Dictionary:
	return _seam.query("viewport", {"op": "state", "path": LAYOUT, "kind": "hud"})


## The viewport ready on its device, its elements reported: a frame at a time until they are.
func _await_ready() -> Dictionary:
	var state := _viewport()
	for _frame in 600:
		if String(state.get("status", "")) == "ready" and bool(state.get("device", {}).get("attached", false)) \
				and not (state.get("items", []) as Array).is_empty():
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport()
	return state


func _overlay() -> HudOverlay:
	var device: SubViewport = _app.get_viewport_device(LAYOUT, "hud")
	if device == null:
		return null
	var found := device.find_children("*", "HudOverlay", true, false)
	return found[0] as HudOverlay if not found.is_empty() else null


func _item(state: Dictionary, element: String) -> Dictionary:
	for item in state.get("items", []):
		if String(item.get("element", "")) == element:
			return item
	return {}


func _pump_frames(count: int) -> void:
	for _frame in count:
		_app.pump()
		await get_tree().process_frame


func test_the_layout_draws_through_the_games_hud() -> void:
	if _app == null:
		return
	_project()
	assert_true(_seam.open_document(LAYOUT), "hudpos.def opens")
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", "its HUD shows in the Preview window")
	var overlay := _overlay()
	assert_not_null(overlay, "its device draws it with the runtime's HudOverlay")
	if overlay == null:
		return
	assert_true(overlay.is_configured(), "configured from the layout as the project holds it")
	assert_eq(overlay.size, Vector2(1024, 768), "laid out at the design screen")
	# The elements the HUD's walk drew, at their hudpos places.
	var frame := _item(state, "frame")
	assert_false(frame.is_empty(), "the static frame drew")
	if not frame.is_empty():
		assert_eq(int(frame["rect"][0]), 6)
		assert_eq(int(frame["rect"][1]), 586)
		assert_eq(String(frame["lines"][0]["key"]), "STATICFRAME")
		assert_eq(String(frame["lines"][0]["locator"]), "2:1")
	var health := _item(state, "health")
	assert_false(health.is_empty(), "the health bar drew")
	if not health.is_empty():
		assert_eq(int(health["rect"][0]), 25)
		assert_eq(int(health["rect"][3]), 751)
	assert_false(_item(state, "stance").is_empty(), "the stance icon drew")
	# The weapon weapon.def holds first: its clip graphic and its silhouette.
	assert_false(_item(state, "clip_indicator").is_empty(), "the weapon's clip graphic drew")
	assert_false(_item(state, "instruments").is_empty(), "the weapon's silhouette drew at HUDWPNICON")
	var hit: Dictionary = _seam.query("viewport", {"op": "hit", "path": LAYOUT, "kind": "hud", "x": 100, "y": 745})
	assert_eq(String(hit.get("kind", "")), "health", "a point of the bar names it")
	# The view effects ride the overlay as the game mounts them.
	var effects := overlay.find_children("PlayerViewEffects", "", true, false)
	assert_eq(effects.size(), 1, "the game's view effects under the overlay")


func test_the_options_reach_the_hud() -> void:
	if _app == null:
		return
	_project()
	assert_true(_seam.open_document(LAYOUT))
	var state := await _await_ready()
	var overlay := _overlay()
	assert_not_null(overlay)
	if overlay == null:
		return
	# A smaller screen: the HUD laid out at it, its elements where the design space scales them.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"width": 640, "height": 480, "weapon": "NONE"}}}))
	await _pump_frames(3)
	assert_eq(overlay.size, Vector2(640, 480))
	state = _viewport()
	var health := _item(state, "health")
	assert_false(health.is_empty())
	if not health.is_empty():
		assert_eq(int(health["rect"][0]), (25 * 640 + 512) / 1024, "the design space scaled to 640")
	assert_true(_item(state, "clip_indicator").is_empty(), "no weapon: no clip graphic")
	# Night vision and a hit: the game's goggle mask and its damage vignette.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"view": "night_vision", "damage": 192}}}))
	await _pump_frames(2)
	var effects := overlay.find_children("PlayerViewEffects", "", true, false)
	assert_eq(effects.size(), 1)
	if effects.size() == 1:
		assert_true(bool(effects[0].call("is_nvg_mask_visible")), "the goggles' mask is up")
		var vignette := effects[0].find_children("ScreenFlashVignette", "", true, false)
		assert_true(not vignette.is_empty() and (vignette[0] as CanvasItem).visible, "the damage vignette shows")
	# The detail level that hides the HUD: nothing of the soldier panel draws.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"detail": 3}}}))
	await _pump_frames(2)
	assert_true(_item(_viewport(), "frame").is_empty(), "level 3 hides the HUD")


func test_an_edit_of_the_text_configures_it_again() -> void:
	if _app == null:
		return
	_project()
	assert_true(_seam.open_document(LAYOUT))
	var state := await _await_ready()
	var builds := int(state.get("builds", 0))
	# The health bar moved by its line (line 3, "HUDHEALTH 25,741,177,751": its first corner).
	assert_true(_seam.done({"kind": "edit_record", "path": LAYOUT, "edits": [
			{"op": "apply", "payload": "text.span", "line": 3, "column": 11, "length": 6, "text": "45,701"}]}))
	_seam.request({"kind": "end_edit", "path": LAYOUT})
	for _frame in 600:
		state = _viewport()
		var health := _item(state, "health")
		if int(state.get("builds", 0)) > builds and not health.is_empty() and int(health["rect"][0]) == 45:
			break
		_app.pump()
		await get_tree().process_frame
	var moved := _item(state, "health")
	assert_false(moved.is_empty())
	if not moved.is_empty():
		assert_eq(int(moved["rect"][0]), 45, "the bar where the edited line puts it")
		assert_eq(int(moved["rect"][1]), 701)


## The viewport's state once the element's box has its left edge at `left` (a frame at a time, up to 600).
func _await_left(element: String, left: int) -> Dictionary:
	var state := _viewport()
	for _frame in 600:
		var item := _item(state, element)
		if not item.is_empty() and int(item["rect"][0]) == left:
			break
		_app.pump()
		await get_tree().process_frame
		state = _viewport()
	return state


func test_the_handles_write_its_lines_and_the_picture_follows() -> void:
	if _app == null:
		return
	_project()
	assert_true(_seam.open_document(LAYOUT))
	var state := await _await_ready()
	# At 1600 x 1200: the health bar moved 20 design units right and 40 up, one undo step, drawn there.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"width": 1600, "height": 1200}}}))
	state = await _await_left("health", (25 * 1600 + 512) / 1024)
	var health := _item(state, "health")
	assert_true(bool(health.get("movable", false)) and bool(health.get("resizable", false)),
			"the bar moves and its corners size it")
	var right_before := int(health["rect"][2]) if not health.is_empty() else 0
	assert_true(_seam.done({"kind": "edit_in_viewport", "path": LAYOUT,
			"command": {"name": "move", "item": "health", "by": [20, -40]}}), "move by its item")
	state = await _await_left("health", (45 * 1600 + 512) / 1024)
	health = _item(state, "health")
	assert_false(health.is_empty())
	if not health.is_empty():
		assert_eq(int(health["rect"][0]), (45 * 1600 + 512) / 1024, "drawn where its line now puts it")
		assert_eq(String(health["lines"][0]["text"]), "HUDHEALTH 45,701,197,711", "its line rewritten in place")
	# Its bottom right corner 30 units further right: the bar wider, its left edge where it was.
	assert_true(_seam.done({"kind": "edit_in_viewport", "path": LAYOUT,
			"command": {"name": "resize", "item": "health", "handle": "bottom_right", "by": [30, 0]}}))
	for _frame in 600:
		health = _item(_viewport(), "health")
		if not health.is_empty() and int(health["rect"][2]) > right_before + 40:
			break
		await _pump_frames(1)
	assert_gt(int(health.get("rect", [0, 0, 0])[2]), right_before + 40, "the far edge moved")
	# At 640 x 480 the same lines draw where the design space scales them to.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"width": 640, "height": 480}}}))
	state = await _await_left("health", (45 * 640 + 512) / 1024)
	assert_eq(int(_item(state, "health").get("rect", [0])[0]), (45 * 640 + 512) / 1024, "laid out at 640")
	# Each command one undo step: two undone, the bar back where the file had it.
	_seam.request({"kind": "undo", "path": LAYOUT})
	_seam.request({"kind": "undo", "path": LAYOUT})
	state = await _await_left("health", (25 * 640 + 512) / 1024)
	assert_eq(int(_item(state, "health").get("rect", [0])[0]), (25 * 640 + 512) / 1024, "undone")


## The editor's Preview background behind a HUD, its own mid grey on Dark: Grey as it first draws, then each of
## the four as the editor sets it, live (preview_background_checks.gd).
func test_the_preview_background_draws_behind_the_hud() -> void:
	if _app == null:
		return
	_project()
	assert_true(_seam.open_document(LAYOUT), "hudpos.def opens")
	var state := await _await_ready()
	assert_eq(String(state.get("status", "")), "ready", str(state))
	PreviewBackgroundChecks.check_each(self, _app, _seam, LAYOUT, "hud", "Backdrop")
	var drawn := PreviewBackgroundChecks.read(_app.get_viewport_device(LAYOUT, "hud"), "Backdrop")
	if not drawn.is_empty():
		assert_almost_eq((drawn["material"] as ShaderMaterial).get_shader_parameter("backdrop_own") as Vector3,
				Vector3(0.27, 0.29, 0.31), Vector3.ONE * 0.001, "Dark draws the HUD preview's own grey")
	# The view effects' sun veil reads the Celestial's process-wide alpha: another device's sun (an environment view
	# looking at it) never veils the HUD preview, which has no world.
	RenderingServer.global_shader_parameter_set("opennova_sun_veil_alpha", 0.8)
	await _pump_frames(2)
	var overlay := _overlay()
	var veil := overlay.get_node_or_null("PlayerViewEffects/SunVeil") as CanvasItem if overlay != null else null
	assert_not_null(veil, "the view effects' sun veil")
	if veil != null:
		assert_false(veil.visible, "no sun over the HUD preview")
	RenderingServer.global_shader_parameter_set("opennova_sun_veil_alpha", 0.0)


## S23 C: the sights and the Tab board. A scoped rifle with a rangefinder and an elevation readout, its sights up
## with the aim on a wall 300 metres away: the game's SIGHTS card up with its one row, the scoped view's ring over
## it, the scope readouts drawn at hudpos.def's HUDSCOPE places from gametext's templates; the binoculars put them
## down. The Tab board held up for a team game over stand-in players draws as the game's board.
func test_the_sights_and_the_board_draw() -> void:
	if _app == null:
		return
	var dir := _project()
	var layout := FileAccess.get_file_as_string(dir.path_join(LAYOUT))
	_write(dir.path_join(LAYOUT), (layout + TestFs.crlf("""fonthud1_hi Gunpl22b.fnt
HUDSCOPERANGEXY 600,400
HUDSCOPEZEROXY 600,420
HUDSCOPEMAGXY 600,440
HUDDECLUT_XHAIRS 1 1 1 1
""")).to_utf8_buffer())
	_write(dir.path_join("fonts/Gunpl22b.fnt"), FileAccess.get_file_as_bytes("res://../fixtures/fnt/synth_1page.fnt"))
	_write(dir.path_join("textures/card.tga"), _tga(8))
	# The crosshair's style 0 picture (cross01.tga), so the HUD draws its crosshair where its gate lets it.
	_write(dir.path_join("textures/cross01.tga"), _tga(8))
	var weapons := FileAccess.get_file_as_string(dir.path_join("defs/weapon.def"))
	_write(dir.path_join("defs/weapon.def"), (weapons + TestFs.crlf("""weapon "W_SCOPE"
	clipsize 5
	startrounds 10
	flags scoped
	flags showrange
	flags showelevation
	scope_max_mag 4 0
	scope_max_zero 10 100 200 1
	pos 25 -5 -145 0 0 0
	tpos -1 12 -138 0 0 0
	sights card.tga 112 0 889 768 blend
end
""")).to_utf8_buffer())
	var gametext := RtxtStringFile.new()
	var overlays := gametext.add_section("Overlays")
	gametext.add_entry("STROVER_DIST", "Distance: %ldm", overlays, Vector2i.ZERO)
	gametext.add_entry("STROVER_DIST1KM", "Distance: >1km", overlays, Vector2i.ZERO)
	gametext.add_entry("STROVER_KILLLIST", "Player List", overlays, Vector2i.ZERO)
	var hud := gametext.add_section("hud")
	gametext.add_entry("hud_scope_zero", "Zero: %dm", hud, Vector2i.ZERO)
	gametext.add_entry("hud_scope_mag", "%dx", hud, Vector2i.ZERO)
	_write(dir.path_join("gametext.bin"), gametext.to_byte_array())
	_seam.request({"kind": "rescan"})
	assert_true(_seam.open_document(LAYOUT))
	var state := await _await_ready()
	var overlay := _overlay()
	assert_not_null(overlay)
	if overlay == null:
		return
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"weapon": "W_SCOPE"}}}))
	await _pump_frames(3)
	assert_false(_item(_viewport(), "crosshair").is_empty(), "the crosshair drawn, the sights down")
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"weapon": "W_SCOPE", "sights": true, "range": 300}}}))
	await _pump_frames(4)
	state = _viewport()
	# The body's aimed shot the range stamps as the game's input pack does shuts the crosshair's gate over the card.
	assert_true(_item(state, "crosshair").is_empty(), "no crosshair with the sights up")
	var sights: Dictionary = state.get("body", {}).get("sights", {})
	assert_true(bool(sights.get("up", false)), str(sights))
	assert_true(bool(sights.get("card", false)) and bool(sights.get("readouts", false)), str(sights))
	assert_almost_eq(float(sights.get("range", 0.0)), 300.0, 2.0, "the rangefinder reads the wall")
	var cards := overlay.find_children("SightsCard", "", true, false)
	assert_eq(cards.size(), 1, "the game's SIGHTS card rides the overlay")
	if cards.size() == 1:
		# Its rows scale to the screen it reads off the viewport: the options' screen, whatever the picture's size,
		# the picture that screen stretched once (never the card's rows scaled to the picture and again by it).
		assert_eq(int(cards[0].call("row_count")), 1, "its one row, its picture found in the project")
		assert_true(bool(cards[0].call("is_card_up")), "up once the scope settles")
	var masks := overlay.find_children("ScopeCircleMask", "", true, false)
	assert_eq(masks.size(), 1)
	if masks.size() == 1:
		assert_true((masks[0] as CanvasItem).visible, "the scoped view's ring over the card")
	var details := _item(state, "scope_details")
	assert_false(details.is_empty(), "the scope readouts drew")
	if not details.is_empty():
		assert_almost_eq(int(details["rect"][0]), 600, 2, "at HUDSCOPERANGEXY (the first glyph's own offset)")
		assert_almost_eq(int(details["rect"][1]), 400, 4, "at HUDSCOPERANGEXY")
	# The binoculars put them down.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"view": "binoculars"}}}))
	await _pump_frames(3)
	state = _viewport()
	assert_false(bool(state.get("body", {}).get("sights", {}).get("up", true)))
	if cards.size() == 1:
		assert_false(bool(cards[0].call("is_card_up")), "no card under the binoculars")
	assert_true(_item(state, "scope_details").is_empty(), "no readouts under the binoculars")
	# The Tab board over six stand-in players of a team game.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"view": "normal", "sights": false, "board": true,
					"game_type": "TDM", "players": 6}}}))
	await _pump_frames(3)
	state = _viewport()
	assert_false(_item(state, "scoreboard").is_empty(), "the Tab board drew")
	assert_eq(int(state.get("body", {}).get("board", {}).get("rows", 0)), 6)
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"board": false}}}))
	await _pump_frames(3)
	assert_true(_item(_viewport(), "scoreboard").is_empty(), "put down, it draws nothing")
	# The card's rows scale to the screen it reads off the viewport, as the game's screen: the options' screen
	# whatever the picture's size (the picture 1024 x 768 here), the picture that screen stretched once, never the
	# rows scaled to the picture and again by it.
	assert_true(_seam.done({"kind": "set_viewport", "path": LAYOUT,
			"viewport": {"kind": "hud", "options": {"width": 1600, "height": 1200, "sights": true}}}))
	await _pump_frames(4)
	if cards.size() == 1:
		assert_true(bool(cards[0].call("is_card_up")), "up again")
		assert_eq((cards[0] as Control).get_viewport_rect().size, Vector2(1600, 1200), "the card reads the HUD's screen")
	assert_eq(overlay.scale, Vector2.ONE, "the overlay at the screen, the viewport stretching it")
