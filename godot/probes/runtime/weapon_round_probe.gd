extends GameProbe

## weapon_round: the visual probe for the weapon-round slice (PR #226) on the
## loaded mission, driven through the real input path: the adjudicated
## crosshair protocol (D-HUD-9/10), the `card_weapon` SIGHTS scope card,
## unscope-on-move and the third-person projected aim point, with the armory
## at the spawn tents tried first (zone-gated; `rig_weapon` is applied
## directly when out of zone). With `fire` the fire-chain diagnostic instead:
## equip `weapon` when given, hold LMB and log the FSM view + the audio bank
## result every ten frames, then the reload variant ring; with `animtrace`
## the per-frame viewmodel playhead trace across a held volley and the
## release (animtrace.log). Captures land in `output_dir` or the run's
## artifact dir. The JOX id is WPN_Barret (one T); the ease reads ~2 sim
## ticks per render frame, so the mid-ease captures sit a few FRAMES after
## the RMB edge. Needs a window.

const EQUIP_SETTLE_FRAMES := 90
const WALK_OUT_FRAMES := 300
const REBUILD_WAIT_FRAMES := 120
const TRACE_HOLD_FRAMES := 70
const TRACE_RELEASE_FRAMES := 50
const FIRE_SAMPLES := 10
const FIRE_SAMPLE_FRAMES := 10
const RELOAD_PLAYS := 3
const RELOAD_POLLS := 30
const BANK_SETS := ["GS_M4", "GF_RL_AR15_2", "SHELLDROP", "DRY_TRIGGER"]
const TEXT_FILES := ["menutxt.BIN", "Game.bin", "gametext.bin"]

var _ctx: ProbeContext
var _out_dir := ""
var _captures: Array = []


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	_out_dir = ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	var rig_weapon := String(ctx.args.get("rig_weapon", "WPN_Barret"))
	var card_weapon := String(ctx.args.get("card_weapon", "WPN_RPG"))
	var fire := bool(ctx.args.get("fire", false))
	var weapon := String(ctx.args.get("weapon", ""))
	var animtrace := bool(ctx.args.get("animtrace", false))
	ctx.defer_restore(func() -> void:
		for key in [KEY_W, KEY_SHIFT, KEY_R]:
			ProbeInput.hold(key, false)
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		ProbeInput.mouse_btn(MOUSE_BUTTON_RIGHT, false))
	if ctx.world() == null or ctx.presenter() == null:
		return ProbeVerdict.failed("no world/presenter")

	# Sanity: the gameplay overlay must carry the root viewport rect; a zero
	# size here clips the HUD and the armory to nothing.
	var hud_presenter := ctx.hud_presenter()
	if hud_presenter != null:
		hud_presenter.tick()  # force the lazy HUD build
		var hud := hud_presenter.get_game_hud()
		var overlay := hud.get_parent() as Control if hud != null else null
		if overlay != null:
			ctx.log("overlay=%s rect=%s hud_rect=%s" % [
					overlay.name, str(overlay.get_global_rect()), str(hud.get_global_rect())])

	if fire:
		if not weapon.is_empty():
			await _equip_direct(weapon)
		if animtrace:
			await _anim_trace()
		else:
			await _fire_diag()
		return _verdict("fire diagnostic done")

	# The armory at the spawn tents (zone-gated) BEFORE walking out.
	var armory_done := await _armory_sequence(rig_weapon)

	# Open ground, level look.
	ProbeInput.hold(KEY_W, true)
	await ctx.wait_frames(WALK_OUT_FRAMES)
	ProbeInput.hold(KEY_W, false)
	await ctx.wait_frames(30)

	# Baseline: the spread crosshair at the 1P design-center pin (whatever is
	# equipped).
	_log_view("hip baseline")
	await _capture("01_hip_crosshair.png")

	if not armory_done:
		await _equip_direct(rig_weapon)
	_log_view("%s hip (rig fix)" % rig_weapon)
	await _capture("02_rig_weapon_hip.png")

	# The card demo weapon: Scoped + authored sights rows (the snipers use the
	# separate magnified-scope overlay, the recorded hud-re deferral).
	await _equip_direct(card_weapon)
	_log_view("%s hip" % card_weapon)
	await _capture("02b_card_weapon_hip.png")

	# Mid-ease: the crosshair MUST still draw a few frames after the RMB edge
	# (D-HUD-9: it yields only at the settled sight view). ~2 sim ticks/frame.
	ProbeInput.mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await ctx.wait_frames(2)
	ProbeInput.mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await ctx.wait_frames(1)
	_log_view("mid-ease")
	await _capture("03_ads_ease_crosshair_up.png")

	# Settled: fraction 1 -> the crosshair yields AND the Scoped weapon draws
	# its SIGHTS card INSTEAD of the FP viewmodel.
	await ctx.wait_frames(50)
	_log_view("settled (card)")
	await _capture("04_sights_card_settled.png")

	# Unscope-on-move: a movement key while SETTLED on a Scoped weapon forces
	# the full unscope [orig: @0x4df4c9]. Capture mid-drop, then at rest.
	ProbeInput.hold(KEY_W, true)
	await ctx.wait_frames(4)
	_log_view("moving (unscope firing)")
	await _capture("05_unscope_on_move_drop.png")
	await ctx.wait_frames(40)
	ProbeInput.hold(KEY_W, false)
	await ctx.wait_frames(20)
	_log_view("stopped (back at hip)")
	await _capture("06_back_at_hip.png")

	# Third person: the crosshair anchors at the PROJECTED aim point, not the
	# screen center (D-HUD-10); pitch down a touch so the offset is visible.
	ProbeInput.look(Vector2(0, 140))
	await ctx.wait_frames(12)
	# On-foot third person is the debug override (no gameplay key resolves it).
	var presenter := ctx.presenter()
	if presenter != null:
		presenter.set_debug_third_person(true)
		ctx.defer_restore(func() -> void:
			var live := _ctx.presenter()
			if live != null:
				live.set_debug_third_person(false))
	await ctx.wait_frames(40)
	_log_view("third person")
	await _capture("07_3p_projected_aim.png")
	return _verdict("crosshair, sights card, unscope-on-move and third person captured")


