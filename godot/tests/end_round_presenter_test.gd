extends GutTest

# The MP end-of-round presenter (godot/game/world/end_round_presenter.gd): the
# no-announcement null path (no overlay, no screen, no signal), the
# once-only latches reset, and the teardown — the presenter's lanes over a
# null WorldView (no simulation). The overlay element itself is pinned in
# hud_overlay_test; the ladder/feed math in the end_round_overlay and
# stat_screen_feed ctests.
# [orig: HUD_DrawOverlayPanels @0x5c0060 -> UI_ProcessEndRoundScreenTransition @0x5b8600]

const EndRoundPresenterScript := preload("res://game/world/end_round_presenter.gd")


func test_no_announcement_is_inert() -> void:
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var presenter = EndRoundPresenterScript.new()
	presenter.setup(WorldView.new(), overlay, null)  # the null view: no sim
	presenter.connect_shell(null, null, func() -> void: pass, func() -> void: pass)
	add_child_autofree(presenter)
	watch_signals(presenter)
	presenter.tick()
	presenter.tick()
	assert_false(presenter.is_open(), "no 0x1D announcement opens nothing")
	assert_false(presenter.is_overlay_shown(), "no announcement shows no overlay")
	assert_signal_emit_count(presenter, "opened", 0)
	presenter.reset()
	presenter.teardown()
	assert_false(presenter.is_open(), "teardown leaves the screen closed")
	assert_null(presenter.get_menu_driver(), "no driver exists before the first open")
