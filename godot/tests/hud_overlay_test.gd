extends GutTest

# The runtime HudOverlay (native, over the engine HudFrameCompiler): configure
# from authored hudpos.def text (the reference fixture's legs are in
# retail/hud_overlay_test.gd), feed typed per-frame state, and assert on the
# compiled draw list (get_draw_list_stats) plus the visible canvas geometry.
# The shell-side HudSightsCard child stack is covered here too.

const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"  # staged as Gunpl22b.fnt
const PlayerViewEffectsScript := preload("res://game/world/player_view_effects.gd")

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
	# Every gated element shown first (an unauthored HUDDECLUT row hides its
	# slot, D-HUD-54); a test's own row for a slot comes later and stands.
	TestFs.write_text(self, fixture.dir.path_join("hudpos.def"),
			"\n".join(PackedStringArray(HudFixture.DECLUTTER_ROWS) + lines))
	fixture.layout = HudPos.new()
	assert_eq(fixture.layout.load(fixture.dir.path_join("hudpos.def")), OK)
	fixture.root = ResourceRoot.new()
	assert_eq(fixture.root.set_root_dir(fixture.dir), OK)
	return fixture


func _copy_font_into(dir_path: String) -> void:
	TestFs.copy(self, FONT_FIXTURE, dir_path.path_join("Gunpl22b.fnt"))


func _make_overlay() -> HudOverlay:
	var hud := HudOverlay.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	return hud


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
	hud.configure(fixture.layout, fixture.root)

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
	#  medic, team, zone_number, def_type, entity_bits, zone_index,
	#  zone_radius, entity_x, entity_y, anchor_x, anchor_y, bound_radius,
	#  entity_z, bay_groups, timer_flags, timer_team, timer_bar_team,
	#  timer_value, timer_limit, timer_rate}.
	# entity_bits 1: the slot entity carries a model (the persistent bank
	# draws only those); the blip centres on its anchor.
	var snapshot := PackedInt32Array([
		7, 36, 1,
		0, 0x1001, 64 << 16, 0, 0, 0, 10, -16711936, 0, 0, 1984, 1,
		1, 0, 0, 6, 0,
		0, 0, 0, 1, -1, 0, 64 << 16, 0, 64 << 16, 0, 0,
		0, 0,
		0, 0, 0, 0, 0, 0,
	])
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false, snapshot)
	var stats := hud.get_draw_list_stats()
	assert_true(stats.map_visible,
			"An authored HUDSPINMAP rect enables the gameplay spinmap.")
	assert_gt(stats.map_terrain_tris, 0,
			"TerrainData's 16x16 sector routing reaches the minimap compiler.")
	assert_eq(stats.map_sprites, 3,
			"One live retained marker, the single waypoint tip cell, and the compass ring compile.")
	assert_eq(stats.map_lines, 1,
			"The waypoint state line reaches the draw list.")
	# Both label suppressors are BSS-zero in retail (LIVE by default): the
	# ring-edge distance label and the MAPCOORDS grid label compile with no
	# authored suppressor token.
	assert_eq(stats.map_labels, 2,
			"The distance + grid labels compile by default (BSS-zero suppressors).")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "The complete minimap pass renders safely.")
	stats = hud.get_draw_list_stats()
	assert_eq(stats.map_texture_filter, 4,
			"The spinmap icon strip's sampler reaches its mip chain (the shader takes the nearest level).")
	assert_eq(stats.map_texture_repeat, 1,
			"The spinmap icon strip clamps past its half texel.")
	assert_true(stats.map_icon_mipmaps,
			"The 64px TSDicon cells retain retail's box-filtered mip chain.")
	assert_eq(stats.map_icon_width, 64)
	assert_eq(stats.map_icon_height, 1920)
	assert_eq(stats.map_water_texture_filter, 2,
			"The spinmap thresholds the linearly sampled depthspin field.")
	assert_eq(stats.map_water_texture_repeat, 1,
			"The spinmap water field clamps at its authored edge.")

	# Unknown versions are rejected as a whole instead of partially walking a
	# stale or shorter row layout. The waypoint tip and compass sprites remain.
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			PackedInt32Array([99, 17, 1]))
	stats = hud.get_draw_list_stats()
	assert_eq(stats.map_sprites, 2,
			"A malformed snapshot contributes no retained marker rows.")
	await get_tree().process_frame

	# v4's medic bit: a LOCAL-TEAM medic's marker is the red-cross plate IN
	# PLACE of its blip — three overlay quads (six tris), no sprite
	# [orig: HUD_DrawEntityLabelsAndMarkers @0x5a49e0 — the cross
	#  @0x5a4cd6..0x5a4d48 replacing the blip].
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			PackedInt32Array([
				7, 36, 1,
				0, 0x1001, 64 << 16, 0, 0, 0, 10, -16711936, 0, 0, 1984, 1,
				1, 0, 0, 6, 1,
				0, 0, 0, 1, -1, 0, 64 << 16, 0, 64 << 16, 0, 0,
				0, 0,
				0, 0, 0, 0, 0, 0,
			]))
	stats = hud.get_draw_list_stats()
	assert_eq(stats.map_sprites, 2,
			"A medic marker draws no blip sprite (waypoint tip + compass remain).")
	assert_eq(stats.map_footprint_tris, 6,
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
				7, 36, 1,
				0, 0x2042, 0, 0, 0, 0, 0, -6250336, 0, 0, 1984, 1,
				2, 0, 0, 6, 0,
				0, 0, 0, 1, -1, 0, 0, 0, 0, 0, 0,
				0, 0,
				0, 0, 0, 0, 0, 0,
			]))
	stats = hud.get_draw_list_stats()
	assert_gt(stats.map_footprint_tris, 0,
			"The footprint feed reaches the compiler's overlay list.")
	assert_eq(stats.map_sprites, 2,
			"The footprint marker draws no icon sprite.")
	await get_tree().process_frame

	# The M-cycle pass owns a separate canvas sandwich and carries the same
	# sampler contract as the corner spinmap.
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 3, false,
			PackedInt32Array([7, 36, 0]))
	await get_tree().process_frame
	stats = hud.get_draw_list_stats()
	assert_true(stats.big_map_visible,
			"The stats seam reports the actual large-map draw list, not the corner map.")
	assert_gt(stats.big_map_terrain_tris, 0)
	assert_eq(stats.big_map_texture_filter, 4,
			"The enlarged map icon strip uses explicit linear mip filtering.")
	assert_eq(stats.big_map_texture_repeat, 1,
			"The enlarged map icon strip clamps at cell boundaries.")
	assert_eq(stats.big_map_water_texture_filter, 2,
			"The enlarged map thresholds the linearly sampled depthspin field.")
	assert_eq(stats.big_map_water_texture_repeat, 1,
			"The enlarged map water field clamps at its authored edge.")


