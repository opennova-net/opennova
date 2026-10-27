extends GutTest

# Pins for the HudPos statics the shell draws with (the design-space rect, the
# scoped-view mask's geometry) and the PlayerHudWeaponDef record; the engine
# math under them is ctest hud_math's and sight_overlay's.
# Rendering is validated on the compiled draw list in hud_overlay_test.gd.


func test_scale_rect_identity() -> void:
	var surface := Vector2(HudPos.DESIGN_WIDTH, HudPos.DESIGN_HEIGHT)
	assert_eq(HudPos.scale_rect(Rect2(2, 739, 139, 18), surface), Rect2(2, 739, 139, 18))


func test_scale_rect_scales_both_corners() -> void:
	# The original scales x1,y1 and x2,y2 independently, then differences.
	var surface := Vector2(2048, 1536)
	assert_eq(HudPos.scale_rect(Rect2(2, 739, 139, 18), surface),
			Rect2(4, 1478, 278, 36))


func test_scope_circle_mask_geometry() -> void:
	# retail: the annulus spans 0.71 to 1.5 of five eighths of the viewport
	# height, as a 130-vertex strip, and the reticle cross plus sixteen grid
	# diamonds only join it when the SIGHTS card drew no authored row.
	# The surface is retail's inclusive overlay rect (0, 0)..(1023, 767); the derived frame
	# (centre, ring, radii, scales) is ctest sight_overlay's.
	var surface := Vector2(HudPos.DESIGN_WIDTH, HudPos.DESIGN_HEIGHT)
	var ring := HudPos.scope_mask_points(surface, 1024, true, HudPos.SCOPE_MASK_RING)
	assert_eq(ring.size(), 130, "65 angle stops, two vertices each.")
	assert_eq(HudPos.scope_mask_indices(surface, 1024, true, HudPos.SCOPE_MASK_RING).size(),
		384, "128 triangles expand the strip.")
	var ring_colors := HudPos.scope_mask_colors(surface, 1024, true, HudPos.SCOPE_MASK_RING)
	assert_eq(ring_colors[0].a8, 255, "The annulus is opaque.")
	assert_eq(HudPos.scope_mask_points(surface, 1024, true, HudPos.SCOPE_MASK_CROSS).size(),
		28, "Four 7-vertex spokes.")
	assert_eq(HudPos.scope_mask_points(surface, 1024, true, HudPos.SCOPE_MASK_GRID).size(),
		80, "Sixteen 5-vertex diamonds.")

	# The NVG lens's reticle: no ring (the lens draws it), the cross and grid
	# at unit scale, nothing at all once the card drew rows.
	# [orig: NVG_DrawScopedLens @0x5d2798..0x5d27bc]
	assert_eq(HudPos.scope_mask_points(surface, 1024, true, HudPos.SCOPE_MASK_RING, -1,
			true).size(), 0, "the lens draws its own ring")
	assert_eq(HudPos.scope_mask_points(surface, 1024, true, HudPos.SCOPE_MASK_CROSS, -1,
			true).size(), 28, "the unit-scale cross")
	assert_eq(HudPos.scope_mask_points(surface, 1024, false, HudPos.SCOPE_MASK_CROSS, -1,
			true).size(), 0, "a card with rows draws no reticle")
	assert_eq(HudPos.scope_mask_points(surface, 1024, false, HudPos.SCOPE_MASK_RING).size(),
		130, "Authored SIGHTS rows never suppress the annulus.")
	assert_true(HudPos.scope_mask_points(surface, 1024, false, HudPos.SCOPE_MASK_CROSS).is_empty(),
		"They suppress the reticle cross.")
	assert_true(HudPos.scope_mask_points(surface, 1024, false, HudPos.SCOPE_MASK_GRID).is_empty(),
		"And the grid ticks.")


func test_hud_weapon_def_decode() -> void:
	# The ADR 0017 slice over the WeaponDef record: field for field from the
	# authored weapon.def, null for no weapon.
	assert_null(PlayerHudWeaponDef.from_weapon_def(null), "No weapon decodes to null.")
	var weapon_path := DefFixture.directory().path_join("hud_weapon.def")
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(weapon_path), OK, "the authored weapon.def loads")
	var index := wdb.find_weapon("WPN_M4AUTO")
	assert_gte(index, 0, "the fixture carries WPN_M4AUTO")
	var weapon := wdb.get_weapon(index)
	var def := PlayerHudWeaponDef.from_weapon_def(weapon)
	assert_eq(def.weapon_name, "WPN_M4AUTO")
	assert_eq(def.clipsize, 30)
	assert_eq(def.clipgfx_texture, "fixture_clip.tga")
	assert_eq(def.rndgfx_offset, Vector2i(13, 14))
	assert_eq(def.rndgfx_step, Vector2i(15, 16))
	assert_eq(def.sights.size(), 1)
	assert_almost_eq(def.error_row_deg(2), 3.0, 0.0001)
	assert_eq(def.round_type, weapon.round_type)
	assert_eq(def.clipsize, weapon.clipsize)
	assert_eq(def.error_deg, weapon.error)
	assert_eq(def.clipgfx_texture, weapon.hudclipgfx_texture)
	assert_eq(def.rndgfx_offset, weapon.hudrndgfx_offset)
	assert_eq(def.rndgfx_step, Vector2i(weapon.hudrndgfx_layout.x, weapon.hudrndgfx_layout.y))
	assert_eq(def.sights.size(), weapon.get_sights().size(), "every authored SIGHTS row rides along")
	assert_almost_eq(def.error_row_deg(2), weapon.error[2], 0.0001, "Hip-stand dispersion row.")
	assert_eq(def.error_row_deg(9), 0.0, "Out-of-table row reads 0.")
	# The divisor is a byte in the original (weapon+727) — out-of-range wraps.
	# [orig: HUDRNDGFX parse @0x5442fc; unsigned byte read @0x599bb1]
	assert_eq(PlayerHudWeaponDef.rounds_per_icon_from_layout(Vector3i(18, 0, 1)), 1)
	assert_eq(PlayerHudWeaponDef.rounds_per_icon_from_layout(Vector3i(18, 0, 257)), 1,
			"Divisor 257 wraps to the byte 1.")
