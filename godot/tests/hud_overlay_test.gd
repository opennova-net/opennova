extends GutTest

# The runtime HudOverlay (native, over the engine HudFrameCompiler): configure
# from a real hudpos.def, feed typed per-frame state, and assert on the
# compiled draw list (get_draw_list_stats) plus the visible canvas geometry.
# The shell-side HudSightsCard child stack is covered here too.

const HUDPOS_PATH := "res://../fixtures/def/hudpos.def"
const WEAPON_PATH := "res://../fixtures/def/weapon.def"
const FONT_FIXTURE := "res://../fixtures/fnt/Gunpl22b.fnt"
const PlayerViewEffectsScript := preload("res://game/world/player_view_effects.gd")
const HudSightsCardScript := preload("res://game/world/hud_sights_card.gd")

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		for file_name in DirAccess.get_files_at(dir_path):
			DirAccess.remove_absolute(dir_path.path_join(file_name))
		DirAccess.remove_absolute(dir_path)
	_temp_dirs.clear()


func _load_temp_layout(lines: PackedStringArray, textures: PackedStringArray,
		texture_sizes: Dictionary = {}) -> Dictionary:
	var dir_path := OS.get_temp_dir().path_join("hud_overlay_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir_path), OK)
	_temp_dirs.append(dir_path)
	for texture_name in textures:
		# Minimal uncompressed BGRA TGA, loaded through the overlay's real VFS path.
		var texture_size: Vector2i = texture_sizes.get(texture_name, Vector2i(2, 2))
		var bytes := PackedByteArray()
		bytes.resize(18 + texture_size.x * texture_size.y * 4)
		bytes[2] = 2
		bytes[12] = texture_size.x & 0xff
		bytes[13] = (texture_size.x >> 8) & 0xff
		bytes[14] = texture_size.y & 0xff
		bytes[15] = (texture_size.y >> 8) & 0xff
		bytes[16] = 32
		bytes[17] = 0x28
		for i in range(18, bytes.size()):
			bytes[i] = 0xff
		var texture_file := FileAccess.open(dir_path.path_join(texture_name), FileAccess.WRITE)
		assert_not_null(texture_file)
		texture_file.store_buffer(bytes)
		texture_file.close()
	var def_file := FileAccess.open(dir_path.path_join("hudpos.def"), FileAccess.WRITE)
	assert_not_null(def_file)
	def_file.store_string("\n".join(lines))
	def_file.close()
	var layout := HudPos.new()
	assert_eq(layout.load(dir_path.path_join("hudpos.def")), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir_path), OK)
	return {"layout": layout, "root": root, "dir": dir_path}


func _copy_font_into(dir_path: String) -> void:
	var fnt_bytes := FileAccess.get_file_as_bytes(FONT_FIXTURE)
	assert_true(fnt_bytes.size() > 0, "font fixture present")
	var fnt := FileAccess.open(dir_path.path_join("Gunpl22b.fnt"), FileAccess.WRITE)
	fnt.store_buffer(fnt_bytes)
	fnt.close()


func _make_overlay() -> HudOverlay:
	var hud := HudOverlay.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	return hud


func _load_weapon(name: String) -> Dictionary:
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(ProjectSettings.globalize_path(WEAPON_PATH)), OK,
		"weapon.def fixture loads")
	var index := weapons.find_weapon(name)
	assert_gte(index, 0, "%s exists in the weapon.def fixture" % name)
	return weapons.get_weapon(index) if index >= 0 else {}


func _set_hud_weapon(hud: HudOverlay, def: PlayerHudWeaponDef, display_name: String) -> void:
	hud.set_weapon(def.weapon_name, display_name, def.round_type, def.clipsize,
			def.rounds_per_icon, def.clipgfx_texture, def.clipgfx_offset,
			def.rndgfx_texture, def.rndgfx_offset, def.rndgfx_step)


