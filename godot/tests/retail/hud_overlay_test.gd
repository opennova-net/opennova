extends GutTest

# The runtime HudOverlay (native, over the engine HudFrameCompiler): configure
# from the reference fixture's hudpos.def (an earlier build's layout, not
# JO:CA's; those legs pend without the fixture set), feed typed per-frame
# state, and assert on the compiled draw list (get_draw_list_stats) plus the
# visible canvas geometry.
# The shell-side HudSightsCard child stack is covered here too.

const HudSightsCardScript := preload("res://game/world/hud_sights_card.gd")

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


# A scratch root carrying a synthetic hudpos.def and solid white textures.
class TempLayout:
	extends RefCounted
	var layout: HudPos
	var root: ResourceRoot
	var dir: String


func _load_temp_layout(lines: PackedStringArray, textures: PackedStringArray,
		texture_sizes: Dictionary = {}) -> TempLayout:
	var fixture := TempLayout.new()
	fixture.dir = TestFs.cache_dir(self, "hud_overlay")
	_temp_dirs.append(fixture.dir)
	for texture_name in textures:
		# Minimal uncompressed BGRA TGA, loaded through the overlay's real VFS path.
		var texture_size: Vector2i = texture_sizes.get(texture_name, Vector2i(2, 2))
		TestFs.write_bytes(self, fixture.dir.path_join(texture_name),
				TestFs.tga_bytes(texture_size))
	TestFs.write_text(self, fixture.dir.path_join("hudpos.def"), "\n".join(lines))
	fixture.layout = HudPos.new()
	assert_eq(fixture.layout.load(fixture.dir.path_join("hudpos.def")), OK)
	fixture.root = ResourceRoot.new()
	assert_eq(fixture.root.set_root_dir(fixture.dir), OK)
	return fixture


func _make_overlay() -> HudOverlay:
	var hud := HudOverlay.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	return hud


func _load_weapon(name: String) -> WeaponDef:
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK,
		"weapon.def fixture loads")
	var index := weapons.find_weapon(name)
	assert_gte(index, 0, "%s exists in the weapon.def fixture" % name)
	return weapons.get_weapon(index) if index >= 0 else null


func _set_hud_weapon(hud: HudOverlay, def: PlayerHudWeaponDef, display_name: String) -> void:
	hud.set_weapon(def.weapon_name, display_name, def.clipsize,
			def.rounds_per_icon, def.clipgfx_texture, def.clipgfx_offset,
			def.rndgfx_texture, def.rndgfx_offset, def.rndgfx_step)


