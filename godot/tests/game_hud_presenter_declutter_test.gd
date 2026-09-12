extends GutTest

const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")

# The HUD declutter seam on GameHudPresenter: the huddetail edge cycles the
# LIVE hud_detail level with wrap, the death screen forces level 3 through the
# same live seam, neither write reaches the persisted config value (which only
# the settings path owns), every mission start re-seeds the live level from
# that config value, and a shared physical key fires huddetail — not hudcolor
# — modeling retail's first-match catalog order (rows 50 < 76; D-CTRL-4 keeps
# hudcolor live on its own key). The capture case runs over a REAL HudOverlay
# (HudFixture).
# [orig: the huddetail cycle Input_HandleActionBinding_0 @0x4E0601..0x4E0624
#  (the layer global only); the death force NapiNPClientMsg_0x00F
#  @0x42E410..0x42E41C (the layer global only); the mission-start apply
#  apply_session_settings_to_globals @0x55154d from the config struct that
#  Game_SaveConfig @0x54c80d persists; the first-match key scan @0x49d42f]
#
# user:// settings hygiene: every test wraps its cycles back to the starting
# value so the shared settings.cfg survives (the hud_color tests' pattern).

var _staged_dir := ""


func after_each() -> void:
	if not _staged_dir.is_empty():
		TestFs.remove_dir_recursive(_staged_dir)
		_staged_dir = ""


func _persisted_hud_detail() -> int:
	return int(ConfigStore.read(
			GameHudPresenter.HUD_COLOR_CONFIG_PATH,
			GameHudPresenter.HUD_COLOR_SECTION,
			GameHudPresenter.HUD_DETAIL_CONFIG_KEY, 0))


func test_huddetail_edge_cycles_and_wraps_the_live_level() -> void:
	var presenter: GameHudPresenter = autofree(GameHudPresenter.new())
	var config := presenter.hud_detail_config()
	var start := presenter.hud_detail_level()
	assert_eq(start, config,
			"a fresh presenter's live level is the persisted config value")

	# A plain down edge cycles once; holding the key does not repeat.
	presenter.poll_hud_detail_edge(true, false, true)
	assert_eq(presenter.hud_detail_level(), (start + 1) % 4,
			"the down edge cycles the level once")
	presenter.poll_hud_detail_edge(true, false, true)
	assert_eq(presenter.hud_detail_level(), (start + 1) % 4,
			"holding the key does not re-cycle")

	# Held across a closed gate window: the latch rides the UNGATED state.
	presenter.poll_hud_detail_edge(true, false, false)
	presenter.poll_hud_detail_edge(true, false, true)
	assert_eq(presenter.hud_detail_level(), (start + 1) % 4,
			"a press held across a gate window cannot re-fire")

	# A chorded press (our debug picks ride Shift+F6) never cycles.
	presenter.poll_hud_detail_edge(false, false, true)
	presenter.poll_hud_detail_edge(true, true, true)
	assert_eq(presenter.hud_detail_level(), (start + 1) % 4,
			"a chorded press never cycles")
	presenter.poll_hud_detail_edge(false, false, true)

	# Three more cycles are the identity: level + 1 wraps past 3 to 0.
	for i in range(3):
		presenter.cycle_hud_detail()
	assert_eq(presenter.hud_detail_level(), start,
			"four cycles wrap 0->1->2->3->0 back to the start")

	# The cycle writes the LIVE level only: the persisted config value and a
	# fresh presenter (the next mission's start) never see it, and the
	# mission-start apply re-seeds the live level from the config value.
	presenter.cycle_hud_detail()
	assert_eq(_persisted_hud_detail(), config,
			"the cycle leaves the persisted config value alone")
	var reread: GameHudPresenter = autofree(GameHudPresenter.new())
	assert_eq(reread.hud_detail_level(), config,
			"a fresh presenter re-seeds from the config value, not the cycled live level")
	presenter.reapply_persisted_hud_detail()
	assert_eq(presenter.hud_detail_level(), config,
			"the mission-start apply re-seeds the live level from the config value")


func test_death_screen_forces_level_3_on_the_live_level_only() -> void:
	var presenter: GameHudPresenter = autofree(GameHudPresenter.new())
	var config := presenter.hud_detail_config()

	presenter.apply_death_screen_hud_detail()
	assert_eq(presenter.hud_detail_level(), 3,
			"the death screen forces the live declutter level to 3")
	assert_eq(_persisted_hud_detail(), config,
			"the force never touches the persisted config value")
	var reread: GameHudPresenter = autofree(GameHudPresenter.new())
	assert_eq(reread.hud_detail_level(), config,
			"the next mission's presenter starts from the config value")

	presenter.reapply_persisted_hud_detail()
	assert_eq(presenter.hud_detail_level(), config,
			"the mission-start apply ends the forced blank")