func _verdict(summary: String) -> ProbeVerdict:
	var data := {"captures": _captures.duplicate(), "output_dir": _out_dir}
	if _ctx.cancelled:
		return ProbeVerdict.failed("cancelled", data)
	return ProbeVerdict.passed(summary, data)


## The armory over live play + the Shift ACCEPT accelerator. True when
## `rig_weapon` was applied through the armory (false -> the caller falls
## back to the direct apply, still demoing the same ACCEPT plumbing).
func _armory_sequence(rig_weapon: String) -> bool:
	ProbeInput.hold(KEY_SHIFT, true)
	await _ctx.wait_frames(4)
	var armory := _ctx.armory_presenter()
	if armory == null or not armory.is_open():
		ProbeInput.hold(KEY_SHIFT, false)
		_ctx.log("armory did not open here (out of zone); direct apply later")
		return false
	await _ctx.wait_frames(20)
	await _capture("00_armory_open_live_play.png")

	var row := -1
	var driver := armory.get_menu_driver()
	var needle := rig_weapon.trim_prefix("WPN_").to_upper()
	if driver != null:
		var primary := driver.widget_id("PRIMARY")
		if primary >= 0:
			for i in range(driver.item_count(primary)):
				if needle in driver.item_text(primary, i).to_upper():
					row = i
					break
			_ctx.log("PRIMARY rows=%d %s_row=%d" % [driver.item_count(primary), needle, row])
			if row >= 0:
				driver.select_row(primary, row)
				await _ctx.wait_frames(10)

	# The ACCEPT hotkey: the opener is still HELD from the open; release once
	# to arm [orig: @0x4de2d0], press again to ACCEPT [orig: @0x5674a8].
	ProbeInput.hold(KEY_SHIFT, false)
	await _ctx.wait_frames(5)
	ProbeInput.hold(KEY_SHIFT, true)
	await _ctx.wait_frames(3)
	ProbeInput.hold(KEY_SHIFT, false)
	await _ctx.wait_frames(60)
	var still_open := is_instance_valid(armory) and armory.is_open()
	_ctx.log("Shift ACCEPT accelerator: armory open=%s" % str(still_open))
	return row >= 0 and not still_open


