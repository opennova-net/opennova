extends GutTest

# Pure-logic coverage for the host-neutral HUD view helpers (godot/engine/ui/hud_*.gd).
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


func test_draw_helpers_null_safe() -> void:
	# All draw helpers guard null inputs and must not crash.
	HudCrosshair.draw(null, null, Vector2.ZERO)
	HudStance.draw(null, null, Vector2.ZERO, Vector2.ONE)
	HudHealthBar.draw(null, Rect2(0, 0, 10, 2), 0.5, Color.GRAY, Color.GREEN, Color.YELLOW, Color.RED, Vector2.ONE)
	HudText.draw_text(null, null, Vector2.ZERO, Vector2.ONE, "x", Color.WHITE)
	assert_true(true, "Null-guarded draw helpers returned without error.")


func test_load_font_null_safe() -> void:
	assert_null(HudText.load_font(null, "anything.fnt"), "Null root returns null font.")