# Every textured map sprite's material is colour family 0x600 (TSDicon 0x651,
# compring 0x651): the corner map's top item receives each with the +8 UV.x
# flag and the raw diffuse, the blip's team colour unfolded (D-HUD-49)
# [orig: Render_DrawIconStripCell_Debug @0x67bae0; HUD_DrawCompassIndicator
# @0x59c900].
func test_map_sprites_submit_their_materials_modulate2x_flag() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"HUDSPINMAPX1 810",
		"HUDSPINMAPX2 1020",
		"HUDSPINMAPY1 552",
		"HUDSPINMAPY2 762",
	]), PackedStringArray(["TSDicon.tga", "compring.tga"]), {
		"TSDicon.tga": Vector2i(64, 1920), "compring.tga": Vector2i(32, 32),
	})
	var hud := _make_overlay()
	hud.configure(fixture.layout, fixture.root)
	# One live marker in a half-bright blue the device doubles.
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false, PackedInt32Array([
		7, 36, 1,
		0, 0x1001, 64 << 16, 0, 0, 0, 10, 0xFF20407F - 0x100000000, 0, 0, 1984, 1,
		1, 0, 0, 6, 0,
		0, 0, 0, 1, -1, 0, 64 << 16, 0, 64 << 16, 0, 0,
		0, 0,
		0, 0, 0, 0, 0, 0,
	]))
	await get_tree().process_frame
	var blips := 0
	var compass := 0
	for row: Dictionary in hud.get_map_submissions():
		if row.size != Vector2i(64, 1920) and row.size != Vector2i(32, 32):
			continue
		for uv: Vector2 in row.uvs:
			assert_gte(uv.x, MAP_MODULATE2X_FLAG * 0.5,
					"a %s sprite vertex carries the MODULATE2X flag" % row.size)
		if row.size == Vector2i(64, 1920):
			blips += 1
			assert_eq(row.colors[0].to_argb32(), 0xFF20407F,
					"the blip submits the raw marker colour")
		else:
			compass += 1
			assert_eq(row.colors[0], Color.WHITE, "the compass submits its raw white diffuse")
	assert_eq(blips, 1, "the TSDicon run reaches the top item")
	assert_eq(compass, 1, "the compass run reaches the top item")


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
	var stats := hud.get_draw_list_stats()
	assert_eq(stats.quads, 0)
	assert_eq(stats.elements_drawn, 0)
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
	hud.configure(fixture.layout, fixture.root)
	hud.set_player_state(100, 1.0, 2, 80.0)
	var stats := hud.get_draw_list_stats()
	assert_eq(stats.quads, 1, "one stance frame quad (no ghost at elapsed 0)")
	hud.queue_redraw()
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(hud.get_canvas_item(), false)

	# The draw list's window coordinates land on this raster's pixel centres
	# half a pixel over (HudPos.d3d9_screen_offset, D3D9's).
	assert_eq(RenderingServer.debug_canvas_item_get_rect(hud.get_canvas_item()),
		Rect2(Vector2(43, 653) + HudPos.d3d9_screen_offset(), Vector2(128, 128)),
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
	hud.configure(fixture.layout, fixture.root)
	# Out-of-range stance must not crash the compile (including the cross-fade ghost).
	hud.set_player_state(10, 0.9, 99, 80.0)
	var stats := hud.get_draw_list_stats()
	assert_eq(stats.quads, 0, "an out-of-range stance frame draws nothing")
	await get_tree().process_frame
	hud.set_player_state(20, 0.9, 1, 80.0)
	stats = hud.get_draw_list_stats()
	assert_eq(stats.quads, 1, "returning in range draws the current frame only")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "Out-of-range stance index is draw-safe.")


func test_crosshair_requires_active_weapon() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga"]), {
		"cross01.tga": Vector2i(8, 8),
	})
	var hud := _make_overlay()
	hud.configure(fixture.layout, fixture.root)
	hud.set_weapon_state(false, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(hud.get_draw_list_stats().tris, 0,
		"No weapon produces no retail crosshair draw commands.")
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(hud.get_draw_list_stats().tris, 14,
		"An armed hip stance emits the five tapered regions (14 triangles).")


# install_weapon is set_weapon over a PlayerHudWeaponDef's slice (the
# presenter's weapon change), and clear_weapon for none: the clip graphic and
# one round icon per round in the clip draw at the HUDCLIP anchor.
func test_install_weapon_takes_the_weapon_slice() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
		"HUDCLIP 100 600",
	]), PackedStringArray(["clip.tga", "rnd.tga"]))
	var weapon := PlayerHudWeaponDef.new()
	weapon.weapon_name = "TESTGUN"
	weapon.clipsize = 30
	weapon.rounds_per_icon = 1
	weapon.clipgfx_texture = "clip.tga"
	weapon.rndgfx_texture = "rnd.tga"
	weapon.rndgfx_step = Vector2i(4, 0)
	var hud := _make_overlay()
	hud.configure(fixture.layout, fixture.root)
	hud.install_weapon(weapon, null)
	hud.set_weapon_state(true, 5, 90, 0, 0, false, false, false, 0)
	var installed: int = hud.get_draw_list_stats().quads_textured
	assert_eq(installed, 6, "The clip graphic and five round icons.")
	hud.install_weapon(null, null)
	hud.set_weapon_state(true, 5, 90, 0, 0, false, false, false, 0)
	assert_eq(hud.get_draw_list_stats().quads_textured, 0, "No weapon draws no clip art.")
	hud.set_weapon(weapon.weapon_name, "", 30, 1, "clip.tga", Vector2i.ZERO, "rnd.tga",
			Vector2i.ZERO, Vector2i(4, 0))
	hud.set_weapon_state(true, 5, 90, 0, 0, false, false, false, 0)
	assert_eq(hud.get_draw_list_stats().quads_textured, installed,
			"The same draw as set_weapon over the slice's fields.")


