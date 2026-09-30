extends RefCounted

## The Recent Messages (J-key) window lane of GameHudPresenter, plus the
## player-chat drain that feeds the CHAT ring it lists. The OldMessages edge
## and the toggled window flag are the engine's (hud/hud_toggles.h, through
## the presenter's HudToggles: retail keeps the window up until the next
## press, the respawn init clears it, and the ShowScore flip closes it
## [orig: `xor g_ShowMessageLog, 1` @0x49b55a in Input_HandleActionBinding
## (jumptable case 29); the clear in Game_InitRespawnState @0x49939a; the
## drawer HUD_DrawMessageLog @0x5b9d70 called from Server_DrawStatusScreen
## @0x50b21f when the flag is set]). This lane shows or hides the window for
## that flag and drains the chat lines.
##
## The chat lines arrive already routed by the engine's channel table
## (Simulation.drain_chat_lines: sink 1 = the CHAT ring, 0 = the SYSTEM ring,
## 2 = the message queue, 3 = channel 3 — neither of the last two is a ring).
## The shell owns the title because it owns the string tables.

const SINK_SYSTEM := 0
const SINK_CHAT := 1

var _pushed := false    # so the window clears exactly once on close


func reset() -> void:
	_pushed = false


func update(hud: HudOverlay, sim: Simulation, open: bool) -> void:
	if hud == null:
		return
	flush_chat_lines(hud, sim)
	if open and not _pushed:
		hud.set_message_log_title(_title())
		hud.set_message_log_shown(true)
		_pushed = true
	elif not open and _pushed:
		hud.set_message_log_shown(false)
		_pushed = false


## Drain the folded S2C 0x14 lines into their rings, and the S2C 0x32
## join/leave lines (formatted natively) into the SYSTEM ring. Runs whether or
## not the window is open — the rings are the live HUD feeds' too.
func flush_chat_lines(hud: HudOverlay, sim: Simulation) -> void:
	if hud == null or sim == null:
		return
	hud.post_game_text_lines(sim, Strings.get_table(Strings.TABLE_GAMETEXT))
	for row_v in sim.drain_chat_lines():
		var row: ChatLineRow = row_v
		if row.text.is_empty():
			continue
		match row.sink:
			SINK_CHAT:
				hud.push_chat_line(row.text, row.argb)
			SINK_SYSTEM:
				hud.push_feed_line(row.text, row.argb)
			_:
				pass  # the message queue / channel 3: no ring


## The stdbox title from gametext Overlays/STROVER43; absent, the box draws
## untitled (no literal is witnessed for this one).
func _title() -> String:
	return Strings.lookup_or(Strings.TABLE_GAMETEXT, Strings.SECTION_OVERLAYS, "STROVER43", "")