func test_sighted_m4_builds_all_authored_sight_card_rows() -> void:
	var weapon := _load_weapon("WPN_M4AUTO")
	assert_false(weapon.is_empty())
	if weapon.is_empty():
		return
	assert_eq(int(weapon.get("flags", 0)) & 0x02000003, 0x2,
		"M4AUTO is Sighted and has neither Scoped nor NoCardSwitch")
	var sights: Array = weapon.get("sights", [])
	assert_eq(sights.size(), 3, "M4AUTO retains all three authored SIGHTS rows")
	var textures := PackedStringArray()
	for entry in sights:
		var sight: Dictionary = entry
		textures.append(String(sight.get("texture", "")))
	assert_eq(textures, PackedStringArray([
		"car15aim.tga", "car15gls.tga", "reddot1.tga",
	]), "The final authored row is the committed M4 red-dot analogue")
	var reticle: Dictionary = sights[2]
	assert_eq(int(reticle.get("blend", -1)), 1, "The red-dot row keeps additive blend")
	assert_true(bool(reticle.get("scale", false)), "The red-dot row keeps its scale flag")

	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), textures)
	var card := HudSightsCardScript.new()
	card.size = Vector2(1024, 768)
	add_child_autofree(card)
	card.set_weapon_sights(sights, fixture["root"])
	card.set_card_up(true)
	await get_tree().process_frame

	assert_eq(card.row_count(), 3,
		"Settled ADS constructs every authored M4AUTO sight-card row")
	if card.row_count() != 3:
		return
	var surface := card.get_viewport_rect().size
	for i in range(sights.size()):
		var row := card.get_child(i) as Control
		assert_not_null(row, "SIGHTS row %d is a drawable Control" % i)
		assert_true(row.visible, "SIGHTS row %d is visible while the card is up" % i)
		RenderingServer.canvas_item_set_custom_rect(row.get_canvas_item(), false)
		var sight: Dictionary = sights[i]
		var x1 := float(sight.get("x1", 0))
		var y1 := float(sight.get("y1", 0))
		# The card scales through the engine's witnessed per-corner pixel snap
		# (HudPos.scale_rect [orig: Viewport_ScaleToVirtualCoords @0x5d2b20]),
		# which replaced the old .gd position*scale approximation.
		var expected := HudPos.scale_rect(Rect2(
			Vector2(x1, y1),
			Vector2(float(sight.get("x2", 0)) - x1,
				float(sight.get("y2", 0)) - y1)), surface)
		assert_eq(RenderingServer.debug_canvas_item_get_rect(row.get_canvas_item()), expected,
			"SIGHTS row %d emits its authored draw rectangle" % i)
	var reticle_row := card.get_child(2) as Control
	var reticle_material := reticle_row.material as CanvasItemMaterial
	assert_not_null(reticle_material, "The red-dot row gets a canvas material")
	if reticle_material != null:
		assert_eq(reticle_material.blend_mode, CanvasItemMaterial.BLEND_MODE_ADD,
			"The M4 red-dot layer renders additively")
	# The presenter folds the binocular suppression into the card switch.
	card.set_card_up(false)
	await get_tree().process_frame
	for i in range(sights.size()):
		assert_false((card.get_child(i) as Control).visible,
				"A lowered card hides authored SIGHTS row %d" % i)


func test_overlay_draws_health_from_fixture_layout() -> void:
	var hud := _make_overlay()
	var hp := HudPos.new()
	assert_eq(hp.load(ProjectSettings.globalize_path(HUDPOS_PATH)), OK, "Fixture loads.")
	hud.configure(hp, null) # null root -> layout only, no art/font
	assert_true(hud.is_configured())
	hud.set_player_state(0, 0.5, 1, 80.0)
	# The fixture authors HUDDECLUT_DMGBAR 0 1 1 0: the authored mask hides
	# the health bar at the default hud_detail level 0 and shows it at 1.
	# (retail: CRenderState_SetLayerVisibility @0x59B0F0; the DMGBAR slot cmp
	# @0x5A7C99 — see docs/interface/hud-re.md)
	var stats: Dictionary = hud.get_draw_list_stats()
	assert_eq(int(stats["quads"]), 0,
		"the fixture's level-0 declutter mask hides the health bar")
	hud.set_hud_detail_level(1)
	stats = hud.get_draw_list_stats()
	assert_eq(int(stats["quads_filled"]), 1, "0.5 health emits the fill quad")
	assert_eq(int(stats["quads_wire"]), 1, "and the wireframe border on top")
	assert_eq(int(stats["tris"]), 0, "no weapon -> no reticle")
	assert_eq(int(stats["glyphs"]), 0, "no font -> no text")
	hud.set_hud_detail_level(3)
	stats = hud.get_draw_list_stats()
	assert_eq(int(stats["elements_drawn"]), 0,
			"retail HUD detail 3 emits no gameplay HUD elements")
	assert_eq(int(stats["quads"]), 0)
	assert_eq(int(stats["tris"]), 0)
	assert_eq(int(stats["lines"]), 0)
	assert_eq(int(stats["glyphs"]), 0)
	assert_false(bool(stats["map_visible"]))
	assert_false(bool(stats["big_map_visible"]))
	assert_true(hud.visible,
			"declutter leaves the overlay mounted for PlayerViewEffects")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "HUD survives a draw with the fixture layout.")