# A hudpos.def with no key (no byte, a comment alone, or no file at all) runs the
# HUD over the globals no arm wrote: the HUDDECLUT mask table stays zero and
# every gated element hides, the armed crosshair among them, where the overlay
# used to keep everything shown (D-HUD-54; the hud_layout ctest walks every
# element). A file with the XHAIRS row alone shows it, and only it.
func test_a_hudpos_with_no_key_hides_every_gated_element() -> void:
	for text: String in ["", "// The HUD layout: nothing yet.\r\n", "<missing>", "HUDDECLUT_XHAIRS 1 1 1 1\r\n"]:
		var dir := TestFs.cache_dir(self, "hud_overlay_no_key")
		_temp_dirs.append(dir)
		TestFs.write_bytes(self, dir.path_join("cross01.tga"), TestFs.tga_bytes(Vector2i(8, 8)))
		if text != "<missing>":
			TestFs.write_bytes(self, dir.path_join("hudpos.def"), text.to_utf8_buffer())
		var root := ResourceRoot.new()
		assert_eq(root.set_root_dir(dir), OK)
		var layout := HudPos.new()
		var loaded := layout.load_from_resource_root(root, "hudpos.def")
		assert_eq(loaded, ERR_FILE_NOT_FOUND if text == "<missing>" else OK,
				"%s: an empty file is a file of no line, not an error" % text)
		var hud := _make_overlay()
		hud.configure(layout, root)
		assert_true(hud.is_configured(), "%s: the HUD runs without a layout" % text)
		hud.set_player_state(100, 1.0, 0, 80.0)
		hud.set_weapon_state(true, 30, 90, 0, 0, false, false, false, 0)
		var shown := text.begins_with("HUDDECLUT_XHAIRS")
		assert_eq(hud.get_draw_list_stats().tris, 14 if shown else 0,
				"%s: the crosshair draws only when a row shows it" % text)


# A texture whose material word is colour family 0x600 draws under
# MODULATE2X(TEXTURE, DIFFUSE), twice texel x vertex colour, saturated, whatever
# loader read it and whatever element draws it (D-HUD-49): the flat HUD's
# commands carry the shader's +16 UV.y flag, the map's sprites its +8 UV.x flag,
# and the vertex colour stays the raw diffuse [orig:
# RenderState_DecodeModeColorStage @0x681080, `& 0x3F00` @0x68113a].
const FLAT_MODULATE2X_FLAG := 16.0
const MAP_MODULATE2X_FLAG := 8.0


# The flat submission rows drawn from the texture of `size`.
func _flat_rows(hud: HudOverlay, size: Vector2i) -> Array:
	var out := []
	for row: Dictionary in hud.get_flat_submissions():
		if row.size == size:
			out.append(row)
	return out


# A row the flat shader doubles: a triangle array whose every UV carries the flag.
func _flat_doubled(row: Dictionary) -> bool:
	if row.kind != "triangles" or row.uvs.is_empty():
		return false
	for uv: Vector2 in row.uvs:
		if uv.y < FLAT_MODULATE2X_FLAG * 0.5:
			return false
	return true


# The shaders decode exactly the flags the submissions write: the flat shader
# strips +16 off UV.y at >= 8 and doubles the colour (before any second stage,
# as the device's stage 0 precedes stage 1), the map shader +8 off UV.x at >= 4.
func test_shaders_decode_the_submitted_modulate2x_flags() -> void:
	var flat := HudOverlay.flat_shader_code()
	assert_true(flat.contains("if (UV.y >= 8.0) {\n\t\tUV.y -= 16.0;\n\t\tmodulate2x_on = 1.0;"),
			"the flat shader strips the +16 UV.y flag")
	var fragment := flat.substr(flat.find("void fragment()"))
	var doubling := fragment.find(
			"if (modulate2x_on > 0.5) {\n\t\tCOLOR.rgb = min(COLOR.rgb * 2.0, vec3(1.0));")
	assert_gt(doubling, 0, "the flagged command's colour doubles, saturated")
	assert_gt(fragment.find("if (stage1_on > 0.5)"), doubling,
			"the stage-0 doubling runs before the second stage")
	var map := HudOverlay.map_shader_code()
	assert_true(map.contains("if (UV.x >= 4.0) {\n\t\tUV.x -= 8.0;\n\t\tmodulate2x_on = 1.0;"),
			"the map shader strips the +8 UV.x flag")
	assert_true(map.contains(
			"if (modulate2x_on > 0.5) {\n\t\tCOLOR.rgb = min(COLOR.rgb * 2.0, vec3(1.0));"),
			"the flagged sprite's colour doubles, saturated")
	# The fixed-function stage's MIPFILTER POINT: the nearest level, bounded by
	# the strip's last retail level (renderer::TextureStage::MapIconStrip).
	assert_true(map.contains("COLOR = vertex_color * textureLod(TEXTURE, UV, level);"),
			"the map pass samples the nearest mip level")
	assert_true(map.contains("max(icon_max_lod, 0.0)"),
			"the level stops at the strip's last retail level")