## The ACCEPT plumbing minus the UI: install the FSM + rebuild the viewmodel
## (GameWorld.set_local_player_weapon_by_name = the armory apply path).
func _equip_direct(weapon: String) -> void:
	var world := _ctx.world()
	if world == null:
		return
	if not world.set_local_player_weapon_by_name(weapon):
		_ctx.log("%s not in this root's weapon.def; staying on the default" % weapon)
		return
	var presenter := _ctx.presenter()
	if presenter != null:
		presenter.refresh_viewmodel()
	await _ctx.wait_frames(EQUIP_SETTLE_FRAMES)


## Per-frame viewmodel playhead trace: hold LMB, then release and keep
## watching. Records the active clip key + playhead seconds per frame
## alongside the FSM view: per-shot restarts must show the playhead snapping
## back near 0 at the fire cadence, and the release must NOT be the first
## visible kick.
func _anim_trace() -> void:
	var lines: PackedStringArray = []
	var part: ObjectModel = null
	for _attempt in range(REBUILD_WAIT_FRAMES):  # the equip rebuilds the viewmodel over frames
		var presenter := _ctx.presenter()
		if presenter != null and presenter.vm_parts().size() > 0:
			part = presenter.vm_parts()[0]
			break
		await _ctx.tree.process_frame
	var world := _ctx.world()
	if part == null or world == null:
		var presenter := _ctx.presenter()
		var diag := "no viewmodel part: presenter=%s vm=%s def=%s" % [
				str(presenter != null),
				str(presenter.viewmodel()) if presenter != null else "-",
				str(world.local_player_viewmodel_def() != null) if world != null else "?"]
		_trace(lines, "ANIMTRACE: " + diag)
		_write_trace(lines)
		return
	var v0 := world.local_player_weapon_view()
	_trace(lines, "ANIMTRACE pre: key=%s t=%.3f clip=%d act=%d" % [
			part.get_active_body_clip(), part.get_animation_time(), v0.clip, v0.current_action])
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
	for i in range(TRACE_HOLD_FRAMES):
		await _ctx.tree.process_frame
		if not is_instance_valid(part) or _ctx.cancelled:
			break
		var v := _ctx.world().local_player_weapon_view()
		_trace(lines, "HOLD f=%02d key=%-16s t=%.3f fired=%d act=%d clip=%d" % [
				i, part.get_active_body_clip(), part.get_animation_time(),
				v.fired_serial, v.current_action, v.clip])
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
	_trace(lines, "RELEASE")
	for i in range(TRACE_RELEASE_FRAMES):
		await _ctx.tree.process_frame
		if not is_instance_valid(part) or _ctx.cancelled:
			break
		var v := _ctx.world().local_player_weapon_view()
		_trace(lines, "REL  f=%02d key=%-16s t=%.3f fired=%d act=%d" % [
				i, part.get_active_body_clip(), part.get_animation_time(),
				v.fired_serial, v.current_action])
	_write_trace(lines)


func _trace(lines: PackedStringArray, text: String) -> void:
	lines.append(text)
	_ctx.log(text)


func _write_trace(lines: PackedStringArray) -> void:
	var path := _out_dir.path_join("animtrace.log")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_ctx.log("could not write %s" % path)
		return
	for line in lines:
		file.store_line(line)
	file.close()
	_ctx.artifact("animtrace", path, "log")


