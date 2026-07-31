extends GutTest

# Pure-logic coverage for the shell-neutral HUD view helpers (godot/engine/ui/hud_*.gd).
# Rendering is validated visually in the ONED preview / runtime; here we lock the math.


func test_scale_point_identity() -> void:
	var surface := Vector2(HudLayout.DESIGN_WIDTH, HudLayout.DESIGN_HEIGHT)
	assert_eq(HudLayout.scale_point(Vector2(0, 0), surface), Vector2(0, 0))
	assert_eq(HudLayout.scale_point(Vector2(512, 384), surface), Vector2(512, 384))
	assert_eq(HudLayout.scale_point(Vector2(1024, 768), surface), Vector2(1024, 768))


func test_scale_point_scaled_surface() -> void:
	# Half the design space maps to half the (doubled) surface.
	var surface := Vector2(2048, 1536)
	assert_eq(HudLayout.scale_point(Vector2(512, 384), surface), Vector2(1024, 768))


func test_output_pixel_delta_stays_exact_across_surface_sizes() -> void:
	var anchor := Vector2(341, 512)
	for surface in [Vector2(1024, 768), Vector2(1600, 900), Vector2(1920, 1080)]:
		var anchor_px := HudLayout.scale_point(anchor, surface)
		var label_design: Vector2 = anchor + HudLayout.pixel_delta_to_design(
				Vector2(0, -15), surface)
		var label_px := HudLayout.scale_point(label_design, surface)
		assert_eq(label_px.x, anchor_px.x,
				"a vertical output-pixel offset preserves x at %s" % surface)
		assert_eq(label_px.y, anchor_px.y - 15,
				"the label stays exactly 15 output pixels above at %s" % surface)


func test_rect_from_corners() -> void:
	# Health rect corners from the fixture: 2,739,141,757.
	assert_eq(HudLayout.rect_from_corners(2, 739, 141, 757), Rect2(2, 739, 139, 18))


func test_scale_rect_identity() -> void:
	var surface := Vector2(HudLayout.DESIGN_WIDTH, HudLayout.DESIGN_HEIGHT)
	assert_eq(HudLayout.scale_rect(Rect2(2, 739, 139, 18), surface), Rect2(2, 739, 139, 18))


func test_half_bright() -> void:
	assert_eq(HudText.half_bright(Color(1, 1, 1, 1)), Color(0.5, 0.5, 0.5, 1.0))
	# Alpha is forced opaque regardless of input.
	var hb := HudText.half_bright(Color(0.8, 0.4, 0.2, 0.25))
	assert_almost_eq(hb.r, 0.4, 0.001)
	assert_almost_eq(hb.g, 0.2, 0.001)
	assert_almost_eq(hb.b, 0.1, 0.001)
	assert_eq(hb.a, 1.0, "Half-bright forces opaque alpha.")


func test_health_thresholds() -> void:
	# Witnessed thresholds: 0xC000 good, 0x6FFF mid.
	assert_almost_eq(HudHealthBar.GOOD_THRESHOLD, 0.75, 0.0001)
	assert_almost_eq(HudHealthBar.MID_THRESHOLD, 0.4374, 0.001)


func test_attach_label_dim() -> void:
	# The non-nearest label transform: RGB halved, alpha forced to 0x7F.
	# [orig: ((rgb & 0xFEFEFE) | 0xFE000001) >> 1 @0x5a364e]
	var d := HudAttachLabels.dim(Color(1, 1, 1, 1))
	assert_almost_eq(d.r, 0.5, 0.001)
	assert_almost_eq(d.g, 0.5, 0.001)
	assert_almost_eq(d.b, 0.5, 0.001)
	assert_almost_eq(d.a, 127.0 / 255.0, 0.001, "Dim forces the 0x7F alpha byte.")
	var e := HudAttachLabels.dim(Color(0.8, 0.4, 0.2, 1.0))
	assert_almost_eq(e.r, 0.4, 0.001)
	assert_almost_eq(e.g, 0.2, 0.001)
	assert_almost_eq(e.b, 0.1, 0.001)