func test_sighted_m4_builds_all_authored_sight_card_rows() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/hudpos.def"))
		return
	var weapon := _load_weapon("WPN_M4AUTO")
	assert_not_null(weapon)
	if weapon == null:
		return
	assert_eq(weapon.flags & 0x02000003, 0x2,
		"M4AUTO is Sighted and has neither Scoped nor NoCardSwitch")
	var sights := weapon.get_sights()
	assert_eq(sights.size(), 3, "M4AUTO retains all three authored SIGHTS rows")
	var textures := PackedStringArray()
	for sight: WeaponSightRow in sights:
		textures.append(sight.get_texture())
	assert_eq(textures, PackedStringArray([
		"car15aim.tga", "car15gls.tga", "reddot1.tga",
	]), "The final authored row is the committed M4 red-dot analogue")
	var reticle: WeaponSightRow = sights[2]
	assert_eq(reticle.get_blend(), 1, "The red-dot row keeps additive blend")
	assert_true(reticle.is_scale(), "The red-dot row keeps its scale flag")

	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), textures)
	var card := HudSightsCardScript.new()
	card.size = Vector2(1024, 768)
	add_child_autofree(card)
	card.set_weapon_sights(sights, fixture.root)
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
		var sight: WeaponSightRow = sights[i]
		# The rect the card draws is the engine's mode-resolved one (a `scale`
		# row shrinks about its centre at the default sight-scale index; a
		# standalone card carries slide multiplier 0), scaled through the
		# engine's witnessed per-corner pixel snap and aspect correction (HudPos.sight_scale_rect
		# [orig: Viewport_ScaleToVirtualCoords @0x5d2b20]), which replaced the
		# old .gd position*scale approximation.
		var design := sight.evaluate_rect(HudOverlay.sight_scale_index_default(), 0)
		assert_eq(card.row_rect(i), design,
			"SIGHTS row %d resolves its rect through the engine evaluator" % i)
		var expected := HudPos.sight_scale_rect(design, surface)
		assert_eq(RenderingServer.debug_canvas_item_get_rect(row.get_canvas_item()), expected,
			"SIGHTS row %d emits its mode-resolved draw rectangle" % i)
	# The scale-flagged red-dot row draws three quarters of its authored box
	# about its centre at the default index, never the full box.
	var reticle_x1 := float(reticle.get_x1())
	var reticle_y1 := float(reticle.get_y1())
	var reticle_authored := Rect2(Vector2(reticle_x1, reticle_y1),
		Vector2(float(reticle.get_x2()) - reticle_x1, float(reticle.get_y2()) - reticle_y1))
	assert_ne(card.row_rect(2), reticle_authored,
		"The scale-flagged red-dot row draws smaller than its authored box")
	# Each half-extent floors ((w * 3) >> 3), so the resolved width sits within
	# two pixels of three quarters.
	assert_almost_eq(card.row_rect(2).size.x, reticle_authored.size.x * 0.75, 2.0,
		"The default sight-scale index draws the red-dot row at three quarters width")
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
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/hudpos.def"))
		return
	var hud := _make_overlay()
	var hp := HudPos.new()
	assert_eq(hp.load(RetailData.fixture("def/hudpos.def")), OK, "Fixture loads.")
	hud.configure(hp, null) # null root -> layout only, no art/font
	assert_true(hud.is_configured())
	hud.set_player_state(0, 0.5, 1, 80.0)
	# The fixture authors HUDDECLUT_DMGBAR 0 1 1 0: the authored mask hides
	# the health bar at the default hud_detail level 0 and shows it at 1.
	# (retail: CRenderState_SetLayerVisibility @0x59B0F0; the DMGBAR slot cmp
	# @0x5A7C99 — see docs/interface/hud-re.md)
	var stats := hud.get_draw_list_stats()
	assert_eq(stats.quads, 0,
		"the fixture's level-0 declutter mask hides the health bar")
	hud.set_hud_detail_level(1)
	stats = hud.get_draw_list_stats()
	assert_eq(stats.quads_filled, 1, "0.5 health emits the fill quad")
	assert_eq(stats.quads_wire, 1, "and the wireframe border on top")
	assert_eq(stats.tris, 0, "no weapon -> no reticle")
	assert_eq(stats.glyphs, 0, "no font -> no text")
	hud.set_hud_detail_level(3)
	stats = hud.get_draw_list_stats()
	assert_eq(stats.elements_drawn, 0,
			"retail HUD detail 3 emits no gameplay HUD elements")
	assert_eq(stats.quads, 0)
	assert_eq(stats.tris, 0)
	assert_eq(stats.lines, 0)
	assert_eq(stats.glyphs, 0)
	assert_false(stats.map_visible)
	assert_false(stats.big_map_visible)
	assert_true(hud.visible,
			"declutter leaves the overlay mounted for PlayerViewEffects")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "HUD survives a draw with the fixture layout.")


func test_weapon_cluster_artless_safe() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/hudpos.def"))
		return
	# The weapon-coupled elements with a real weapon record but no art/root: text
	# has no font, and the clip indicator and crosshair skip cleanly.
	var hud := _make_overlay()
	var hp := HudPos.new()
	assert_eq(hp.load(RetailData.fixture("def/hudpos.def")), OK)
	hud.configure(hp, null)
	var weapon := WeaponDef.new()
	weapon.name = "WPN_AK47"
	weapon.clipsize = 30
	weapon.round_type = "AMMO_762"
	weapon.error = PackedFloat32Array([0.05, 0.2, 0.25, 0.05, 0.1, 0.15])
	weapon.hudclipgfx_texture = "H_clip.tga"
	weapon.hudclipgfx_offset = Vector2i(0, 0)
	weapon.hudrndgfx_texture = "H_round.tga"
	weapon.hudrndgfx_offset = Vector2i(9, 0)
	weapon.hudrndgfx_layout = Vector3i(18, 0, 1)
	_set_hud_weapon(hud, PlayerHudWeaponDef.from_weapon_def(weapon), "AK-47")
	hud.set_weapon_state(true, 12, 90, 0, 0, false, false, false, 0)
	await get_tree().process_frame
	var armed := hud.get_draw_list_stats()
	assert_eq(armed.tris, 0, "no crosshair texture resolves without a root: no reticle")
	assert_eq(armed.glyphs, 0, "no font resolves without a root: no ammo text")
	assert_eq(armed.quads, 0, "no clip or round art resolves without a root: no clip indicator")
	# A settled aimed shot hides the crosshair; clearing the weapon clears the cluster.
	hud.set_weapon_state(true, 12, 90, 0, 0, true, false, false, 0)
	assert_eq(hud.get_draw_list_stats().tris, 0,
		"a settled aimed shot emits no reticle")
	await get_tree().process_frame
	hud.clear_weapon()
	await get_tree().process_frame
	var cleared := hud.get_draw_list_stats()
	assert_eq(cleared.quads + cleared.tris + cleared.glyphs, 0,
		"clearing the weapon clears the whole cluster")
	assert_true(is_instance_valid(hud), "Weapon cluster draw is art-less safe.")
