extends RefCounted

## The Recent Messages (J-key) window lane of GameHudPresenter, plus the
## player-chat drain that feeds the CHAT ring it lists. The OldMessages action
## is an EDGE that TOGGLES the window flag — retail keeps it up until the next
## press, and the respawn init clears it [orig: `xor g_showMessageLog, 1`
## @0x49b55a in Input_HandleActionBinding (jumptable case 29); the clear in
## Game_InitRespawnState @0x49939a; the drawer HUD_DrawMessageLog @0x5b9d70
## called from Server_DrawStatusScreen @0x50b21f when the flag is set]. The
## binding ships in the controls catalog as "OldMessages" (row 56, vk 0x4A =
## 'J'), so nothing new is bound.
##
## The chat lines arrive already routed by the engine's channel table
## (Simulation.drain_chat_lines: sink 1 = the CHAT ring, 0 = the SYSTEM ring,
## 2 = the message queue, 3 = channel 3 — neither of the last two is a ring).
## The shell owns the title because it owns the string tables.

const SINK_SYSTEM := 0
const SINK_CHAT := 1

var _open := false      # the toggled window flag [orig: g_showMessageLog @0x24C18C0]
var _was_down := false  # the toggle's down-edge latch
var _pushed := false    # so the window clears exactly once on close


## The flag clears with the mission, as retail's respawn init clears its
## global [orig: @0x49939a].
func reset() -> void:
	_open = false
	_was_down = false
	_pushed = false


func is_open() -> bool:
	return _open


## The ShowScore toggle's respawn-init wrapper closes this window too
## [orig: Game_InitRespawnStateKeepingToggle @0x4993c0 -> Game_InitRespawnState @0x49939a].
func close(hud: HudOverlay) -> void:
	_open = false
	if _pushed and hud != null:
		hud.set_message_log_shown(false)
	_pushed = false


func update(hud: HudOverlay, sim: Simulation, down: bool, chorded: bool,
		active: bool) -> void:
	if hud == null:
		return
	if down and not _was_down and active and not chorded:
		_open = not _open
	_was_down = down
	flush_chat_lines(hud, sim)
	if _open and not _pushed:
		hud.set_message_log_title(_title())
		hud.set_message_log_shown(true)
		_pushed = true
	elif not _open and _pushed:
		hud.set_message_log_shown(false)
		_pushed = false


## Drain the folded S2C 0x14 lines into their rings. Runs whether or not the
## window is open — the rings are the live HUD feeds' too.
func flush_chat_lines(hud: HudOverlay, sim: Simulation) -> void:
	if hud == null or sim == null:
		return
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
	var table: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	if table != null and table.has_string_in_section(Strings.SECTION_OVERLAYS, "STROVER43"):
		return table.get_string_in_section(Strings.SECTION_OVERLAYS, "STROVER43")
	return ""