## Hold LMB and log the FSM/audio state: which layer breaks, the FSM
## (serials), the ammo (clip), or the sound (bank lookup).
func _fire_diag() -> void:
	var world := _ctx.world()
	var sim := _ctx.sim()
	if world == null or sim == null:
		_ctx.log("fire diag: no world/sim")
		return
	var audio := world.get_mission_audio()
	if audio != null:
		for set_name in BANK_SETS:
			_ctx.log("bank probe %-14s -> %s" % [set_name,
					str(audio.fire_soundset(set_name, sim.get_local_player_position()))])
	var t0 := world.local_player_weapon_view()
	_ctx.log("pre-fire: active=%s clip=%s reserve=%s act=%s" % [
			str(t0.active), str(t0.clip), str(t0.reserve), str(t0.current_action)])
	var vdef := world.local_player_viewmodel_def()
	if vdef != null:
		_ctx.log("vmdef %s: pos=%s rot=%s tpos=%s fov=%s gfx1=%s adm=%s" % [
				vdef.weapon_name, str(vdef.pos_units), str(vdef.rot_bias_deg),
				str(vdef.tpos_units), str(vdef.renderfov_h_deg), vdef.gfx1, vdef.animadm])
	await _capture("90_diag_hip.png")
	# ADS alignment check: raise, settle, capture, drop (the Sighted tpos view).
	ProbeInput.mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _ctx.wait_frames(3)
	ProbeInput.mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _ctx.wait_frames(40)
	_log_view("diag ADS settled")
	await _capture("91_diag_ads.png")
	ProbeInput.mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _ctx.wait_frames(3)
	ProbeInput.mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _ctx.wait_frames(30)
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
	for i in range(FIRE_SAMPLES):
		await _ctx.wait_frames(FIRE_SAMPLE_FRAMES)
		var v := _ctx.world().local_player_weapon_view()
		_ctx.log("fire t+%03d: act=%d fired=%d dry=%d clip=%d res=%d endss=%s" % [
				(i + 1) * FIRE_SAMPLE_FRAMES, v.current_action, v.fired_serial, v.dry_serial,
				v.clip, v.reserve, v.action_end_soundset])
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
	await _ctx.wait_frames(10)
	var vf := _ctx.world().local_player_weapon_view()
	_ctx.log("post-fire: act=%d fired=%d clip=%d res=%d" % [
			vf.current_action, vf.fired_serial, vf.clip, vf.reserve])
	# Variant-ring rotation: reload three times and log the served reload
	# variant per play. REVVY M4's row is "m4_1r" "m4_1r" "m4_1r2", so the
	# plays must walk the ring in .adm order from wherever the bake's
	# auto-delay reads left the head. [orig: AnimMap_PlayAnimBySlot @0x40bda0]
	for r in range(RELOAD_PLAYS):
		var observed := false
		var seen_serial: int = _ctx.world().local_player_weapon_view().play_serial
		ProbeInput.hold(KEY_R, true)
		await _ctx.wait_frames(2)
		ProbeInput.hold(KEY_R, false)
		for _w in range(RELOAD_POLLS):
			await _ctx.wait_frames(5)
			var rv := _ctx.world().local_player_weapon_view()
			if rv.anim_key.nocasecmp_to("anim_wpn_reload") == 0 and rv.play_serial != seen_serial:
				_ctx.log("reload %d: key=%s variant=%d clip=%d" % [r, rv.anim_key, rv.anim_variant, rv.clip])
				observed = true
				break
		if not observed:
			_ctx.log("reload %d: NOT OBSERVED" % r)
		await _ctx.wait_frames(90)  # let the reload finish before the next request
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)  # spend a round so the next reload is allowed
		await _ctx.wait_frames(6)
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		await _ctx.wait_frames(20)
	# The armory-name thread: is the loadout text table resolvable on this root?
	var root := _ctx.world().get_resource_root()
	if root != null:
		for f in TEXT_FILES:
			_ctx.log("root has %-12s -> %s" % [f, str(root.has_file(f))])
	for table in ["menutxt", "gametext"]:
		var tb := Strings.get_table(table)
		_ctx.log("strings %-9s -> %s  WepDes/WEAP_SHORT_M4=%s" % [table, str(tb != null),
				Strings.lookup(table, "WepDes", "WEAP_SHORT_M4") if tb != null else "<no table>"])


func _log_view(stage: String) -> void:
	var world := _ctx.world()
	var pv := world.local_player_view() if world != null else null
	var wv := world.local_player_weapon_view() if world != null else null
	_ctx.log("%s: engaged=%s fraction=%.2f card=%s clip=%s" % [
			stage,
			str(pv.scope_engaged) if pv != null else "<null>",
			pv.scope_fraction if pv != null else -1.0,
			str(pv.scope_card_active) if pv != null else "<null>",
			str(wv.clip) if wv != null else "<null>"])


func _capture(file_name: String) -> void:
	var path := _out_dir.path_join(file_name)
	if await ProbeCapture.save_viewport_png(_ctx.viewport(), path):
		_captures.append(path)
		_ctx.artifact(file_name.get_basename(), path, "png")
		_ctx.log("wrote " + file_name)
	else:
		_ctx.log("capture %s produced no image" % file_name)