func test_spinmap_compiles_terrain_retained_markers_and_waypoint() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDSPINMAPX1 810",
		"HUDSPINMAPX2 1020",
		"HUDSPINMAPY1 552",
		"HUDSPINMAPY2 762",
	]), PackedStringArray(["TSDicon.tga", "compring.tga", "dmgslice.tga"]), {
		"TSDicon.tga": Vector2i(64, 1920),
	})
	var hud := _make_overlay()
	hud.configure(fixture["layout"], fixture["root"])

	var terrain := TerrainData.new()
	terrain.set_sector_count(16)
	terrain.set_sector_rows(16)
	terrain.set_origin_x(0)
	terrain.set_origin_y(0)
	var sectors := PackedInt32Array()
	sectors.resize(256)
	sectors.fill(1)
	terrain.set_sector_grid(sectors)
	hud.set_minimap_terrain(terrain)
	# Waypoint 100 mission units ahead, 20 wu above -> the state line draws
	# with its tip cell, and the altitude nub frame is "above".
	hud.set_waypoint("Target", 1024, Vector2(100, 0), 20.0)

	# {version, stride, count}, then one retained overlay row:
	# {bank, handle, x, y, z, heading, icon, argb, flags, source,
	#  remaining_ticks, entity_known, policy_flags, half_x, half_y, floor,
	#  medic}.
	var snapshot := PackedInt32Array([
		4, 17, 1,
		0, 0x1001, 64 << 16, 0, 0, 0, 10, -16711936, 0, 0, 1984, 1,
		1, 0, 0, 6, 0,
	])
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false, snapshot)
	var stats: Dictionary = hud.get_draw_list_stats()
	assert_true(bool(stats["map_visible"]),
			"An authored HUDSPINMAP rect enables the gameplay spinmap.")
	assert_eq(int(stats["map_backing_tris"]), 32,
			"The circular backing is the portable 32-sided fan.")
	assert_gt(int(stats["map_terrain_tris"]), 0,
			"TerrainData's 16x16 sector routing reaches the minimap compiler.")
	assert_eq(int(stats["map_sprites"]), 3,
			"One live retained marker, the single waypoint tip cell, and the compass ring compile.")
	assert_eq(int(stats["map_lines"]), 1,
			"The waypoint state line reaches the draw list.")
	# Both label suppressors are BSS-zero in retail (LIVE by default): the
	# ring-edge distance label and the MAPCOORDS grid label compile with no
	# authored suppressor token.
	assert_eq(int(stats["map_labels"]), 2,
			"The distance + grid labels compile by default (BSS-zero suppressors).")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "The complete minimap pass renders safely.")
	stats = hud.get_draw_list_stats()
	assert_eq(int(stats["map_texture_filter"]), 4,
			"The spinmap icon strip uses explicit linear mip filtering.")
	assert_eq(int(stats["map_texture_repeat"]), 1,
			"The spinmap icon strip clamps past its half texel.")
	assert_true(bool(stats["map_icon_mipmaps"]),
			"The 64px TSDicon cells retain retail's box-filtered mip chain.")
	assert_eq(int(stats["map_icon_width"]), 64)
	assert_eq(int(stats["map_icon_height"]), 1920)
	assert_eq(int(stats["map_water_texture_filter"]), 2,
			"The spinmap thresholds the linearly sampled depthspin field.")
	assert_eq(int(stats["map_water_texture_repeat"]), 1,
			"The spinmap water field clamps at its authored edge.")

	# Unknown versions are rejected as a whole instead of partially walking a
	# stale or shorter row layout. The waypoint tip and compass sprites remain.
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			PackedInt32Array([99, 17, 1]))
	stats = hud.get_draw_list_stats()
	assert_eq(int(stats["map_sprites"]), 2,
			"A malformed snapshot contributes no retained marker rows.")
	await get_tree().process_frame

	# v4's medic bit: a LOCAL-TEAM medic's marker is the red-cross plate IN
	# PLACE of its blip — three overlay quads (six tris), no sprite
	# [orig: draw_entity_labels_and_markers @0x5a49e0 — the cross
	#  @0x5a4cd6..0x5a4d48 replacing the blip].
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			PackedInt32Array([
				4, 17, 1,
				0, 0x1001, 64 << 16, 0, 0, 0, 10, -16711936, 0, 0, 1984, 1,
				1, 0, 0, 6, 1,
			]))
	stats = hud.get_draw_list_stats()
	assert_eq(int(stats["map_sprites"]), 2,
			"A medic marker draws no blip sprite (waypoint tip + compass remain).")
	assert_eq(int(stats["map_footprint_tris"]), 6,
			"The medic marker is the three cross-plate quads as overlay tris.")
	await get_tree().process_frame

	# A footprint-class marker skips its icon quad and fills the static
	# polygon feed instead (the building/zone collision ground slice).
	hud.set_minimap_footprints(PackedInt32Array([
		1, 1,
		0x2042, -6250336, 6,
		20 << 16, -(4 << 16), 24 << 16, -(4 << 16), 22 << 16, 4 << 16,
		4,
		20 << 16, -(4 << 16), 24 << 16, -(4 << 16),
	]))
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			PackedInt32Array([
				4, 17, 1,
				0, 0x2042, 0, 0, 0, 0, 0, -6250336, 0, 0, 1984, 1,
				2, 0, 0, 6, 0,
			]))
	stats = hud.get_draw_list_stats()
	assert_gt(int(stats["map_footprint_tris"]), 0,
			"The footprint feed reaches the compiler's overlay list.")
	assert_eq(int(stats["map_sprites"]), 2,
			"The footprint marker draws no icon sprite.")
	await get_tree().process_frame

	# The M-cycle pass owns a separate canvas sandwich and carries the same
	# sampler contract as the corner spinmap.
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 3, false,
			PackedInt32Array([4, 17, 0]))
	await get_tree().process_frame
	stats = hud.get_draw_list_stats()
	assert_true(bool(stats["big_map_visible"]),
			"The stats seam reports the actual large-map draw list, not the corner map.")
	assert_eq(int(stats["big_map_backing_tris"]), 2,
			"The fullscreen large map owns its independent rectangular backing.")
	assert_gt(int(stats["big_map_terrain_tris"]), 0)
	assert_eq(int(stats["big_map_texture_filter"]), 4,
			"The enlarged map icon strip uses explicit linear mip filtering.")
	assert_eq(int(stats["big_map_texture_repeat"]), 1,
			"The enlarged map icon strip clamps at cell boundaries.")
	assert_eq(int(stats["big_map_water_texture_filter"]), 2,
			"The enlarged map thresholds the linearly sampled depthspin field.")
	assert_eq(int(stats["big_map_water_texture_repeat"]), 1,
			"The enlarged map water field clamps at its authored edge.")