func test_draw_helpers_null_safe() -> void:
	# All draw helpers guard null inputs and must not crash.
	HudCrosshair.draw(null, null, Vector2.ZERO, Vector2.ONE)
	HudStance.draw(null, null, Vector2.ZERO, Vector2.ONE)
	HudHealthBar.draw(null, Rect2(0, 0, 10, 2), 0.5, Color.GRAY, Color.GREEN, Color.YELLOW, Color.RED, Vector2.ONE)
	HudText.draw_text(null, null, Vector2.ZERO, Vector2.ONE, "x", Color.WHITE)
	HudWeaponText.draw_ammo(null, null, Vector4i.ZERO, 30, 90, 30, Color.WHITE, Vector2.ONE)
	HudWeaponText.draw_weapon_name(null, null, Vector4i.ZERO, "AK-47", Color.WHITE, Vector2.ONE)
	HudClipIndicator.new().draw(null, Vector2i(5, 579), null, null, null, 12, 90,
		Color.WHITE, Vector3(30, 50, 3), 0, Vector2.ONE)
	HudMessages.new().draw(null, null, Vector2.ZERO, Vector2.ONE, 0, 8, 100.0, Color.WHITE)
	assert_true(true, "Null-guarded draw helpers returned without error.")


func test_load_font_null_safe() -> void:
	assert_null(HudText.load_font(null, "anything.fnt"), "Null root returns null font.")


func test_crosshair_spread_px() -> void:
	# [orig: HUD_DrawCrosshair @0x592b07..0x592bf5] pixel = (err_16.16 * screen_w /
	# int(fov_deg)) >> 16 — 0.25 deg over an 80-deg fov on a 1024-wide screen = 3 px.
	assert_eq(HudCrosshair.spread_px(0.25, 80.0, 1024.0), 3.0)
	assert_eq(HudCrosshair.spread_px(0.0, 80.0, 1024.0), 0.0)
	# The fov register's integer part gates: fov <= 0 is a safe no-spread.
	assert_eq(HudCrosshair.spread_px(1.0, 0.0, 1024.0), 0.0)
	assert_eq(HudCrosshair.spread_px_fp16(0x4000, 80.0, 1024.0), 3.0,
			"the exact 16.16 entry preserves the retail projection")


func test_crosshair_live_spread_sum_uses_signed_shifts() -> void:
	# ERROR + (entity+0x380 >> 7) + (entity+0x384 >> 7), with arithmetic
	# shifts before any screen conversion. [orig: HUD_DrawCrosshair
	# @0x592b07..0x592b28]
	assert_eq(HudCrosshair.total_spread_fp16(
			0x4000, 0x80000, 0x40000), 0x5800)
	assert_eq(HudCrosshair.total_spread_fp16(0, -1, -129), -3,
			"negative carriers use x86-style SAR, not truncating division")
	assert_eq(HudCrosshair.total_spread_fp16(0x7FFFFFFF, 128, 0), -2147483648,
			"spread addition retains retail signed-32 wrap")
	assert_true(HudCrosshair.should_draw(false))
	assert_false(HudCrosshair.should_draw(true),
			"an ordinary settled aimed shot hides the reticle")
	assert_true(HudCrosshair.should_draw(true, true),
			"the vehicle/gunner keep-up leg can use ERROR's second triplet")


func test_crosshair_error_row() -> void:
	# Rows: hip prone/crouch/stand then scoped prone/crouch/stand.
	assert_eq(HudCrosshair.error_row(0, false), 0)
	assert_eq(HudCrosshair.error_row(2, false), 2)
	assert_eq(HudCrosshair.error_row(0, true), 3)
	assert_eq(HudCrosshair.error_row(2, true), 5)


func test_fade_decay_curve() -> void:
	# [orig: draw_hud_ammo_indicator @0x599af9] — 255 right after the change, 0 at the
	# ramp end, with the witnessed elapsed-0 u16 wrap quirk reading as fully decayed.
	assert_eq(HudFade.decay(0, 186), 0, "Elapsed 0 wraps to no flash (one-tick latency).")
	assert_eq(HudFade.decay(1, 186), 254)
	assert_eq(HudFade.decay(93, 186), 128)
	assert_eq(HudFade.decay(186, 186), 0)
	assert_eq(HudFade.decay(500, 186), 0, "Elapsed clamps to the ramp.")
	assert_eq(HudFade.decay(10, 0), 0, "Zero ramp is safe.")