# Every glyph draws through its font page's material, 0x651: MODULATE2X(TEXTURE,
# DIFFUSE), so the half-bright drawers' halved colour reads at full brightness
# (D-HUD-51). The flat HUD's glyph runs carry the +16 UV.y flag and the map's its
# +8 UV.x flag, each at the drawer's raw halved diffuse [orig: GameFont_LoadFromBlob
# @0x674825 (0x651 per page); CGameFont_DrawText @0x6752c0 binds it per page run;
# sub_580560 @0x59aeab halves the system ring's white].
func test_glyph_runs_draw_through_the_font_pages_modulate2x() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"HUDSPINMAPX1 810",
		"HUDSPINMAPX2 1020",
		"HUDSPINMAPY1 552",
		"HUDSPINMAPY2 762",
	]), PackedStringArray(["TSDicon.tga", "compring.tga"]), {
		"TSDicon.tga": Vector2i(64, 1920),
	})
	_copy_font_into(fixture.dir)
	# A fresh root sees the font copied in after the fixture's scan.
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	hud.set_player_state(50, 1.0, 0, 80.0)
	hud.push_message("Move to the extraction point")
	await get_tree().process_frame
	var runs := 0
	for row: Dictionary in hud.get_flat_submissions():
		if row.kind != "glyphs":
			continue
		runs += 1
		assert_eq(row.size, Vector2i(256, 256), "a glyph run samples its 256x256 font page")
		for uv: Vector2 in row.uvs:
			assert_gte(uv.y, FLAT_MODULATE2X_FLAG * 0.5, "every glyph vertex carries the flag")
		for c: Color in row.colors:
			assert_eq(c.to_argb32(), 0xFF7F7F7F,
					"the white line's diffuse is the drawer's halved white, doubled on the device")
	assert_gt(runs, 0, "the system ring's line submits its glyph run")

	# The spinmap's distance and grid labels: the map pass's flag, the raw halved
	# diffuse (the compile-side doubling is gone).
	var terrain := TerrainData.new()
	terrain.set_sector_count(16)
	terrain.set_sector_rows(16)
	var sectors := PackedInt32Array()
	sectors.resize(256)
	sectors.fill(1)
	terrain.set_sector_grid(sectors)
	hud.set_minimap_terrain(terrain)
	hud.set_waypoint("Target", 1024, Vector2(100, 0), 20.0)
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false, PackedInt32Array([
		7, 36, 1,
		0, 0x1001, 64 << 16, 0, 0, 0, 10, -16711936, 0, 0, 1984, 1,
		1, 0, 0, 6, 0,
		0, 0, 0, 1, -1, 0, 64 << 16, 0, 64 << 16, 0, 0,
		0, 0,
		0, 0, 0, 0, 0, 0,
	]))
	assert_gt(hud.get_draw_list_stats().map_labels, 0, "the distance and grid labels compile")
	await get_tree().process_frame
	var map_runs := 0
	for row: Dictionary in hud.get_map_submissions():
		if not row.has("page"):
			continue
		map_runs += 1
		for uv: Vector2 in row.uvs:
			assert_gte(uv.x, MAP_MODULATE2X_FLAG * 0.5, "every map glyph vertex carries the flag")
		for c: Color in row.colors:
			assert_true(c.r8 <= 127 and c.g8 <= 127 and c.b8 <= 127,
					"a map label's diffuse is the drawer's halved colour (%s)" % c)
	assert_gt(map_runs, 0, "the map labels submit their glyph runs")


# Every HUD texture draws through its maker's material word, not its loader's:
# the file loader's capture-point icons and waypoint indicator (0x300631) double
# like the HUD loader's colour mode (0x651) [orig: HUD_LoadAllTextures @0x59dda0 —
# TSDicon 0x651 @0x59e042, WPIndctr 0x300631 @0x59e056, JO_LFP / R_LFP / N_LFP
# 0x300631 @0x59e09e / @0x59e0b7 / @0x59e0d0; BoxTexture_LoadAndSetupUVRegions
# 0x651 @0x56af2d; CTipSystem_Init 0x651 @0x5b69bc; CNetworkIcons_LoadTextures
# 0x300451 @0x4c2d53].
func test_hud_textures_take_their_makers_material_words() -> void:
	var names := PackedStringArray(["JO_LFP.tga", "R_LFP.tga", "N_LFP.tga", "WPIndctr.tga",
			"TSDicon.tga", "border.tga", "boxtile.tga", "border3.tga", "k_tip.tga", "g_tip.tga",
			"neticon1.tga", "neticon2.tga", "neticon3.tga", "compring.tga", "lfp_alf.tga"])
	var fixture := _load_temp_layout(PackedStringArray(["ALPHAFADE 30 50 3"]), names)
	var hud := _make_overlay()
	hud.configure(fixture.layout, fixture.root)
	for name in ["JO_LFP.tga", "R_LFP.tga", "N_LFP.tga", "WPIndctr.tga"]:
		assert_eq(hud.get_texture_material_word(name), 0x300631,
				"%s draws through its maker's 0x300631, colour family 0x600" % name)
	for name in ["TSDicon.tga", "border.tga", "border3.tga", "k_tip.tga", "g_tip.tga",
			"compring.tga", "lfp_alf.tga"]:
		assert_eq(hud.get_texture_material_word(name), 0x651, "%s draws through 0x651" % name)
	for name in ["neticon1.tga", "neticon2.tga", "neticon3.tga"]:
		assert_eq(hud.get_texture_material_word(name), 0x300451,
				"%s draws through 0x300451, SELECTARG1(TEXTURE)" % name)
	assert_eq(hud.get_texture_material_word("boxtile.tga"), 0,
			"the camo is only ever the combined material's second stage")


# The triangle class: the crosshair's tapered regions (cross%02d.tga, colour mode).
func test_colour_mode_crosshair_draws_under_modulate2x() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga"]), {
		"cross01.tga": Vector2i(8, 8),
	})
	var hud := _make_overlay()
	hud.configure(fixture.layout, fixture.root)
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	var rows := _flat_rows(hud, Vector2i(8, 8))
	assert_eq(rows.size(), 1, "the crosshair's triangles submit as one run")
	var vertices := 0
	for row: Dictionary in rows:
		assert_true(_flat_doubled(row), "every crosshair vertex carries the MODULATE2X flag")
		vertices += row.uvs.size()
	assert_eq(vertices, 14 * 3, "the run holds all fourteen triangles")


