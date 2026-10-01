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
## that flag; the rings it lists are fed in wire order by the presenter's
## feed flush (HudOverlay.post_feed_lines). The shell owns the title because
## it owns the string tables.

var _pushed := false    # so the window clears exactly once on close


func reset() -> void:
	_pushed = false


func update(hud: HudOverlay, open: bool) -> void:
	if hud == null:
		return
	if open and not _pushed:
		hud.set_message_log_title(_title())
		hud.set_message_log_shown(true)
		_pushed = true
	elif not open and _pushed:
		hud.set_message_log_shown(false)
		_pushed = false


## The stdbox title from gametext Overlays/STROVER43; absent, the box draws
## untitled (no literal is witnessed for this one).
func _title() -> String:
	return Strings.lookup_or(Strings.TABLE_GAMETEXT, Strings.SECTION_OVERLAYS, "STROVER43", "")