func test_fade_alphas() -> void:
	# ALPHAFADE 30 50 3 -> base 76, max 127, ramp 186 ticks.
	assert_eq(int(30 * HudFade.PERCENT_TO_ALPHA), 76)
	assert_eq(int(50 * HudFade.PERCENT_TO_ALPHA), 127)
	assert_eq(int(3 * HudFade.SECONDS_TO_TICKS), 186)
	# Fractional file fields survive: the original converts through atof
	# [orig: @0x5a0882..0x5a08c2] — 1.5 s is a 93-tick ramp, not 62.
	assert_eq(int(1.5 * HudFade.SECONDS_TO_TICKS), 93)
	# The clip flash clamps at the ALPHAFADE max; the stance pair clamps at 255 and
	# ghosts the previous frame at quarter fade.
	assert_eq(HudFade.flash_alpha(93, 186, 76, 127), 127)
	assert_eq(HudFade.flash_alpha(186, 186, 76, 127), 76)
	assert_eq(HudFade.stance_current_alpha(93, 186, 76), 204)
	assert_eq(HudFade.stance_prev_alpha(93, 186), 32)


func test_ammo_text_format() -> void:
	# [orig: hud_draw_weapon_ammo_and_name @0x593a33..0x593ab0]
	assert_eq(HudWeaponText.format_ammo(30, 90, 30), "30/90")
	assert_eq(HudWeaponText.format_ammo(-1, 90, 30), "90", "No clip -> reserve only.")
	assert_eq(HudWeaponText.format_ammo(1, 4, 1), "4", "Capacity 1 -> reserve only.")
	assert_eq(HudWeaponText.format_ammo(5, -1, 30), "", "Reserve -1 hides the element.")
	assert_eq(HudWeaponText.format_ammo(5, 90, -1), "", "Capacity -1 hides the element.")


func test_hud_weapon_def_decode() -> void:
	# The ADR 0017 record over NovaWeaponDatabase's transport dict.
	assert_null(PlayerHudWeaponDef.from_weapon_dict({}), "Empty dict decodes to null.")
	var def := PlayerHudWeaponDef.from_weapon_dict({
		"name": "WPN_AK47", "round_type": "AMMO_762", "clipsize": 30,
		"error": PackedFloat32Array([0.05, 0.2, 0.25, 0.05, 0.1, 0.15]),
		"hudclipgfx_texture": "H_clip.tga", "hudclipgfx_offset": Vector2i(0, 0),
		"hudrndgfx_texture": "H_round.tga", "hudrndgfx_offset": Vector2i(9, 0),
		"hudrndgfx_layout": Vector3i(18, 0, 1),
	})
	assert_eq(def.weapon_name, "WPN_AK47")
	assert_eq(def.clipsize, 30)
	assert_eq(def.rndgfx_offset, Vector2i(9, 0))
	assert_eq(def.rndgfx_step, Vector2i(18, 0))
	assert_eq(def.rounds_per_icon, 1)
	assert_almost_eq(def.error_row_deg(2), 0.25, 0.0001, "Hip-stand dispersion row.")
	assert_eq(def.error_row_deg(9), 0.0, "Out-of-table row reads 0.")
	# The divisor is a byte in the original (weapon+727) — out-of-range wraps.
	# [orig: HUDRNDGFX parse @0x5442fc; unsigned byte read @0x599bb1]
	var wrapped := PlayerHudWeaponDef.from_weapon_dict({
		"name": "X", "hudrndgfx_layout": Vector3i(18, 0, 257),
	})
	assert_eq(wrapped.rounds_per_icon, 1, "Divisor 257 wraps to the byte 1.")


func test_stance_shared_scale() -> void:
	# One 16.16 factor from FRAME 0 scales every frame; centering comes from frame
	# 0's scaled dims and skips at 127+. [orig: HUD_DrawStanceIndicator
	# @0x599fed..0x59a07e — 0x800000/max(w0,h0), (q16*dim+0x8000)>>16]
	assert_eq(HudStance.scale_q16(Vector2i(128, 128)), 0x10000)
	assert_eq(HudStance.scaled_dim(128, 0x10000), 128)
	assert_eq(HudStance.center_offset(Vector2i(128, 128), 0x10000), Vector2i.ZERO,
		"128 scales to 128 (>=127): no centering.")
	var q := HudStance.scale_q16(Vector2i(64, 32))
	assert_eq(q, 0x20000)
	assert_eq(HudStance.scaled_dim(64, q), 128, "The max dim fills the box.")
	assert_eq(HudStance.scaled_dim(32, q), 64)
	assert_eq(HudStance.center_offset(Vector2i(64, 32), q), Vector2i(0, 32),
		"The minor dim centers by (128-scaled)/2; the 128 dim does not.")
	assert_eq(HudStance.scaled_dim(256, q), 512,
		"Other frames scale by frame 0's factor blindly, like the original.")
	assert_eq(HudStance.scale_q16(Vector2i.ZERO), 0, "Zero dims are safe.")


