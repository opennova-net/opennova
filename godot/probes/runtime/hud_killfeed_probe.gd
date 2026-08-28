extends GameProbe

## hud_killfeed: the message-feed screenshot probe. Drives the ported
## SYSTEM-ring surface (D-HUD-23) end to end on the HUD side of the loaded
## mission: real gametext "Canned Msg" templates resolved from the mounted
## install, the engine formatters (Simulation.format_feed_line /
## format_feed_camp_line) and HudOverlay.push_feed_line with the witnessed
## per-case colors, so the capture shows the real ring geometry (HUDSYSTEXT
## anchor, three rows, 18-design-px downward step) and the real line palette.
## Only the wire leg (S2C 0x1E -> netsim fold) is bypassed; ctest
## `feed_format` pins that half and needs a remote match to fire live. Needs
## a window.

const HUD_TIMEOUT_MS := 30_000
const SETTLE_MS := 3000
# The witnessed line palette: an uninvolved kill (grey), a medic line, and a
# blue-team camp line composed from the WPNames table.
const COLOR_KILL_UNINVOLVED := 0xFFAFAFAF
const COLOR_MEDIC := 0xFF008CEE
const COLOR_CAMP_BLUE := 0xFF00AFFF


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var out_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	var hud := await _wait_hud(ctx)
	if hud == null:
		return ProbeVerdict.failed("HudOverlay never appeared")
	var table: RtxtStringFile = Strings.get_table("gametext")
	if table == null:
		return ProbeVerdict.failed("gametext table unavailable")
	# Let streaming and the frame rate settle before the capture.
	await ctx.wait_ms(SETTLE_MS)
	var sim := ctx.sim()
	hud = await _wait_hud(ctx)
	if sim == null or hud == null:
		return ProbeVerdict.failed("the simulation or the HUD went away while settling")

	var kill_tmpl := table.get_string_in_section("Canned Msg", "STRCND04")
	var kill_line: String = sim.format_feed_line(kill_tmpl, "SPAGHETTI", "Belsman", "", "")
	hud.push_feed_line(kill_line, COLOR_KILL_UNINVOLVED)
	var medic_tmpl := table.get_string_in_section("Canned Msg", "STRCND45")
	var medic_line: String = sim.format_feed_line(medic_tmpl, "A-99", "elk road", "", "")
	hud.push_feed_line(medic_line, COLOR_MEDIC)
	var camp_tmpl := table.get_string_in_section("Canned Msg", "STRCND_FULLYCAMPED_BLUE")
	var wpname := ""
	if table.has_string_in_section("WPNames", "STRWPNAME001"):
		wpname = table.get_string_in_section("WPNames", "STRWPNAME001")
	var camp_line: String = sim.format_feed_camp_line(camp_tmpl, wpname)
	hud.push_feed_line(camp_line, COLOR_CAMP_BLUE)
	var lines := [kill_line, medic_line, camp_line]
	for line in lines:
		ctx.log("feed: %s" % line)

	await ctx.tree.process_frame
	var capture_path := out_dir.path_join("killfeed.png")
	var captured := await ProbeCapture.save_viewport_png(ctx.viewport(), capture_path)
	if captured:
		ctx.artifact("killfeed", capture_path, "png")
	var data := {
		"lines": lines,
		"templates": {"kill": kill_tmpl, "medic": medic_tmpl, "camp": camp_tmpl},
		"capture": capture_path if captured else "",
	}
	if kill_tmpl.is_empty() or medic_tmpl.is_empty() or camp_tmpl.is_empty():
		return ProbeVerdict.failed("a Canned Msg template resolved empty", data)
	if not captured:
		return ProbeVerdict.failed("the frame capture produced no image", data)
	return ProbeVerdict.passed("three feed lines pushed and captured", data)


## The HUD builds lazily on the first frame a mission has a local player.
func _wait_hud(ctx: ProbeContext) -> HudOverlay:
	var deadline := Time.get_ticks_msec() + HUD_TIMEOUT_MS
	while not ctx.cancelled and Time.get_ticks_msec() < deadline:
		var presenter := ctx.hud_presenter()
		if presenter != null:
			var hud := presenter.get_hud() as HudOverlay
			if hud != null:
				return hud
		await ctx.tree.process_frame
	return null