func test_hud_hidden_capture_is_scoped_non_persisting_and_keeps_effects_active() -> void:
	_staged_dir = HudFixture.stage_root(true)
	var presenter := HudFixture.booted_presenter(self, _staged_dir)
	var hud := presenter.get_game_hud()
	var persisted_start := _persisted_hud_detail()
	# The runtime level enters through the live gameplay seam; neither it nor
	# the capture may write the persisted config value.
	var runtime_start := (persisted_start + 1) % 3
	presenter.set_hud_detail_level(runtime_start)
	assert_eq(hud.get_hud_detail_level(), runtime_start,
			"the real overlay carries the presenter's runtime level")
	assert_eq(_persisted_hud_detail(), persisted_start,
			"the live seam never writes the persisted config value")

	assert_eq(presenter.begin_hud_hidden_capture(), OK)
	assert_eq(hud.get_hud_detail_level(), 3,
			"capture uses retail's blank HUD declutter level")
	assert_eq(presenter.begin_hud_hidden_capture(), ERR_BUSY,
			"a nested capture cannot overwrite the saved runtime level")
	var witness: HudHiddenCaptureWitness = \
			presenter.hud_hidden_capture_witness()
	assert_true(witness.is_valid())
	assert_eq(witness.hud_detail_level, 3)
	assert_false(witness.gameplay_hud_visible,
			"the blank level compiles no gameplay draw family")
	assert_true(witness.player_view_effects_active)
	assert_false(witness.ads_active)
	assert_false(witness.big_map_active)
	assert_false(witness.hud_canvas_layer_active)
	# The independent large-map pass (map_mode 2) cannot masquerade as a
	# blank HUD.
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 0x10000, 0x10000, 2, false,
			PackedInt32Array())
	witness = presenter.hud_hidden_capture_witness()
	assert_true(witness.big_map_active,
			"the independent large-map pass cannot masquerade as a blank HUD")
	assert_true(witness.gameplay_hud_visible)
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 0x10000, 0x10000, 0, false,
			PackedInt32Array())
	hud.visible = false
	witness = presenter.hud_hidden_capture_witness()
	assert_false(witness.player_view_effects_active,
			"effects hidden by their GameHud ancestor are not capture-active")
	hud.visible = true
	assert_eq(_persisted_hud_detail(), persisted_start,
			"capture must not write the user's persisted HUD detail")

	presenter.finish_hud_hidden_capture()
	assert_eq(hud.get_hud_detail_level(), runtime_start,
			"capture restores the exact prior runtime level")
	presenter.finish_hud_hidden_capture()
	assert_eq(hud.get_hud_detail_level(), runtime_start,
			"cleanup is idempotent")
	presenter.set_hud_detail_level(persisted_start)
	assert_eq(_persisted_hud_detail(), persisted_start,
			"the test leaves the persisted level where it started")


func test_shared_key_fires_huddetail_not_hudcolor() -> void:
	# Pin the catalog default rows (huddetail F6 / hudcolor F6) so the
	# shared-key predicate holds regardless of ambient user remaps. In-memory
	# only — the user's controls.cfg is never rewritten here.
	ControlsBindings.model().restore_defaults()
	var presenter: GameHudPresenter = autofree(GameHudPresenter.new())
	var detail_start := presenter.hud_detail_level()
	var color_start := presenter.hud_color_index()

	# Both catalog rows default to F6: one shared physical press fires the
	# earlier huddetail row only (first-match-wins, rows 50 < 76).
	presenter.poll_hud_keys(true, true, false, true)
	assert_eq(presenter.hud_detail_level(), (detail_start + 1) % 4,
			"the shared key cycles the declutter level")
	assert_eq(presenter.hud_color_index(), color_start,
			"the shadowed hudcolor row does not fire")
	presenter.poll_hud_keys(false, false, false, true)

	# A hudcolor edge without a huddetail press stays a live row (the
	# D-CTRL-4 adjudication: shadow only a SHARED key's edge).
	presenter.poll_hud_keys(false, true, false, true)
	assert_eq(presenter.hud_color_index(), (color_start + 1) % 6,
			"hudcolor stays live on its own edge")
	assert_eq(presenter.hud_detail_level(), (detail_start + 1) % 4,
			"the hudcolor edge leaves the declutter level alone")
	presenter.poll_hud_keys(false, false, false, true)

	# Wrap the live level and the persisted color token back to their
	# starting values.
	for i in range(3):
		presenter.cycle_hud_detail()
	for i in range(5):
		presenter.cycle_hud_color()
	assert_eq(presenter.hud_detail_level(), detail_start,
			"the test leaves the live level where it started")
	assert_eq(presenter.hud_color_index(), color_start,
			"the test leaves the persisted scheme where it started")