# The quad class: the hudpos STATICFRAME background (colour mode), submitted as
# a flagged triangle pair at the raw white diffuse.
func test_colour_mode_static_frame_draws_under_modulate2x() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"STATICFRAME frame.tga 10 20",
	]), PackedStringArray(["frame.tga"]), {"frame.tga": Vector2i(16, 8)})
	var hud := _make_overlay()
	hud.configure(fixture.layout, fixture.root)
	hud.set_player_state(100, 1.0, 0, 80.0)
	var rows := _flat_rows(hud, Vector2i(16, 8))
	assert_eq(rows.size(), 1, "the static frame quad submits once")
	if rows.size() == 1:
		assert_true(_flat_doubled(rows[0]), "as a flagged triangle pair")
		assert_eq(rows[0].colors[0], Color.WHITE, "at the raw white diffuse")
	assert_eq(hud.get_textured_quad_colors(true)[0], Color.WHITE,
			"a white texel under white draws white")


# The centred-quad class: a capture point's own-zone tile (lfp_dlf.tga, colour
# mode) carries the raw half-bright diffuse; the device doubles it, as
# HUD_DrawTexturedQuadCentered's material does (no compile-side fold).
func test_colour_mode_zone_tile_draws_half_bright_under_modulate2x() -> void:
	var fixture := _load_temp_layout(PackedStringArray(["fonthud1_hi Gunpl22b.fnt"]),
			PackedStringArray(["lfp_alf.tga", "lfp_dlf.tga"]),
			{"lfp_alf.tga": Vector2i(36, 36), "lfp_dlf.tga": Vector2i(36, 36)})
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var view := sim.get_local_player_view()
	var projection := Projection.create_perspective(80.0, 4.0 / 3.0, 0.1, 1000.0)
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			PackedInt32Array([7, 36, 0]))
	hud.set_combat_state(view, Transform3D.IDENTITY, projection, true, null, "E")
	var before := hud.get_draw_list_stats()
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			_capture_zone_snapshot(0))
	hud.set_combat_state(view, Transform3D.IDENTITY, projection, true, null, "E")
	assert_eq(hud.get_draw_list_stats().tris - before.tris, 2, "the own-zone tile compiles")
	var rows := _flat_rows(hud, Vector2i(36, 36))
	assert_eq(rows.size(), 1, "the zone tile's two triangles submit as one run")
	for row: Dictionary in rows:
		assert_eq(row.uvs.size(), 6)
		assert_true(_flat_doubled(row), "both triangles carry the MODULATE2X flag")
		for c: Color in row.colors:
			assert_true(c.r8 <= 127 and c.g8 <= 127 and c.b8 <= 127,
					"the tile's diffuse is the raw half-bright point colour")


# The alpha class stays its own: stance art (alpha mode, material 0xA51).
func test_alpha_mode_stance_keeps_the_alpha_material() -> void:
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
	hud.configure(fixture.layout, fixture.root)
	hud.set_player_state(100, 1.0, 2, 80.0)
	assert_eq(hud.get_draw_list_stats().quads, 1, "one stance frame quad")
	var rows := hud.get_flat_submissions()
	assert_eq(rows.size(), 1, "the stance quad submits once")
	if rows.size() == 1:
		assert_eq(rows[0].kind, "rect", "an unflagged texture rect: no MODULATE2X stage")
		var compiled: Color = hud.get_textured_quad_colors(false)[0]
		var doubled := Color8(mini(compiled.r8 * 2, 255), mini(compiled.g8 * 2, 255),
				mini(compiled.b8 * 2, 255), compiled.a8)
		assert_eq(rows[0].colors[0].to_rgba32(), doubled.to_rgba32(),
				"the alpha material's ADD(DIFFUSE, DIFFUSE) rides the submitted colour")
		assert_eq(hud.get_textured_quad_colors(true)[0].to_rgba32(), doubled.to_rgba32())


func test_crosshair_missing_texture_draws_nothing() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray())
	var hud := _make_overlay()
	hud.configure(fixture.layout, fixture.root)
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(hud.get_draw_list_stats().tris, 0,
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
	hud.configure(fixture.layout, fixture.root)
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	hud.set_view_state(true, Vector2.INF)
	assert_eq(hud.get_draw_list_stats().tris, 0,
			"Binocular view hides the normal weapon crosshair without hiding the HUD.")
	hud.set_view_state(false, Vector2.INF)
	assert_eq(hud.get_draw_list_stats().tris, 14,
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
	hud.configure(fixture.layout, fixture.root)
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


func test_crosshair_color_and_spread_survive_configure() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"ALPHAFADE 30 50 3",
	]), PackedStringArray(["cross01.tga"]))
	var hud := _make_overlay()
	# Set before configure(): both are picked up like the style.
	hud.set_crosshair_color(0x123456)
	hud.set_crosshair_spread_enabled(false)
	hud.configure(fixture.layout, fixture.root)
	assert_eq(hud.get_crosshair_color(), 0x123456,
			"the pre-configure colour survives the layout rebuild")
	assert_false(hud.is_crosshair_spread_enabled(),
			"the pre-configure spread toggle survives the layout rebuild")
	# Live sets: the colour masks to its 24-bit RGB (retail persists RGB).
	hud.set_crosshair_color(0x7F00FF00)
	assert_eq(hud.get_crosshair_color(), 0x00FF00)
	hud.set_crosshair_spread_enabled(true)
	assert_true(hud.is_crosshair_spread_enabled())


func test_message_feed_draws_and_expires() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
	]), PackedStringArray())
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	hud.set_player_state(50, 1.0, 0, 80.0)
	hud.push_message("Move to the extraction point")
	assert_gt(hud.get_draw_list_stats().glyphs, 0,
		"A pushed triggered-text line lays out glyph quads through the real .fnt.")
	await get_tree().process_frame
	# [orig: Chat_AddMessageChannel2 930-tick life] — the line is gone at push+930.
	hud.set_player_state(50 + 930, 1.0, 0, 80.0)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
		"The 930-tick life expires the line.")
	assert_true(is_instance_valid(hud), "A pushed triggered-text line draws safely.")


# The friendly tags (D-HUD-20) draw modes — FULL glyphs, the medic plate, the
# downed count, BRIEF ticks, OFF — are pinned by the hud_friendly_tags ctest
# over the compiler; the overlay's mode setter clamps.
func test_friendly_tag_mode_clamps() -> void:
	var hud := _make_overlay()
	assert_eq(hud.get_friendly_tag_mode(), 2, "the boot default mode is FULL")
	hud.set_friendly_tag_mode(99)
	assert_eq(hud.get_friendly_tag_mode(), 3, "the mode setter clamps to 0..3")
	hud.set_friendly_tag_mode(0)
	hud.set_friendly_tags(false, Transform3D.IDENTITY, Projection.IDENTITY, 0.0, null)
	assert_eq(hud.get_draw_list_stats().glyphs, 0, "OFF draws nothing")


