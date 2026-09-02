extends GutTest

# Pure-logic pins for the native HUD view math (the HudPos statics over
# engine/runtime/hud hud_math.h — the single source the HudOverlay compiler
# draws with), and the PlayerHudWeaponDef record.
# Rendering is validated on the compiled draw list in hud_overlay_test.gd.


func test_scale_point_identity() -> void:
	var surface := Vector2(HudPos.DESIGN_WIDTH, HudPos.DESIGN_HEIGHT)
	assert_eq(HudPos.scale_point(Vector2(0, 0), surface), Vector2(0, 0))
	assert_eq(HudPos.scale_point(Vector2(512, 384), surface), Vector2(512, 384))
	assert_eq(HudPos.scale_point(Vector2(1024, 768), surface), Vector2(1024, 768))


func test_scale_point_scaled_surface() -> void:
	# Half the design space maps to half the (doubled) surface.
	var surface := Vector2(2048, 1536)
	assert_eq(HudPos.scale_point(Vector2(512, 384), surface), Vector2(1024, 768))


func test_output_pixel_delta_stays_exact_across_surface_sizes() -> void:
	var anchor := Vector2(341, 512)
	for surface in [Vector2(1024, 768), Vector2(1600, 900), Vector2(1920, 1080)]:
		var anchor_px := HudPos.scale_point(anchor, surface)
		var label_design: Vector2 = anchor + HudPos.pixel_delta_to_design(
				Vector2(0, -15), surface)
		var label_px := HudPos.scale_point(label_design, surface)
		assert_eq(label_px.x, anchor_px.x,
				"a vertical output-pixel offset preserves x at %s" % surface)
		assert_eq(label_px.y, anchor_px.y - 15,
				"the label stays exactly 15 output pixels above at %s" % surface)


func test_scale_rect_identity() -> void:
	var surface := Vector2(HudPos.DESIGN_WIDTH, HudPos.DESIGN_HEIGHT)
	assert_eq(HudPos.scale_rect(Rect2(2, 739, 139, 18), surface), Rect2(2, 739, 139, 18))


func test_scale_rect_scales_both_corners() -> void:
	# The original scales x1,y1 and x2,y2 independently, then differences.
	var surface := Vector2(2048, 1536)
	assert_eq(HudPos.scale_rect(Rect2(2, 739, 139, 18), surface),
			Rect2(4, 1478, 278, 36))


func test_health_thresholds() -> void:
	# Witnessed 16.16 thresholds (hud/hud_math.h): 0xC000 good, 0x6FFF mid —
	# 0 good / 1 mid / 2 bad through the bound band.
	assert_eq(HudPos.health_color_band(0.7501), 0)
	assert_eq(HudPos.health_color_band(0.75), 1)
	assert_eq(HudPos.health_color_band(0.4375), 1)
	assert_eq(HudPos.health_color_band(0.4374), 2)


func test_crosshair_spread_px() -> void:
	# [orig: HUD_DrawCrosshair @0x592b07..0x592bf5] pixel = (err_16.16 * screen_w /
	# int(fov_deg)) >> 16 — 0.25 deg (0x4000) over an 80-deg fov on a 1024-wide
	# screen = 3 px.
	assert_eq(HudPos.crosshair_spread_px_fp16(0x4000, 80.0, 1024.0), 3.0)
	assert_eq(HudPos.crosshair_spread_px_fp16(0, 80.0, 1024.0), 0.0)
	# The fov register's integer part gates: fov <= 0 is a safe no-spread.
	assert_eq(HudPos.crosshair_spread_px_fp16(0x10000, 0.0, 1024.0), 0.0)