func test_kill_banner_retains_text_after_expiry() -> void:
	_staged_dir = HudFixture.stage_root(true)
	var world := WorldFixture.boot_minimal(self, _staged_dir)
	var presenter := HudFixture.presenter_over(self, world)
	var hud := presenter.get_game_hud()
	var sim := world.get_sim()
	var start := presenter.hud_detail_level()
	presenter.set_hud_detail_level(3)
	presenter.tick(false)
	var baseline: int = hud.get_draw_list_stats().glyphs
	var now := Simulation.ticks_from_ms(Time.get_ticks_msec())
	sim.retain_feed_announcement("ABC", now)
	presenter.tick(false)
	assert_eq(hud.get_draw_list_stats().glyphs, baseline + 3,
			"the banner is drawn through the presenter even at detail 3")
	now = Simulation.ticks_from_ms(Time.get_ticks_msec())
	sim.retain_feed_announcement("XYZ", now - 187)
	presenter.tick(false)
	assert_eq(hud.get_draw_list_stats().glyphs, baseline,
			"the presenter expires the banner using the HUD clock")
	assert_eq(sim.get_kill_announcement_text(), "XYZ",
			"expiration retains the death-screen text")
	presenter.set_hud_detail_level(start)


func test_world_points_project_through_the_presenter_view_projection() -> void:
	# The HUD's world-point consumers (the attach labels, the friendly tags)
	# and the crosshair-toast pick ray read the FRAME's projection, never the
	# gameplay camera's: while an aspect mode draws through the presenter's
	# stretched target the gameplay camera carries only a CULLING SUPERSET of
	# the frustum (LocalPlayerPresenter.view_projection), so its projection
	# lands a label off on one axis, and its ray misses on one axis.
	var world := WorldFixture.boot_minimal(self)
	var surface := SubViewport.new()
	surface.size = Vector2i(1024, 600)
	add_child_autofree(surface)
	var camera := Camera3D.new()
	surface.add_child(camera)
	camera.current = true
	var player: LocalPlayerPresenter = add_child_autofree(LocalPlayerPresenter.new())
	player.setup(world, camera, null, ControlsModel.new())
	var presenter: GameHudPresenter = add_child_autofree(GameHudPresenter.new())
	presenter.setup(world, player, null)
	# The stock 4:3 mode over the wider 1024x600 surface: the frame keeps the
	# policy 80 horizontal across the width and stretches 0.75/(600/1024).
	world.get_sim().set_local_player_aspect_mode(0)
	var dt := Simulation.tick_dt()
	for i in 2:
		var frame_input := player.before_world_tick(dt, false, true)
		world.tick(camera.global_position, camera.global_transform, dt, frame_input)
		player.after_world_tick()
	var through: Camera3D = player.projection_camera()
	var target: SubViewport = player.projection_viewport()
	assert_not_null(through, "the 4:3 mode over a 1024x600 surface draws through a target")
	if through == null or target == null:
		player.teardown()
		return

	var projection := presenter.hud_view_projection(camera)
	assert_almost_eq(projection.x.x, 1.0 / tan(deg_to_rad(40.0)), 0.001,
			"proj[0][0] = cot(fov_h/2) across the real width: the frame's projection")
	assert_almost_eq(projection.y.y, 1.0 / (tan(deg_to_rad(40.0)) * 0.75), 0.001,
			"proj[1][1] follows the SELECTED ratio")
	assert_false(is_equal_approx(camera.get_camera_projection().x.x, projection.x.x),
			"the gameplay camera carries only the culling superset, which the HUD never projects through")

	# The pick ray rides the same frame: a surface point maps into the
	# target's pixels by the blit stretch (target size / surface size) and the
	# ray leaves the target camera there.
	assert_eq(DebugEntityPicker.view_camera(camera, player), through,
			"the pick ray is built through the target camera while the mode is live")
	var surface_point := Vector2(256.0, 150.0)
	var target_point := DebugEntityPicker.view_point(camera, surface_point, player)
	assert_almost_eq(target_point,
			Vector2(surface_point.x * float(target.size.x) / 1024.0,
					surface_point.y * float(target.size.y) / 600.0),
			Vector2(0.001, 0.001), "the surface point scales into the target's pixels")
	var pick := DebugEntityPicker.pick_with_camera(world.get_sim(), camera, surface_point,
			"mouse_click", player)
	assert_not_null(pick, "the sim always answers the stable card")
	assert_eq(pick.ray_dir_godot, through.project_ray_normal(target_point),
			"the card's replayable ray is the target camera's at the stretched point")
	assert_eq(DebugEntityPicker.view_point(camera, surface_point, null), surface_point,
			"a bare camera picks in its own pixels")

	# The production click picker rides the same recipe: a tools-open click at
	# a surface point builds its ray through the target camera at the
	# stretched point, never through the gameplay camera's culling superset.
	var catcher := PickClickCatcher.new()
	surface.add_child(catcher)
	catcher.setup(world.world_view(), player, DebugPickList.new())
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	click.position = surface_point
	catcher.handle_click(click)
	assert_not_null(catcher.last_pick, "the click ran its ray")
	assert_eq(catcher.last_pick.ray_dir_godot, through.project_ray_normal(target_point),
			"a click at a surface point projects through the target camera at the stretched point")
	assert_ne(catcher.last_pick.ray_dir_godot, camera.project_ray_normal(surface_point),
			"...never through the gameplay camera, whose ray misses on one axis")
	player.teardown()
	assert_eq(presenter.hud_view_projection(camera).x.x, camera.get_camera_projection().x.x,
			"without a live target the HUD projects through the camera's own projection")