# The end-of-round overlay element: the resolved Impact38 ladder centred on x
# 512 inside the safe-area stdbox; hidden draws nothing.
# [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0]
func test_end_round_overlay_draws_the_ladder() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
	]), PackedStringArray())
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	var before := hud.get_draw_list_stats()
	hud.set_end_round_overlay(true, 0, 768,
			PackedStringArray(["Mission Completed", "Blue Team : 12", "Game time : 0:01:05"]),
			PackedInt32Array([300, 414, 478]))
	var shown := hud.get_draw_list_stats()
	assert_gt(shown.glyphs, before.glyphs,
			"the ladder lays out its lines")
	assert_gt(shown.elements_drawn, before.elements_drawn,
			"the overlay element counts once")
	await get_tree().process_frame
	hud.set_end_round_overlay(false, 0, 768, PackedStringArray(), PackedInt32Array())
	assert_eq(hud.get_draw_list_stats().glyphs, before.glyphs,
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
	hud.configure(fixture.layout, fixture.root)
	hud.set_weapon_state(true, -1, -1, 0, 0, false, false, false, 0)
	assert_eq(hud.get_draw_list_stats().quads, 0, "zero heat hides the bar")
	await get_tree().process_frame
	hud.set_weapon_state(true, -1, -1, 0x8000, 0, false, false, false, 0)
	var stats := hud.get_draw_list_stats()
	assert_eq(stats.quads_wire, 1, "the HUDHEATBORDER wireframe")
	assert_eq(stats.quads_filled, 1, "plus the proportional fill")
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
		_copy_font_into(fixture.dir)
		var root := ResourceRoot.new()
		assert_eq(root.set_root_dir(fixture.dir), OK)
		var hud := _make_overlay()
		hud.configure(fixture.layout, root)
		hud.set_player_state(0, 1.0, 0, 80.0)
		# No waypoint entry: the label hides.
		assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"no waypoint entry draws nothing (align %s)" % align)
		hud.set_waypoint("North Sea Village", 143)
		var stats := hud.get_draw_list_stats()
		assert_gt(stats.glyphs, 0,
			"name + distance lay out glyphs for align %s" % align)
		assert_eq(stats.quads_wire, 1,
			"the distance box wireframe draws for align %s" % align)
		await get_tree().process_frame
		# The nameless form still draws the distance.
		hud.set_waypoint("", 9)
		assert_gt(hud.get_draw_list_stats().glyphs, 0,
			"a nameless entry still draws the distance for align %s" % align)
		await get_tree().process_frame
		hud.clear_waypoint()
		assert_eq(hud.get_draw_list_stats().glyphs, 0,
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
	effects.set_resource_root(fixture.root)
	effects.update_view(true, 1000, true, 4)
	await get_tree().process_frame
	RenderingServer.canvas_item_set_custom_rect(effects.get_canvas_item(), false)

	assert_eq(RenderingServer.debug_canvas_item_get_rect(effects.get_canvas_item()),
			Rect2(HudPos.d3d9_screen_offset(), Vector2(1024, 768)),
			"Retail masks cover the viewport (on D3D9's pixel centres) while inset art stays in design coordinates.")
	assert_eq(effects.get_child_count(true), 4,
			"The sun veil and the three fullscreen damage-feedback quads are "
			+ "internal children; the NVG image is the terminal FrameFx pass's, and "
			+ "the underwater murk draws in the 3D view's post-particle overlay "
			+ "pass, before the frame effects [orig: Terrain_RenderWorldScene "
			+ "@ 0x5c96f5].")
	assert_null(effects.get_node_or_null("NvgPost"))
	assert_null(effects.get_node_or_null("UnderwaterMurk"))
	var veil := effects.get_node("SunVeil") as ColorRect
	assert_not_null(veil,
			"The veil composites after the scene [orig: the veil draws in "
			+ "Render_ProcessMainSceneFrame @ 0x5cac4b, after the scene composites].")
	assert_not_null(veil.material as ShaderMaterial,
			"The veil rect samples the opennova_sun_veil_alpha global via its shader.")
	assert_true(effects.is_nvg_mask_visible(),
			"First-person-visible NVG draws the NVG.tga mask and gain scale "
			+ "[orig: NVG_DrawMaskAndGain @0x5cffab..0x5d0055].")
	effects.update_view(false, 1, false, 0)
	assert_false(effects.is_nvg_mask_visible(),
			"Camera suppression hides the mask without consuming simulation state.")


# The three fullscreen damage-feedback quads. The engine owns every word, decay
# and cap (ctest screen_flash); this asserts only the presenter's reduction of
# those draw values onto rects, and the retail emission order white -> red
# vignette -> revive tint. The addressed witness lives in
# docs/interface/hud-re.md, "Local damage feedback".
func test_player_view_effects_damage_feedback_quads() -> void:
	var effects := PlayerViewEffectsScript.new()
	effects.size = Vector2(1024, 768)
	add_child_autofree(effects)
	var white := effects.get_node("ScreenFlashWhite") as ColorRect
	var vignette := effects.get_node("ScreenFlashVignette") as TextureRect
	var revive := effects.get_node("ScreenFlashReviveTint") as ColorRect
	assert_not_null(white)
	assert_not_null(vignette)
	assert_not_null(revive)
	assert_false(white.visible, "No damage means no quads.")
	assert_false(vignette.visible)
	assert_false(revive.visible)
	assert_lt(white.get_index(true), vignette.get_index(true),
			"retail emits the white hit flash before the red damage vignette.")
	assert_lt(vignette.get_index(true), revive.get_index(true),
			"and the red vignette before the revive tint.")
	assert_false(white.show_behind_parent,
			"The quads follow the binocular/NVG draw, not precede it.")
	var tint_material := revive.material as CanvasItemMaterial
	assert_not_null(tint_material,
			"The revive tint multiplies the frame rather than covering it.")
	assert_eq(tint_material.blend_mode, CanvasItemMaterial.BLEND_MODE_MUL)

	# A full white flash: opaque white, no texture involved.
	effects.update_damage_feedback(255, 0, 0, 255)
	assert_true(white.visible)
	assert_almost_eq(white.color.a, 1.0, 0.0001)
	assert_false(vignette.visible)
	assert_false(revive.visible)

	# Half a white flash plus the red vignette at its 192 draw cap. With no
	# resource root the vignette texture is absent, so that quad stays down.
	effects.update_damage_feedback(128, 192, 0, 255)
	assert_almost_eq(white.color.a, 128.0 / 255.0, 0.0001)
	assert_false(vignette.visible,
			"Without vignette.tga loaded the red quad has nothing to draw.")

	# The revive tint at its held floor: R = G = 255 - (196 >> 1) = 157, B 255.
	effects.update_damage_feedback(0, 0, 196, 157)
	assert_false(white.visible)
	assert_true(revive.visible)
	assert_almost_eq(revive.color.r, 157.0 / 255.0, 0.0001)
	assert_almost_eq(revive.color.g, 157.0 / 255.0, 0.0001)
	assert_almost_eq(revive.color.b, 1.0, 0.0001)
	assert_almost_eq(revive.color.a, 1.0, 0.0001)

	effects.update_damage_feedback(0, 0, 0, 255)
	assert_false(revive.visible, "A cleared word retires its quad.")


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
	var layout: HudPos = fixture.layout
	hud.configure(layout, fixture.root)
	var block := layout.get_vehicle_hud("dbuggy1")
	assert_not_null(block, "The fixture's VEHICLE_HUD block resolves by sid.")
	if block == null:
		return
	var before := hud.get_draw_list_stats().quads_textured
	hud.set_vehicle_panel(true, block, 0, null)
	assert_eq(hud.get_draw_list_stats().quads_textured, before + 1,
			"The panel adds exactly the interface silhouette quad.")
	await get_tree().process_frame
	hud.set_vehicle_panel(false, null, 0, null)
	assert_eq(hud.get_draw_list_stats().quads_textured, before,
			"Hiding the panel removes the silhouette.")
	# A block whose interface art is missing draws NO panel at all.
	var missing := layout.get_vehicle_hud("dbuggy1")
	missing.sid = "nosuch"
	missing.interface_texture = "missing.tga"
	hud.set_vehicle_panel(true, missing, 0, null)
	assert_eq(hud.get_draw_list_stats().quads_textured, before,
			"Without the interface texture the panel is skipped entirely.")


# The Recent Messages (J) window lists the CHAT ring with NO expiry gate: a
# chat line that has already faded off the HUD feed still lists in the window
# [orig: HUD_DrawMessageLog @0x5b9d70 walks slots 16..1 of both rings].
func test_message_log_lists_expired_lines() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"HUDCHATTEXT 142 , 711",
	]), PackedStringArray())
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	hud.set_player_state(50, 1.0, 0, 80.0)
	hud.push_chat_line("Taylor: moving to bravo", -1)
	assert_gt(hud.get_draw_list_stats().glyphs, 0,
			"A pushed chat line lays out glyph quads on the HUDCHATTEXT feed.")
	await get_tree().process_frame
	# Past the 930-tick life the feed loop drops the line...
	hud.set_player_state(50 + 930, 1.0, 0, 80.0)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"The 930-tick life expires the chat line off the feed.")
	# ...but the window still lists it.
	hud.set_message_log_title("Recent Messages")
	hud.set_message_log_shown(true)
	assert_gt(hud.get_draw_list_stats().glyphs, 0,
			"The Recent Messages window lists the expired chat line (no expiry gate).")
	await get_tree().process_frame
	hud.set_message_log_shown(false)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"Closing the window hides the history again.")