func test_round_icon_count() -> void:
	# [orig: draw_hud_ammo_indicator @0x599b9c..0x599bc1]
	assert_eq(HudClipIndicator.round_icon_count(12, 90, 30, 1), 12)
	assert_eq(HudClipIndicator.round_icon_count(12, 90, 1, 1), 40, "Capacity 1 counts the pool, capped at 40.")
	assert_eq(HudClipIndicator.round_icon_count(12, 90, 30, 3), 4, "Divisor rounds up: (12+1)/3.")
	assert_eq(HudClipIndicator.round_icon_count(0, 90, 30, 1), 0)


func test_message_feed_expiry() -> void:
	# [orig: Chat_AddDebugMessage @0x4987f0 — 930-tick life, >=186-tick expiry stagger]
	var feed := HudMessages.new()
	feed.push("first", Color.WHITE, 0)
	feed.push("second", Color.WHITE, 0)
	assert_eq(feed.live_lines(0).size(), 2)
	assert_eq(feed.live_lines(929).size(), 2)
	assert_eq(feed.live_lines(930).size(), 1, "The first line expires at 930 ticks.")
	assert_eq(feed.live_lines(1115).size(), 1, "The second is staggered to 930+186.")
	assert_eq(feed.live_lines(1116).size(), 0)
	var empty_feed := HudMessages.new()
	empty_feed.push("", Color.WHITE, 0)
	assert_eq(empty_feed.live_lines(0).size(), 0, "Empty text is ignored.")


func test_message_feed_preserves_text_until_display_wrap() -> void:
	var feed := HudMessages.new()
	var text := "x".repeat(238)
	feed.push(text, Color.WHITE, 0)
	assert_eq(feed.live_lines(0)[0]["text"], text,
		"The source message remains whole until display width is known.")

	var rows := feed.display_lines(null, 16, 0, 0.0)
	assert_eq(rows.size(), 3)
	assert_eq(rows[0]["text"], "x".repeat(119))
	assert_eq(rows[1]["text"], "  " + "x".repeat(117),
		"Continuation indentation is part of the 119-character slot.")
	assert_eq(rows[2]["text"], "  xx", "Wrapping preserves the final remainder.")
	assert_eq(rows[0]["expire"], HudMessages.LINE_LIFE_TICKS)
	assert_eq(rows[1]["expire"], HudMessages.LINE_LIFE_TICKS,
		"Wrapping copies the source message timer; the 186-tick floor applies per push.")
	assert_eq(rows[2]["expire"], HudMessages.LINE_LIFE_TICKS)
	assert_eq(feed.display_lines(null, 16, HudMessages.LINE_LIFE_TICKS, 0.0).size(), 0)


func test_message_feed_keeps_newest_40_messages() -> void:
	var feed := HudMessages.new()
	feed.push("x".repeat(238), Color.WHITE, 0)
	for i in 40:
		feed.push("line %d" % i, Color.WHITE, 0)
	var live := feed.live_lines(0)
	assert_eq(live.size(), HudMessages.DISPLAY_SLOT_COUNT)
	assert_eq(live[0]["text"], "line 0", "The oldest source message is replaced.")
	assert_eq(live[-1]["text"], "line 39")
	assert_eq(live[0]["expire"], HudMessages.LINE_LIFE_TICKS + HudMessages.EXPIRY_STAGGER,
		"Evicting a wrapped source does not reset the persisted per-message expiry floor.")
	assert_eq(live[-1]["expire"],
		HudMessages.LINE_LIFE_TICKS + HudMessages.EXPIRY_STAGGER * 40)


func test_message_display_keeps_newest_40_wrapped_slots() -> void:
	var feed := HudMessages.new()
	feed.push("x".repeat(HudMessages.LINE_TEXT_MAX * 41), Color.WHITE, 0)
	var rows := feed.display_lines(null, 16, 0, 0.0)
	assert_eq(rows.size(), HudMessages.DISPLAY_SLOT_COUNT)
	for row in rows:
		assert_lte(String(row["text"]).length(), HudMessages.LINE_TEXT_MAX)