func test_hud_color_index_round_trips_and_clamps() -> void:
	var hud := _make_overlay()
	assert_eq(hud.get_hud_color_index(), 2,
			"The retail config default is scheme 2 (hudpos hud_textcolor).")
	hud.set_hud_color_index(5)
	assert_eq(hud.get_hud_color_index(), 5, "Scheme 5 round-trips.")
	hud.set_hud_color_index(9)
	assert_eq(hud.get_hud_color_index(), 5, "Indexes clamp to the 0..5 table.")
	hud.set_hud_color_index(-3)
	assert_eq(hud.get_hud_color_index(), 0, "Negative indexes clamp to zero.")


func test_overlay_unconfigured_draws_nothing() -> void:
	var hud := HudOverlay.new()
	hud.size = Vector2(800, 600)
	add_child_autofree(hud)
	assert_false(hud.is_configured())
	hud.set_player_state(0, 1.0, 0, 80.0)
	var stats: Dictionary = hud.get_draw_list_stats()
	assert_eq(int(stats["quads"]), 0)
	assert_eq(int(stats["elements_drawn"]), 0)
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)
	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()), Rect2(),
		"HUD with no layout draws nothing without error.")


func test_stance_assets_use_explicit_ids_for_slots() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDSTANCEPOS 21 630",
		"ALPHAFADE 30 50 3",
		"HUDSTANCE 5 50 51 stance_6.tga PARACHUTE",
		"HUDSTANCE 0 10 11 stance_1.tga STAND",
		"HUDSTANCE 2 20 21 stance_old.tga PRONE_OLD",
		"HUDSTANCE 4 40 41 stance_5.tga EMPLACED",
		"HUDSTANCE 1 12 13 stance_2.tga CROUCH",
		"HUDSTANCE 2 22 23 stance_3.tga PRONE",
		"HUDSTANCE 3 30 31 stance_4.tga SITTING",
	]), PackedStringArray([
		"stance_1.tga", "stance_2.tga", "stance_old.tga",
		"stance_3.tga", "stance_4.tga", "stance_5.tga", "stance_6.tga",
	]))
	var hud := _make_overlay()
	hud.configure(fixture["layout"], fixture["root"])
	hud.set_player_state(100, 1.0, 2, 80.0)
	var stats: Dictionary = hud.get_draw_list_stats()
	assert_eq(int(stats["quads"]), 1, "one stance frame quad (no ghost at elapsed 0)")
	hud.queue_redraw()
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()),
		Rect2(43, 653, 128, 128),
		"Explicit IDs select slots independent of file order; the later ID 2 offset replaces the earlier one.")


func test_stance_index_bounds_safe() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDSTANCEPOS 21 630",
		"ALPHAFADE 30 50 3",
		"HUDSTANCE 0 10 11 s1.tga STAND",
		"HUDSTANCE 1 12 13 s2.tga CROUCH",
		"HUDSTANCE 2 22 23 s3.tga PRONE",
		"HUDSTANCE 3 30 31 s4.tga SITTING",
		"HUDSTANCE 4 40 41 s5.tga EMPLACED",
		"HUDSTANCE 5 50 51 s6.tga PARACHUTE",
	]), PackedStringArray(["s1.tga", "s2.tga", "s3.tga", "s4.tga", "s5.tga", "s6.tga"]))
	var hud := _make_overlay()
	hud.configure(fixture["layout"], fixture["root"])
	# Out-of-range stance must not crash the compile (including the cross-fade ghost).
	hud.set_player_state(10, 0.9, 99, 80.0)
	var stats: Dictionary = hud.get_draw_list_stats()
	assert_eq(int(stats["quads"]), 0, "an out-of-range stance frame draws nothing")
	await get_tree().process_frame
	hud.set_player_state(20, 0.9, 1, 80.0)
	stats = hud.get_draw_list_stats()
	assert_eq(int(stats["quads"]), 1, "returning in range draws the current frame only")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "Out-of-range stance index is draw-safe.")


func test_weapon_cluster_artless_safe() -> void:
	# The weapon-coupled elements with a real weapon record but no art/root: text
	# has no font, and the clip indicator and crosshair skip cleanly.
	var hud := _make_overlay()
	var hp := HudPos.new()
	assert_eq(hp.load(ProjectSettings.globalize_path(HUDPOS_PATH)), OK)
	hud.configure(hp, null)
	_set_hud_weapon(hud, PlayerHudWeaponDef.from_weapon_dict({
		"name": "WPN_AK47",
		"clipsize": 30,
		"round_type": "AMMO_762",
		"error": PackedFloat32Array([0.05, 0.2, 0.25, 0.05, 0.1, 0.15]),
		"hudclipgfx_texture": "H_clip.tga",
		"hudclipgfx_offset": Vector2i(0, 0),
		"hudrndgfx_texture": "H_round.tga",
		"hudrndgfx_offset": Vector2i(9, 0),
		"hudrndgfx_layout": Vector3i(18, 0, 1),
	}), "AK-47")
	hud.set_weapon_state(true, 12, 90, 0, 0, false, false, false, 0)
	await get_tree().process_frame
	# A settled aimed shot hides the crosshair; clearing the weapon clears the cluster.
	hud.set_weapon_state(true, 12, 90, 0, 0, true, false, false, 0)
	assert_eq(int(hud.get_draw_list_stats()["tris"]), 0,
		"a settled aimed shot emits no reticle")
	await get_tree().process_frame
	hud.clear_weapon()
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "Weapon cluster draw is art-less safe.")