# The stdbox border pieces bind the boxtile camo as a second texture stage the
# flat HUD material combines, screen-anchored; the fill does not
# [orig: CGfxTexture_Create(BoxTexA, BoxTexB, 0x651, 2) @0x56af3c, applied for
#  the pieces @0x56b902; HUD_DrawTexturedQuad_0 @0x56b3e0].
func test_stdbox_pieces_carry_the_camo_stage() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"HUDCHATTEXT 142 , 711",
	]), PackedStringArray(["border.tga", "boxtile.tga"]),
			{"border.tga": Vector2i(128, 128), "boxtile.tga": Vector2i(64, 64)})
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	hud.set_player_state(50, 1.0, 0, 80.0)
	assert_eq(hud.get_draw_list_stats().quads_stage2, 0, "No box, no camo stage.")
	hud.set_message_log_title("")
	hud.set_message_log_shown(true)
	var stats := hud.get_draw_list_stats()
	assert_eq(stats.quads_stage2, 8,
			"The untitled box's eight border pieces carry the camo stage.")
	assert_gt(stats.quads_textured, 8, "The wrap-tiled fill draws beside them.")
	# Every box quad draws through the box material 0x651 at retail's raw
	# alpha<<24 | 0x7F7F7F: stage 0 doubles it on the device (the UV.y flag),
	# the pieces' camo stage rides the UV.x flag [orig: Render_HUDBoxOverlay
	# @0x56b700, the diffuse @0x56b70e..0x56b713; BoxTexture_LoadAndSetupUVRegions
	# 0x651 @0x56ae78 / @0x56af2d].
	var pieces := 0
	var fills := 0
	for row: Dictionary in _flat_rows(hud, Vector2i(128, 128)):
		assert_true(_flat_doubled(row), "a box quad carries the MODULATE2X flag")
		assert_eq(row.colors[0].to_argb32(), 0xFF7F7F7F, "at the raw half-bright diffuse")
		if row.uvs[0].x >= 4.0:
			pieces += 1
		else:
			fills += 1
	assert_eq(pieces, 8, "the eight pieces add the camo stage")
	assert_gt(fills, 0, "the fill tiles draw the stage-0 doubling alone")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "The two-stage pieces render safely.")
	hud.set_message_log_shown(false)
	assert_eq(hud.get_draw_list_stats().quads_stage2, 0)


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
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	var before := hud.get_draw_list_stats()
	hud.set_lfp_panel(true, NetProtocol.GAME_TYPE_ADVANCE_AND_SECURE, 1, 0,
			{"under_attack": "!Under\nAttack!!", "ready": "!Ready for\nTakeover!"}, null)
	var shown := hud.get_draw_list_stats()
	assert_eq(shown.elements_drawn, before.elements_drawn,
			"With no zone rows the panel draws nothing.")
	await get_tree().process_frame
	hud.set_lfp_panel(false, 0, 0, 0, {}, null)
	assert_true(is_instance_valid(hud), "Hiding the zone panel is safe.")


