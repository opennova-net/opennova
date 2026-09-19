extends GutTest

const MountLook := preload("res://tests/support/mount_look.gd")

# Optional installed-data integration test. Assets stay in the user's mounted
# VFS; only the three selected weapon/ammo rows are staged into the real minimal
# world. No copyrighted art is committed to the fixture pack.
var _stage := ""

func after_each() -> void:
	if not _stage.is_empty():
		TestFs.remove_dir_recursive(_stage)
		_stage = ""

func _definition(source: String, kind: String, name: String) -> String:
	var pattern := RegEx.new()
	pattern.compile('(?mi)^' + kind + '\\s+"?' + name + '"?\\s*$')
	var found := pattern.search(source)
	if found == null:
		return ""
	var next := RegEx.new()
	next.compile('(?mi)^' + kind + '\\s+')
	var end := next.search(source, found.get_end())
	return source.substr(found.get_start(),
			(end.get_start() if end != null else source.length()) - found.get_start())

# Javelin and tanks are Escalation content in the stock install. Mods may
# provide them in the base mount; choose the first authored weapon table that
# carries Javelin instead of assuming OPENNOVA_JO_DIR implies an expansion.
func _combat_root() -> ResourceRoot:
	var installed := RetailData.install()
	if installed.is_empty():
		pending("OPENNOVA_JO_DIR is required for installed combat HUD validation")
		return null
	var expansions := PackedStringArray([""])
	expansions.append_array(RetailData.expansions())
	for expansion: String in expansions:
		var art := ResourceRoot.new()
		var mounted := art.mount_runtime(installed, expansion)
		assert_eq(mounted, OK, "combat HUD assets mount: " + expansion)
		if mounted != OK:
			return null
		var weapons := WeaponDatabase.new()
		var loaded := weapons.load_from_resource_root(art, "weapon.def")
		assert_eq(loaded, OK, "combat HUD weapon definitions load: " + expansion)
		if loaded != OK:
			return null
		if weapons.find_weapon("WPN_JAVELIN") >= 0:
			return art
	pending("OPENNOVA_JO_DIR needs Escalation or mod data providing Javelin and tanks")
	return null


func test_installed_launcher_and_mortar_hud_transitions() -> void:
	var art := _combat_root()
	if art == null:
		return
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(art, "weapon.def"), OK)
	var layout := HudPos.new()
	assert_eq(layout.load_from_resource_root(art, "hudpos.def"), OK)
	var weapon_source := art.read_file("weapon.def").get_string_from_utf8()
	var ammo_source := art.read_file("ammo.def").get_string_from_utf8()
	_stage = HudFixture.stage_root(true)
	var staged_weapons := FileAccess.get_file_as_string(_stage.path_join("weapon.def"))
	var staged_ammo := FileAccess.get_file_as_string(_stage.path_join("ammo.def"))
	var names := ["WPN_JAVELIN", "WPN_STINGER", "WPN_MORTAR"]
	for name: String in names:
		var row := _definition(weapon_source, "weapon", name)
		assert_false(row.is_empty(), name + " has an installed definition")
		staged_weapons += "\n" + row
	for name: String in names:
		var ammo_name := weapons.get_weapon(weapons.find_weapon(name)).get_round_type()
		var row := _definition(ammo_source, "ammo", ammo_name)
		assert_false(row.is_empty(), ammo_name + " has an installed ammo definition")
		staged_ammo += "\n" + row
	WorldFixture.write_file(_stage.path_join("weapon.def"), staged_weapons)
	WorldFixture.write_file(_stage.path_join("ammo.def"), staged_ammo)
	var world := WorldFixture.boot_minimal(self, _stage)
	world.set_process(false)
	var sim := world.get_sim()
	for _i in range(90):
		sim.step()
	var viewport := SubViewport.new()
	viewport.size = Vector2i(1024, 768)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.make_current()
	var background := ColorRect.new()
	background.size = viewport.size
	background.color = Color(0.12, 0.18, 0.22)
	viewport.add_child(background)
	var ui := Control.new()
	ui.size = viewport.size
	viewport.add_child(ui)
	var presenter := GameHudPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, null, ui)
	presenter.ensure_game_hud()
	presenter.set_hud_detail_level(0)
	var hud := presenter.get_game_hud()
	hud.configure(layout, art)
	var card := hud.get_node("SightsCard") as HudSightsCard
	for name: String in names:
		assert_true(world.set_local_player_weapon_by_name(name))
		for _i in range(90):
			sim.step()
		presenter.tick()
		assert_false(card.is_card_up(), name + " starts lowered")
		assert_true(sim.request_local_player_scope_toggle())
		for _i in range(100):
			sim.step()
		var view := sim.get_local_player_view()
		camera.global_position = view.camera_eye
		camera.look_at(view.camera_eye + Simulation.presentation_forward(
				view.camera_yaw_deg, view.camera_pitch_deg), Vector3.UP)
		camera.fov = view.fov_h_deg
		presenter.tick()
		assert_true(world.local_player_weapon_view().aimed_shot_available, name + " finishes raising")
		var weapon := weapons.get_weapon(weapons.find_weapon(name))
		card.set_weapon_sights(weapon.get_sights(), art)
		assert_eq(card.row_count(), weapon.get_sights().size())
		for row in card.get_children():
			assert_not_null(row.tex, name + " resolves its installed sight texture")
		var stats := hud.get_draw_list_stats()
		assert_gt(stats.glyphs, 0, "installed HUD fonts render")
		assert_false((hud.get_node("InsetScope") as HudInsetScope).is_scope_active(),
				"these installed weapons do not author Scoped + Inset")
		if name == "WPN_MORTAR":
			assert_true(stats.big_map_visible, "deploying the mortar opens its impact map")
			assert_gte(stats.big_map_lines, 96, "the live impact ring reaches the map renderer")
			# LollyPop (0x8000) draws a world marker; UseDesignator (0x200000)
			# adjusts the 2D ring. These are independent authored flags.
			if (weapon.get_flags() & 0x8000) == 0:
				assert_eq(stats.tris, 0, "this mortar has no authored world impact marker")
		else:
			assert_true(card.is_card_up(), name + " shows its authored optical card")
		if RenderingServer.get_rendering_device() != null:
			for size: Vector2i in [Vector2i(1024, 768), Vector2i(1920, 1080)]:
				viewport.size = size
				ui.size = size
				background.size = size
				presenter.tick()
				await get_tree().process_frame
				await get_tree().process_frame
				RenderingServer.force_draw(true)
				RenderingServer.force_sync()
				var image := viewport.get_texture().get_image()
				assert_eq(image.get_size(), size)
				var changed := 0
				for y in range(0, size.y, 8):
					for x in range(0, size.x, 8):
						if absf(image.get_pixel(x, y).r - background.color.r) + absf(image.get_pixel(x, y).g - background.color.g) > 0.1:
							changed += 1
				assert_gt(changed, 15, name + " produces visible HUD pixels")
				var capture := "user://hud-installed-captures"
				if not capture.is_empty():
					DirAccess.make_dir_recursive_absolute(capture)
					assert_eq(image.save_png(capture.path_join("%s-%dx%d.png" % [name, size.x, size.y])), OK)
		assert_true(sim.request_local_player_scope_toggle())
		for _i in range(100):
			sim.step()
		presenter.tick()
		assert_false(card.is_card_up(), name + " hides its card when lowered")
		assert_false(hud.get_draw_list_stats().big_map_visible, "lowering the mortar closes the map")

