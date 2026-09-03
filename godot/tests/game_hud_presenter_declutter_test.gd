extends GutTest

const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")

# The HUD declutter seam on GameHudPresenter: the huddetail edge cycles the
# persisted hud_detail level with wrap, the death screen forces level 3
# through the same seam, the value survives via the settings path, and a
# shared physical key fires huddetail — not hudcolor — modeling retail's
# first-match catalog order (rows 50 < 76; D-CTRL-4 keeps hudcolor live on
# its own key). The capture case runs over a REAL HudOverlay (HudFixture).
# [orig: the huddetail cycle Input_HandleActionBinding_0 @0x4E0601..0x4E0624;
#  the death force NapiNPClientMsg_0x00F @0x42E410..0x42E41C; the first-match
#  key scan @0x49d42f]
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


func test_huddetail_edge_cycles_and_wraps_persisted_level() -> void:
	var presenter: GameHudPresenter = autofree(GameHudPresenter.new())
	var start := presenter.hud_detail_level()

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

	# The persisted value survives the settings path: a fresh presenter
	# instance reads the cycled token back.
	presenter.cycle_hud_detail()
	var reread: GameHudPresenter = autofree(GameHudPresenter.new())
	assert_eq(reread.hud_detail_level(), (start + 1) % 4,
			"a fresh presenter reads the persisted hud_detail back")
	for i in range(3):
		presenter.cycle_hud_detail()
	assert_eq(presenter.hud_detail_level(), start,
			"the test leaves the persisted level where it started")


func test_death_screen_forces_level_3_through_the_persisted_seam() -> void:
	var presenter: GameHudPresenter = autofree(GameHudPresenter.new())
	var start := presenter.hud_detail_level()

	presenter.apply_death_screen_hud_detail()
	assert_eq(presenter.hud_detail_level(), 3,
			"the death screen forces the declutter level to 3")
	var reread: GameHudPresenter = autofree(GameHudPresenter.new())
	assert_eq(reread.hud_detail_level(), 3,
			"the force writes the persisted global like retail")

	presenter.set_hud_detail_level(start)
	assert_eq(presenter.hud_detail_level(), start,
			"the test restores the persisted level")


func test_hud_hidden_capture_is_scoped_non_persisting_and_keeps_effects_active() -> void:
	_staged_dir = HudFixture.stage_root(true)
	var presenter := HudFixture.booted_presenter(self, _staged_dir)
	var hud := presenter.get_game_hud()
	var persisted_start := _persisted_hud_detail()
	# The runtime level enters through the public gameplay action, which
	# persists it; from here on capture must leave that token alone.
	var runtime_start := (persisted_start + 1) % 3
	presenter.set_hud_detail_level(runtime_start)
	assert_eq(hud.get_hud_detail_level(), runtime_start,
			"the real overlay carries the presenter's runtime level")

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
	assert_eq(_persisted_hud_detail(), runtime_start,
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

	# Wrap both persisted tokens back to their starting values.
	for i in range(3):
		presenter.cycle_hud_detail()
	for i in range(5):
		presenter.cycle_hud_color()
	assert_eq(presenter.hud_detail_level(), detail_start,
			"the test leaves the persisted level where it started")
	assert_eq(presenter.hud_color_index(), color_start,
			"the test leaves the persisted scheme where it started")