func test_crosshair_requires_active_weapon() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga"]), {
		"cross01.tga": Vector2i(8, 8),
	})
	var hud := _make_overlay()
	hud.configure(fixture["layout"], fixture["root"])
	hud.set_weapon_state(false, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(int(hud.get_draw_list_stats()["tris"]), 0,
		"No weapon produces no retail crosshair draw commands.")
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(int(hud.get_draw_list_stats()["tris"]), 14,
		"An armed hip stance emits the five tapered regions (14 triangles).")


func test_crosshair_missing_texture_draws_nothing() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray())
	var hud := _make_overlay()
	hud.configure(fixture["layout"], fixture["root"])
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(int(hud.get_draw_list_stats()["tris"]), 0,
		"A missing crosshair texture produces no invented replacement reticle.")
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)
	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()), Rect2())


func test_binocular_view_hides_weapon_crosshair() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga"]), {
		"cross01.tga": Vector2i(8, 8),
	})
	var hud := _make_overlay()
	hud.configure(fixture["layout"], fixture["root"])
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	hud.set_view_state(true, Vector2.INF)
	assert_eq(int(hud.get_draw_list_stats()["tris"]), 0,
			"Binocular view hides the normal weapon crosshair without hiding the HUD.")
	hud.set_view_state(false, Vector2.INF)
	assert_eq(int(hud.get_draw_list_stats()["tris"]), 14,
			"Leaving the binocular view restores the reticle.")


func test_crosshair_style_clamps_and_reloads_live() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga", "cross25.tga"]), {
		"cross01.tga": Vector2i(2, 2),
		"cross25.tga": Vector2i(6, 6),
	})
	var hud := _make_overlay()
	hud.set_crosshair_style(99)
	hud.configure(fixture["layout"], fixture["root"])
	assert_eq(hud.get_crosshair_style(), HudOverlay.MAX_CROSSHAIR_STYLE)
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)
	var high_style_rect := RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item())
	assert_ne(high_style_rect, Rect2(), "Styles above 24 clamp to drawable cross25.tga.")

	hud.set_crosshair_style(-4)
	assert_eq(hud.get_crosshair_style(), HudOverlay.MIN_CROSSHAIR_STYLE)
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)
	var low_style_rect := RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item())
	assert_ne(low_style_rect, Rect2(), "Negative styles clamp to drawable cross01.tga.")
	assert_gt(high_style_rect.size.x, low_style_rect.size.x,
		"Changing style live-reloads the differently sized selected texture.")


func test_message_feed_draws_and_expires() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
	]), PackedStringArray())
	_copy_font_into(fixture["dir"])
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture["dir"]), OK)
	var hud := _make_overlay()
	hud.configure(fixture["layout"], root)
	hud.set_player_state(50, 1.0, 0, 80.0)
	hud.push_message("Move to the extraction point")
	assert_gt(int(hud.get_draw_list_stats()["glyphs"]), 0,
		"A pushed triggered-text line lays out glyph quads through the real .fnt.")
	await get_tree().process_frame
	# [orig: Chat_AddDebugMessage 930-tick life] — the line is gone at push+930.
	hud.set_player_state(50 + 930, 1.0, 0, 80.0)
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 0,
		"The 930-tick life expires the line.")
	assert_true(is_instance_valid(hud), "A pushed triggered-text line draws safely.")