# One v7 marker row: a spawn-point zone (entity_bits 4) with zone number 0x41
# (a capture bit in the slot's source byte), team 1, at mission (0, 10, 0),
# with a zone-timer entry when `timer_flags` is nonzero.
func _capture_zone_snapshot(timer_flags: int) -> PackedInt32Array:
	return PackedInt32Array([
		7, 36, 1,
		0, 0x3001, 0, 10 << 16, 0, 0, 0, -1, 0, 0x41, 0, 1, 0, 0, 0, 6,
		0, 1, 0x41, 0, 4,
		0, 0, 0, 10 << 16, 0, 10 << 16, 0,
		0, 0,
		timer_flags, 1, 1, 0, 62, 0,
	])


# The capture-point labels' device seam: set_combat_state walks the fed rows
# and projects each admitted point; a zone ahead of the camera then compiles
# the marker's stem and the viewer's own-zone tile, plus the letter once a
# zone-timer entry exists [orig: Render_CapturePointLabels @0x5a2840 ->
# HUD_DrawEntityMarker types 8/9 @0x593140].
func test_capture_point_label_device_seam() -> void:
	var fixture := _load_temp_layout(PackedStringArray(["fonthud1_hi Gunpl22b.fnt"]),
			PackedStringArray(["lfp_alf.tga", "lfp_dlf.tga"]),
			{"lfp_alf.tga": Vector2i(36, 36), "lfp_dlf.tga": Vector2i(36, 36)})
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var view := sim.get_local_player_view()
	var projection := Projection.create_perspective(80.0, 4.0 / 3.0, 0.1, 1000.0)
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			PackedInt32Array([7, 36, 0]))
	hud.set_combat_state(view, Transform3D.IDENTITY, projection, true, null, "E")
	var before := hud.get_draw_list_stats()
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			_capture_zone_snapshot(0))
	hud.set_combat_state(view, Transform3D.IDENTITY, projection, true, null, "E")
	var blank := hud.get_draw_list_stats()
	assert_eq(blank.lines - before.lines, 1, "The zone's marker stem compiles.")
	assert_eq(blank.tris - before.tris, 2, "The own-zone tile compiles as two textured triangles.")
	hud.set_minimap_state(Vector2.ZERO, 0.0, 0, 65536, 65536, 0, false,
			_capture_zone_snapshot(1))
	hud.set_combat_state(view, Transform3D.IDENTITY, projection, true, null, "E")
	assert_eq(blank.glyphs - before.glyphs, 1, "The blank letter lays out its one space quad.")
	var lettered := hud.get_draw_list_stats()
	assert_eq(lettered.glyphs, blank.glyphs,
			"A zone-timer entry letters the zone; no name outside binoculars.")
	assert_eq(lettered.quads_filled, blank.quads_filled, "No capture bar outside binoculars.")
	# Through binoculars the entry's capture bar draws its three quads (the
	# name is empty: no gametext here, and GameText_GetString misses to "").
	hud.set_view_state(true, Vector2.INF)
	var binoculars := hud.get_draw_list_stats()
	assert_eq(binoculars.quads_filled - lettered.quads_filled, 3,
			"The binocular capture bar: border, black inner rect, fill.")
	hud.set_view_state(false, Vector2.INF)
	# Behind the camera the marker draws nothing.
	hud.set_combat_state(view, Transform3D.IDENTITY.rotated(Vector3.UP, PI), projection, true,
			null, "E")
	var behind := hud.get_draw_list_stats()
	assert_eq(behind.lines, before.lines, "A zone behind the camera draws no stem.")
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "The capture-point labels render safely.")


# The connection indicators' device seam (D-HUD-38): the three atlases and the
# NETWORKINDICATOR corners load through the real VFS path, and out of a
# session (no Simulation: the role facts' session gate is down) the drawer
# adds nothing [orig: CNetQuality_DrawIndicators @0x4c3200, the session gate
# @0x4c3210; the atlas loads CNetworkIcons_LoadTextures @0x4c2cf0].
func test_net_quality_indicators_device_seam() -> void:
	var fixture := _load_temp_layout(PackedStringArray([
		"fonthud1_hi Gunpl22b.fnt",
		"NETWORKINDICATOR 6,6 22,6 54,6",
	]), PackedStringArray(["neticon1.tga", "neticon2.tga", "neticon3.tga"]),
			{"neticon1.tga": Vector2i(32, 64), "neticon2.tga": Vector2i(16, 64),
			 "neticon3.tga": Vector2i(16, 32)})
	_copy_font_into(fixture.dir)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(fixture.dir), OK)
	var hud := _make_overlay()
	hud.configure(fixture.layout, root)
	var before := hud.get_draw_list_stats()
	hud.set_role_facts(null, null)
	var after := hud.get_draw_list_stats()
	assert_eq(after.quads_textured, before.quads_textured,
			"Out of a session the connection indicators draw nothing.")
	assert_eq(after.elements_drawn, before.elements_drawn)
	await get_tree().process_frame
	assert_true(is_instance_valid(hud), "The indicator atlases load and render safely.")


func test_sights_viewport_aspect_preserves_square_reticle() -> void:
	var square := Rect2(384, 256, 256, 256)
	assert_eq(HudPos.sight_scale_rect(square, Vector2(1024, 768)), square)
	assert_eq(HudPos.sight_scale_rect(square, Vector2(1920, 1080)),
			Rect2(720, 300, 480, 480), "widescreen reticle stays square and centered")