func test_installed_tank_and_pilot_hud_entry_exit() -> void:
	var art := _combat_root()
	if art == null:
		return
	var items := ItemDatabase.new()
	assert_eq(items.load_from_resource_root(art, "items.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(art, "weapon.def"), OK)
	var layout := HudPos.new()
	assert_eq(layout.load_from_resource_root(art, "hudpos.def"), OK)
	for item_id: int in [100164, 100165, 102010]:
		var seat_card := items.extract_seat_specs_for_item(art, item_id)
		var control_seat: EntityCardSeat = null
		for seat: EntityCardSeat in seat_card.get_seats():
			if seat.get_type() == 2 or seat.get_type() == 5:
				control_seat = seat
				break
		assert_not_null(control_seat, "%d has an authored controller/driver seat" % item_id)
		if control_seat == null:
			continue
		var mission := MissionData.new()
		assert_eq(mission.create_default(), OK)
		var vehicle_position := Vector3(0, 0, 30)
		assert_not_null(mission.add_entity(MissionData.KIND_ITEM, item_id, vehicle_position, Vector3.ZERO))
		var sim := Simulation.new()
		sim.enable_listen_server(true)
		sim.set_asset_root(art)
		assert_true(sim.install_seat_specs_for_type_ids(items, PackedInt32Array([item_id - 100000])))
		assert_true(sim.load_from_mission_data(mission))
		sim.resolve_item_traits(items)
		assert_eq(sim.load_weapon_table(art, "weapon.def"), OK)
		assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
		sim.set_local_player_weapon(weapons.get_weapon(weapons.find_weapon("WPN_M4AUTO")), {})
		var seat_position := vehicle_position + control_seat.get_local()
		MountLook.face(sim, seat_position, seat_position - Vector3(1, 0, 0))
		assert_true(sim.local_player_toggle_mount(), "%d boards its control seat" % item_id)
		for _i in range(50):
			sim.step()
		var view := sim.get_local_player_view()
		assert_true(view.mounted, "%d remains mounted" % item_id)
		var hud := HudOverlay.new()
		hud.size = Vector2(1024, 768)
		add_child_autofree(hud)
		hud.configure(layout, art)
		hud.set_hud_detail_level(0)
		hud.set_player_context(view)
		hud.set_weapon_state(true, 0, 0, 0, 0, false, false, false, 0)
		hud.set_combat_state(view, Transform3D.IDENTITY, Projection.IDENTITY, false, null, "E")
		var mounted_stats := hud.get_draw_list_stats()
		assert_gt(mounted_stats.glyphs, 0, "the control seat displays its gear label")
		if item_id == 102010:
			assert_gte(mounted_stats.lines, 9, "the pilot sees the AGL ladder and readout connectors")
		assert_true(sim.local_player_toggle_mount())
		sim.step()
		view = sim.get_local_player_view()
		assert_false(view.mounted)
		hud.set_player_context(view)
		hud.set_combat_state(view, Transform3D.IDENTITY, Projection.IDENTITY, false, null, "E")
		assert_lt(hud.get_draw_list_stats().glyphs, mounted_stats.glyphs,
				"leaving the vehicle removes its control/instrument labels")