# The friendly tags (D-HUD-20): the binding feeds projected tags through the
# compiler's element — FULL mode lays out centered fallback-name glyphs, the
# mode cycle clamps, BRIEF swaps to the tick lines, OFF draws nothing, and the
# medic flag adds the three cross-plate quads.
# [orig: HUD_DrawEntityLabel @0x5a39b0 via HUD_DrawFriendlyTagsPass @0x5a4480]
func test_friendly_tags_draw_modes() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"tagcolor_good 005,250,013",
	]), PackedStringArray())
	_copy_font_into(fixture["dir"])
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture["dir"]), OK)
	var hud := _make_overlay()
	hud.configure(fixture["layout"], root)
	assert_eq(hud.get_friendly_tag_mode(), 2, "the boot default mode is FULL")
	var screens := PackedVector2Array([Vector2(300, 200)])
	var dists := PackedFloat32Array([100.0])
	var names := PackedStringArray([""])
	var ids := PackedInt32Array([24]) # the fallback table's "SGT  Brown"
	var ratios := PackedInt32Array([0x10000])
	var flags := PackedInt32Array([0])
	hud.set_friendly_tags(screens, dists, names, ids, ratios, flags)
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 11,
		"FULL lays out the 11 fallback-name glyphs ('^SGT  Brown)")
	await get_tree().process_frame
	flags = PackedInt32Array([1]) # medic
	hud.set_friendly_tags(screens, dists, names, ids, ratios, flags)
	assert_eq(int(hud.get_draw_list_stats()["quads_filled"]), 3,
		"the medic plate adds the white square + two red cross bars")
	await get_tree().process_frame
	# The downed legs: dead (8) + slot (16) + a revive window (seconds << 8)
	# appends ": 87" to the name [orig: "%s: %ld" @0x5a400e].
	flags = PackedInt32Array([8 | 16 | (87 << 8)])
	ratios = PackedInt32Array([0])
	hud.set_friendly_tags(screens, dists, names, ids, ratios, flags)
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 15,
		"a downed slot entry appends the revive count to the label")
	await get_tree().process_frame
	ratios = PackedInt32Array([0x10000])
	hud.set_friendly_tag_mode(3)
	flags = PackedInt32Array([0])
	hud.set_friendly_tags(screens, dists, names, ids, ratios, flags)
	var brief: Dictionary = hud.get_draw_list_stats()
	assert_eq(int(brief["glyphs"]), 0, "BRIEF draws no text")
	assert_eq(int(brief["lines"]), 3, "BRIEF draws the three tick lines")
	await get_tree().process_frame
	# BRIEF draws the bare count above the ticks [orig: "%ld" @0x5a41f0].
	flags = PackedInt32Array([8 | 16 | (7 << 8)])
	ratios = PackedInt32Array([0])
	hud.set_friendly_tags(screens, dists, names, ids, ratios, flags)
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 1,
		"BRIEF draws the bare one-digit revive count")
	ratios = PackedInt32Array([0x10000])
	flags = PackedInt32Array([0])
	await get_tree().process_frame
	hud.set_friendly_tag_mode(0)
	hud.set_friendly_tags(screens, dists, names, ids, ratios, flags)
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 0, "OFF draws nothing")
	hud.set_friendly_tag_mode(99)
	assert_eq(hud.get_friendly_tag_mode(), 3, "the mode setter clamps to 0..3")
	assert_true(is_instance_valid(hud), "friendly tags draw safely")


# The end-of-round overlay element: the resolved Impact38 ladder centred on x
# 512 inside the safe-area stdbox; hidden draws nothing.
# [orig: draw_endround_stats_overlay @0x5b7cd0]
func test_end_round_overlay_draws_the_ladder() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
	]), PackedStringArray())
	_copy_font_into(fixture["dir"])
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture["dir"]), OK)
	var hud := _make_overlay()
	hud.configure(fixture["layout"], root)
	var before: Dictionary = hud.get_draw_list_stats()
	hud.set_end_round_overlay(true, 0, 768,
			PackedStringArray(["Mission Completed", "Blue Team : 12", "Game time : 0:01:05"]),
			PackedInt32Array([300, 414, 478]))
	var shown: Dictionary = hud.get_draw_list_stats()
	assert_gt(int(shown["glyphs"]), int(before["glyphs"]),
			"the ladder lays out its lines")
	assert_gt(int(shown["elements_drawn"]), int(before["elements_drawn"]),
			"the overlay element counts once")
	await get_tree().process_frame
	hud.set_end_round_overlay(false, 0, 768, PackedStringArray(), PackedInt32Array())
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), int(before["glyphs"]),
			"hidden draws nothing")
	assert_true(is_instance_valid(hud))


# The weapon heat bar draws at nonzero heat inside the HUDHEAT rect and stays
# hidden (draw-safe) at zero — the original's hudInfo+60 gate.
# [orig: HUD_DrawWeaponHeatBar @0x599700 gate @0x59970a]
func test_heat_bar_draws_at_nonzero_heat() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDHEAT 10 579 133 598",
		"HUDHEATBORDER 90,200,200,200",
		"stancecolor_bad 200,175,009,009",
	]), PackedStringArray())
	var hud := _make_overlay()
	hud.configure(fixture["layout"], fixture["root"])
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(int(hud.get_draw_list_stats()["quads"]), 0, "zero heat hides the bar")
	await get_tree().process_frame
	hud.set_weapon_state(true, -1, -1, 0x8000, 0, false, false, false, 0)
	var stats: Dictionary = hud.get_draw_list_stats()
	assert_eq(int(stats["quads_wire"]), 1, "the HUDHEATBORDER wireframe")
	assert_eq(int(stats["quads_filled"]), 1, "plus the proportional fill")
	await get_tree().process_frame
	hud.set_weapon_state(true, -1, -1, 0x20000, 0, false, false, false, 0) # above the clamp
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "The heat bar draws at any heat value.")