func test_crosshair_live_spread_sum_uses_signed_shifts() -> void:
	# ERROR + (entity+0x380 >> 7) + (entity+0x384 >> 7), with arithmetic
	# shifts before any screen conversion. [orig: HUD_DrawCrosshair
	# @0x592b07..0x592b28]
	assert_eq(HudPos.crosshair_total_spread_fp16(
			0x4000, 0x80000, 0x40000), 0x5800)
	assert_eq(HudPos.crosshair_total_spread_fp16(0, -1, -129), -3,
			"negative carriers use x86-style SAR, not truncating division")
	assert_eq(HudPos.crosshair_total_spread_fp16(0x7FFFFFFF, 128, 0), -2147483648,
			"spread addition retains retail signed-32 wrap")
	assert_true(HudPos.crosshair_should_draw(false, false))
	assert_false(HudPos.crosshair_should_draw(true, false),
			"an ordinary settled aimed shot hides the reticle")
	assert_true(HudPos.crosshair_should_draw(true, true),
			"the vehicle/gunner keep-up leg can use ERROR's second triplet")


func test_crosshair_error_row() -> void:
	# Rows: hip prone/crouch/stand then scoped prone/crouch/stand.
	assert_eq(HudPos.crosshair_error_row(0, false), 0)
	assert_eq(HudPos.crosshair_error_row(2, false), 2)
	assert_eq(HudPos.crosshair_error_row(0, true), 3)
	assert_eq(HudPos.crosshair_error_row(2, true), 5)


func test_fade_decay_curve() -> void:
	# [orig: draw_hud_ammo_indicator @0x599af9] — 255 right after the change, 0 at the
	# ramp end, with the witnessed elapsed-0 u16 wrap quirk reading as fully decayed.
	assert_eq(HudPos.fade_decay(0, 186), 0, "Elapsed 0 wraps to no flash (one-tick latency).")
	assert_eq(HudPos.fade_decay(1, 186), 254)
	assert_eq(HudPos.fade_decay(93, 186), 128)
	assert_eq(HudPos.fade_decay(186, 186), 0)
	assert_eq(HudPos.fade_decay(500, 186), 0, "Elapsed clamps to the ramp.")
	assert_eq(HudPos.fade_decay(10, 0), 0, "Zero ramp is safe.")


func test_fade_alphas() -> void:
	# ALPHAFADE 30 50 3 -> base 76, max 127, ramp 186 ticks.
	assert_eq(int(30 * HudPos.percent_to_alpha()), 76)
	assert_eq(int(50 * HudPos.percent_to_alpha()), 127)
	# The witnessed literals pin the BINDING (the single native source) — a
	# re-witness that changes hud_math.h must show up here, not drift silently.
	assert_almost_eq(float(HudPos.percent_to_alpha()), 2.55, 0.0001)
	assert_eq(HudPos.MESSAGE_LIFE_TICKS, 930)
	assert_eq(HudPos.MESSAGE_EXPIRY_STAGGER, 186)
	assert_eq(HudPos.MESSAGE_TEXT_MAX, 119)
	assert_eq(HudPos.MESSAGE_SLOT_COUNT, 40)
	# The clip flash clamps at the ALPHAFADE max; the stance pair clamps at 255 and
	# ghosts the previous frame at quarter fade.
	assert_eq(HudPos.fade_flash_alpha(93, 186, 76, 127), 127)
	assert_eq(HudPos.fade_flash_alpha(186, 186, 76, 127), 76)
	assert_eq(HudPos.stance_current_alpha(93, 186, 76), 204)
	assert_eq(HudPos.stance_prev_alpha(93, 186), 32)


func test_message_expiry_policy() -> void:
	# [orig: Chat_AddDebugMessage @0x4987f0 — 930-tick life @0x51f216, the
	# >=186-tick expiry stagger vs the previous line @0x49894e]. The live ring
	# is HudOverlay compiler state; the policy math pins here.
	assert_eq(HudPos.message_expire_tick(0, 0, false), 930)
	assert_eq(HudPos.message_expire_tick(0, 930, true), 1116,
			"A line pushed at the same tick staggers behind the previous expiry.")
	assert_eq(HudPos.message_expire_tick(2000, 930, true), 2930,
			"A late push takes its own now+life when it already clears the stagger.")


