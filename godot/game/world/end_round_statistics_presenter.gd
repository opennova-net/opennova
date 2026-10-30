extends RefCounted

## The SP "Show Score" statistics panel lane of GameHudPresenter: one titled
## label box with four label/value rows, shown or hidden for the engine's
## ShowScore flag (hud/hud_toggles.h, through the presenter's HudToggles). The
## rows, their title and labels are the engine's (hud/end_round_statistics.h:
## the "%d/%d" forms, the enemy-kill clamp, end_round_statistics_text's
## gametext resolution); this lane pushes them to the HUD element while the
## panel is open, every frame (the counters are live), and clears it once on
## the close.

var _pushed := false    # so the panel clears exactly once on close


func reset() -> void:
	_pushed = false


func update(hud: HudOverlay, sim: Simulation, open: bool) -> void:
	if hud == null:
		return
	if open:
		_push(hud, sim)
		_pushed = true
	elif _pushed:
		hud.set_end_round_statistics(false, false, "", PackedStringArray(),
				PackedStringArray())
		_pushed = false


func _push(hud: HudOverlay, sim: Simulation) -> void:
	if sim == null:
		return
	var stats := sim.get_end_round_statistics()
	if stats == null:
		return
	var gametext := Strings.get_table(Strings.TABLE_GAMETEXT)
	hud.set_end_round_statistics(true, stats.raised, stats.title(gametext),
			stats.labels(gametext), stats.values)