# The waypoint label draws name + distance at the HUDWPDINFO anchor for every
# alignment form (with a REAL .fnt so the glyph layout runs), and hides cleanly
# when the presenter clears the entry.
# [orig: HUD_DrawWaypointNameAndDistance @0x5947a0; retail authors align "right"]
func test_waypoint_label_draws_each_alignment() -> void:
	for align in ["right", "center", "left"]:
		var fixture := _load_temp_layout(PackedStringArray([
			"fonthud1_hi Gunpl22b.fnt",
			"HUDWPDINFO 1013,448,0,%s" % align,
		]), PackedStringArray())
		_copy_font_into(fixture["dir"])
		var root := ResourceRoot.new()
		assert_eq(root.set_root_dir(fixture["dir"]), OK)
		var hud := _make_overlay()
		hud.configure(fixture["layout"], root)
		hud.set_player_state(0, 1.0, 0, 80.0)
		# No waypoint entry: the label hides.
		assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 0,
			"no waypoint entry draws nothing (align %s)" % align)
		hud.set_waypoint("North Sea Village", 143)
		var stats: Dictionary = hud.get_draw_list_stats()
		assert_gt(int(stats["glyphs"]), 0,
			"name + distance lay out glyphs for align %s" % align)
		assert_eq(int(stats["quads_wire"]), 1,
			"the distance box wireframe draws for align %s" % align)
		await get_tree().process_frame
		# The nameless form still draws the distance.
		hud.set_waypoint("", 9)
		assert_gt(int(hud.get_draw_list_stats()["glyphs"]), 0,
			"a nameless entry still draws the distance for align %s" % align)
		await get_tree().process_frame
		hud.clear_waypoint()
		assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 0,
			"clearing the entry hides the label for align %s" % align)
		assert_true(is_instance_valid(hud), "waypoint label draws for align %s" % align)


func test_binocular_range_smoothing_matches_retail_steps() -> void:
	assert_eq(PlayerViewEffectsScript.smooth_range_value(-10, 1000), 1000,
			"Corrections larger than 1000 snap to the target.")
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 1000), 111)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 111), 33)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 33), 11)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 11), 3)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(0, 3), 1)
	assert_eq(PlayerViewEffectsScript.smooth_range_value(10, 1), 7,
			"Negative corrections use the same stepped easing.")
	assert_eq(PlayerViewEffectsScript.smooth_range_value(1, 0), 1,
			"The raw range target clamps to the retail 1..1000 interval.")


func test_player_view_effects_draw_retail_asset_stack() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray([
		"Binoculr.tga", "BinoCH.tga", "BNumbers.tga", "NVG.tga", "Nvgscale.tga",
	]), {
		"Binoculr.tga": Vector2i(512, 256),
		"BinoCH.tga": Vector2i(128, 128),
		"BNumbers.tga": Vector2i(16, 160),
		"NVG.tga": Vector2i(512, 512),
		"Nvgscale.tga": Vector2i(16, 80),
	})
	var effects := PlayerViewEffectsScript.new()
	effects.size = Vector2(1024, 768)
	add_child_autofree(effects)
	effects.set_resource_root(fixture["root"])
	effects.update_info({
		"binoculars_view_active": true,
		"binocular_range": 1000,
		"nvg_visible": true,
		"nvg_gain": 4,
	})
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(effects.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(effects.get_canvas_item()),
			Rect2(0, 0, 1024, 768),
			"Retail masks cover the viewport while inset art stays in design coordinates.")
	assert_eq(effects.get_child_count(true), 3,
			"The underwater murk, sun veil, and NVG post-processes are internal children.")
	var murk := effects.get_node("UnderwaterMurk") as ColorRect
	var veil := effects.get_node("SunVeil") as ColorRect
	var nvg := effects.get_node("NvgPost") as ColorRect
	assert_not_null(murk)
	assert_not_null(veil)
	assert_not_null(nvg)
	assert_lt(murk.get_index(true), veil.get_index(true),
			"Retail composites underwater murk before the sun-glare veil "
			+ "[orig: the veil draws in Render_ProcessMainSceneFrame @ 0x5cac4b, "
			+ "after the scene composites].")
	assert_lt(veil.get_index(true), nvg.get_index(true),
			"Retail composites underwater murk before later first-person HUD effects.")
	assert_not_null(veil.material as ShaderMaterial,
			"The veil rect samples the opennova_sun_veil_alpha global via its shader.")
	assert_true(nvg.visible,
			"First-person-visible NVG enables the post-process.")
	effects.update_info({"nvg_visible": false})
	assert_false(nvg.visible,
			"Camera suppression hides the post-process without consuming simulation state.")


func test_player_view_effects_tracks_exact_underwater_murk_pass() -> void:
	var effects := PlayerViewEffectsScript.new()
	effects.size = Vector2(1024, 768)
	add_child_autofree(effects)
	var env := MissionEnvironment.new()
	add_child_autofree(env)
	effects.set_environment(env)
	var murk := effects.get_node("UnderwaterMurk") as ColorRect
	assert_not_null(murk)
	assert_false(murk.visible, "The dry view has no murk scissor.")

	env.set_underwater_overlay_view(true)
	assert_true(murk.visible,
			"The render-pass edge updates synchronously even when the shell is frozen.")
	var lit := env.get_underwater_overlay_color()
	assert_true(Vector3(murk.color.r, murk.color.g, murk.color.b).is_equal_approx(lit))
	assert_almost_eq(murk.color.a, 204.0 / 255.0, 0.000001,
			"Default murk 0.8 becomes the witnessed alpha byte 204.")
	assert_true(murk.show_behind_parent,
			"The murk quad stays behind the ordinary HUD draw list.")

	env.set_underwater_overlay_view(false)
	assert_false(murk.visible, "Surfacing retires the murk quad synchronously.")