func test_ammo_text_format() -> void:
	# [orig: hud_draw_weapon_ammo_and_name @0x593a33..0x593ab0]
	assert_eq(HudPos.format_ammo(30, 90, 30), "30/90")
	assert_eq(HudPos.format_ammo(-1, 90, 30), "90", "No clip -> reserve only.")
	assert_eq(HudPos.format_ammo(1, 4, 1), "4", "Capacity 1 -> reserve only.")
	assert_eq(HudPos.format_ammo(5, -1, 30), "", "Reserve -1 hides the element.")
	assert_eq(HudPos.format_ammo(5, 90, -1), "", "Capacity -1 hides the element.")


func test_hud_weapon_def_decode() -> void:
	# The ADR 0017 slice over the WeaponDef record: field for field from the
	# shipped weapon.def, null for no weapon.
	assert_null(PlayerHudWeaponDef.from_weapon_def(null), "No weapon decodes to null.")
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(RetailData.fixture("def/weapon.def")), OK, "the shipped weapon.def loads")
	var index := wdb.find_weapon("WPN_M4AUTO")
	assert_gte(index, 0, "the fixture carries WPN_M4AUTO")
	var weapon := wdb.get_weapon(index)
	var def := PlayerHudWeaponDef.from_weapon_def(weapon)
	assert_eq(def.weapon_name, "WPN_M4AUTO")
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


func test_stance_shared_scale() -> void:
	# One 16.16 factor from FRAME 0 scales every frame; centering comes from frame
	# 0's scaled dims and skips at 127+. [orig: HUD_DrawStanceIndicator
	# @0x599fed..0x59a07e — 0x800000/max(w0,h0), (q16*dim+0x8000)>>16]
	assert_eq(HudPos.stance_scale_q16(Vector2i(128, 128)), 0x10000)
	assert_eq(HudPos.stance_scaled_dim(128, 0x10000), 128)
	assert_eq(HudPos.stance_center_offset(Vector2i(128, 128), 0x10000), Vector2i.ZERO,
		"128 scales to 128 (>=127): no centering.")
	var q := HudPos.stance_scale_q16(Vector2i(64, 32))
	assert_eq(q, 0x20000)
	assert_eq(HudPos.stance_scaled_dim(64, q), 128, "The max dim fills the box.")
	assert_eq(HudPos.stance_scaled_dim(32, q), 64)
	assert_eq(HudPos.stance_center_offset(Vector2i(64, 32), q), Vector2i(0, 32),
		"The minor dim centers by (128-scaled)/2; the 128 dim does not.")
	assert_eq(HudPos.stance_scaled_dim(256, q), 512,
		"Other frames scale by frame 0's factor blindly, like the original.")
	assert_eq(HudPos.stance_scale_q16(Vector2i.ZERO), 0, "Zero dims are safe.")


func test_round_icon_count() -> void:
	# [orig: draw_hud_ammo_indicator @0x599b9c..0x599bc1]
	assert_eq(HudPos.round_icon_count(12, 90, 30, 1), 12)
	assert_eq(HudPos.round_icon_count(12, 90, 1, 1), 40, "Capacity 1 counts the pool, capped at 40.")
	assert_eq(HudPos.round_icon_count(12, 90, 30, 3), 4, "Divisor rounds up: (12+1)/3.")
	assert_eq(HudPos.round_icon_count(0, 90, 30, 1), 0)


func test_folded_reserve() -> void:
	# The capacity-1 chambered-round fold the presenter applies at the info edge.
	# [orig: HUD_BuildEntityInfo @0x4b85ef — hudInfo+52 += clip when def+88 == 1]
	assert_eq(HudPos.folded_reserve(1, 4, 1), 5)
	assert_eq(HudPos.folded_reserve(12, 90, 30), 90, "Only capacity 1 folds.")
	assert_eq(HudPos.folded_reserve(-1, 4, 1), 4, "The -1 sentinels never fold.")