# The mounted-vehicle panel: the rider's VEHICLE_HUD block lands the interface
# silhouette at the HUDVEHSTANCEPOS anchor (plus the stance offset), loaded per
# sid through the real VFS path. With no Simulation the seat rows stay empty
# (the set_scoreboard shape); the silhouette is the panel's one witnessed gate
# [orig: HUD_DrawVehicleHealthBars @0x5a5038 — no interface texture, no panel].
func test_vehicle_panel_draws_the_block_silhouette() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDVEHSTANCEPOS 0 272",
		"HUDSTANCE 0 0 0 stance_1.tga STAND",
		"VEHICLE_HUD ",
		"  sid dbuggy1 ",
		"  interface h_buggya.tga",
		"  driver 16,196",
		"  seats 1,38,196",
		"VEHICLE_END",
	]), PackedStringArray(["h_buggya.tga", "stance_1.tga"]),
			{"h_buggya.tga": Vector2i(40, 30)})
	var hud := _make_overlay()
	var layout: HudPos = fixture["layout"]
	hud.configure(layout, fixture["root"])
	var block: Dictionary = layout.get_vehicle_hud("dbuggy1")
	assert_false(block.is_empty(), "The fixture's VEHICLE_HUD block resolves by sid.")
	var before := int(hud.get_draw_list_stats()["quads_textured"])
	hud.set_vehicle_panel(true, block, 0, null)
	assert_eq(int(hud.get_draw_list_stats()["quads_textured"]), before + 1,
			"The panel adds exactly the interface silhouette quad.")
	await get_tree().process_frame
	hud.set_vehicle_panel(false, {}, 0, null)
	assert_eq(int(hud.get_draw_list_stats()["quads_textured"]), before,
			"Hiding the panel removes the silhouette.")
	# A block whose interface art is missing draws NO panel at all.
	var missing := block.duplicate()
	missing["sid"] = "nosuch"
	missing["interface"] = "missing.tga"
	hud.set_vehicle_panel(true, missing, 0, null)
	assert_eq(int(hud.get_draw_list_stats()["quads_textured"]), before,
			"Without the interface texture the panel is skipped entirely.")


# The Recent Messages (J) window lists the CHAT ring with NO expiry gate: a
# chat line that has already faded off the HUD feed still lists in the window
# [orig: HUD_DrawMessageLog @0x5b9d70 walks slots 16..1 of both rings].
func test_message_log_lists_expired_lines() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"HUDCHATTEXT 142 , 711",
	]), PackedStringArray())
	_copy_font_into(fixture["dir"])
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture["dir"]), OK)
	var hud := _make_overlay()
	hud.configure(fixture["layout"], root)
	hud.set_player_state(50, 1.0, 0, 80.0)
	hud.push_chat_line("Taylor: moving to bravo", -1)
	assert_gt(int(hud.get_draw_list_stats()["glyphs"]), 0,
			"A pushed chat line lays out glyph quads on the HUDCHATTEXT feed.")
	await get_tree().process_frame
	# Past the 930-tick life the feed loop drops the line...
	hud.set_player_state(50 + 930, 1.0, 0, 80.0)
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 0,
			"The 930-tick life expires the chat line off the feed.")
	# ...but the window still lists it.
	hud.set_message_log_title("Recent Messages")
	hud.set_message_log_shown(true)
	assert_gt(int(hud.get_draw_list_stats()["glyphs"]), 0,
			"The Recent Messages window lists the expired chat line (no expiry gate).")
	await get_tree().process_frame
	hud.set_message_log_shown(false)
	assert_eq(int(hud.get_draw_list_stats()["glyphs"]), 0,
			"Closing the window hides the history again.")


# The AAS zone status panel's device seam: the anchor + atlases load through
# the real VFS path, a null Simulation leaves no zone rows (nothing draws),
# and hiding clears the state [orig: HUD_DrawZoneStatusPanel @0x5a2480 draws
# nothing without a contested zone].
func test_lfp_panel_device_seam() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"LFP_FLAGS 1020 , 27",
	]), PackedStringArray(["JO_LFP.tga", "R_LFP.tga", "N_LFP.tga", "lfp_alf.tga",
			"lfp_dlf.tga"]),
			{"JO_LFP.tga": Vector2i(64, 256), "R_LFP.tga": Vector2i(64, 256),
			 "N_LFP.tga": Vector2i(64, 256), "lfp_alf.tga": Vector2i(36, 36),
			 "lfp_dlf.tga": Vector2i(36, 36)})
	_copy_font_into(fixture["dir"])
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture["dir"]), OK)
	var hud := _make_overlay()
	hud.configure(fixture["layout"], root)
	var before: Dictionary = hud.get_draw_list_stats()
	hud.set_lfp_panel(true, NetProtocol.GAME_TYPE_ADVANCE_AND_SECURE, 1, 0,
			{"under_attack": "!Under\nAttack!!", "ready": "!Ready for\nTakeover!"}, null)
	var shown: Dictionary = hud.get_draw_list_stats()
	assert_eq(int(shown["elements_drawn"]), int(before["elements_drawn"]),
			"With no zone rows the panel draws nothing.")
	await get_tree().process_frame
	hud.set_lfp_panel(false, 0, 0, 0, {}, null)
	assert_true(is_instance_valid(hud), "Hiding the zone panel is safe.")
