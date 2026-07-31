extends Node

# Visual probe for the weapon-round slice (PR #226): captures the adjudicated
# crosshair protocol (D-HUD-9/10), the M82/Barrett SIGHTS scope card, and
# unscope-on-move, driven through the real standalone game's input path
# (same boot shape as fp_clean_probe). Windowed run:
#   NOVA_RESOURCE_DIR=<assets> "$GODOT_BIN" --path godot res://tests/weapon_round_probe.tscn
# Captures land in .scratch/weapon_round/. Note the JOX id is WPN_Barret (one T);
# the ease reads ~2 sim ticks per render frame fullscreen (62.5 Hz vs ~30-45 fps),
# so the mid-ease captures sit a few FRAMES after the RMB edge.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const OUT_DIR := "res://../.scratch/weapon_round"
const RIG_WEAPON := "WPN_Barret"  # Scoped; M82_1st carries the empty/dup bone rows (rig-fix demo)
const CARD_WEAPON := "WPN_RPG"    # Scoped (flags 1) + 2 sights rows in JOX -> the SIGHTS card

var _out_abs := ""
var _world = null
var _presenter = null


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	# NOVA_WR_EXPANSION: mount an expansion over the base game for this run (the
	# persisted key is what GameWorld reads — mirror of the retail /exp flag).
	var expn := OS.get_environment("NOVA_WR_EXPANSION").strip_edges()
	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "05TR.bms"
	var session: Dictionary = await StandaloneProbe.boot(self, root, bms, expn)
	if not String(session.get("error", "")).is_empty():
		push_error("[wr] " + String(session.error)); get_tree().quit(1); return
	_world = session.world
	_presenter = _find_by_method(get_tree().root, "set_debug_force_viewmodel")
	if _world == null:
		push_error("[wr] no weapon world"); get_tree().quit(1); return

	# Sanity: the standalone GameplayOverlay must carry the root viewport rect;
	# a zero size here clips the HUD and armory to nothing.
	var hud_presenter := _find_by_method(get_tree().root, "hud_objective_line")
	if hud_presenter != null:
		hud_presenter.tick()  # force the lazy HUD build
		var hud = hud_presenter.get_hud()
		if hud != null:
			var ov: Control = hud.get_parent()
			print("[wr] overlay=%s rect=%s hud_rect=%s" % [
				ov.name, str(ov.get_global_rect()), str(hud.get_global_rect())])

	# NOVA_WR_FIRE=1: the fire-chain diagnostic — equip NOVA_WR_WEAPON, hold LMB
	# ~100 frames, log the FSM view + the audio bank result every 10 frames.
	if OS.get_environment("NOVA_WR_FIRE") == "1":
		var wpn := OS.get_environment("NOVA_WR_WEAPON").strip_edges()
		if not wpn.is_empty():
			await _equip_direct(wpn)
		# NOVA_WR_ANIMTRACE=1: per-frame viewmodel playhead trace across a held
		# volley + the release — pins whether per-shot clip restarts land visually.
		if OS.get_environment("NOVA_WR_ANIMTRACE") == "1":
			await _anim_trace()
		else:
			await _fire_diag()
		print("[wr] done -> ", _out_abs)
		get_tree().quit()
		return

	# Try the armory at the spawn tents (zone-gated) BEFORE walking out.
	var armory_done := await _armory_sequence()

	# Open ground, level look.
	_hold(KEY_W, true)
	await _settle(300)
	_hold(KEY_W, false)
	await _settle(30)

	# Baseline: the spread crosshair at the 1P design-center pin (whatever's equipped).
	_log_view("hip baseline")
	await _capture("01_hip_crosshair.png")

	if not armory_done:
		await _equip_direct(RIG_WEAPON)
	_log_view("barrett hip (rig fix)")
	await _capture("02_barrett_rig_hip.png")

	# The card demo weapon: Scoped + authored sights rows (the snipers use the
	# separate magnified-scope overlay — the recorded hud-re deferral).
	await _equip_direct(CARD_WEAPON)
	_log_view("rpg hip")
	await _capture("02b_rpg_hip.png")

	# Mid-ease: the crosshair MUST still draw a few frames after the RMB edge
	# (D-HUD-9: it yields only at the settled sight view). ~2 sim ticks/frame.
	_mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _settle(2)
	_mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _settle(1)
	_log_view("mid-ease")
	await _capture("03_ads_ease_crosshair_up.png")

	# Settled: fraction 1 -> the crosshair yields AND the Scoped weapon draws its
	# SIGHTS card INSTEAD of the FP viewmodel.
	await _settle(50)
	_log_view("settled (card)")
	await _capture("04_sights_card_settled.png")

	# Unscope-on-move: a movement key while SETTLED on a Scoped weapon forces the
	# full unscope [orig: @0x4df4c9]. Capture mid-drop, then at rest.
	_hold(KEY_W, true)
	await _settle(4)
	_log_view("moving (unscope firing)")
	await _capture("05_unscope_on_move_drop.png")
	await _settle(40)
	_hold(KEY_W, false)
	await _settle(20)
	_log_view("stopped (back at hip)")
	await _capture("06_back_at_hip.png")

	# Third person: the crosshair anchors at the PROJECTED aim point, not the
	# screen center (D-HUD-10) — pitch down a touch so the offset is visible.
	_look(Vector2(0, 140))
	await _settle(12)
	_hold(KEY_F4, true)
	await _settle(2)
	_hold(KEY_F4, false)
	await _settle(40)
	_log_view("third person")
	await _capture("07_3p_projected_aim.png")

	print("[wr] done -> ", _out_abs)
	get_tree().quit()


# The armory over live play + the Shift ACCEPT accelerator. Returns true when the
# sniper was applied through the armory (false -> caller falls back to the direct
# apply, still demoing the same ACCEPT plumbing).
func _armory_sequence() -> bool:
	_hold(KEY_SHIFT, true)
	await _settle(4)
	var menu := get_tree().root.find_child("ArmoryMenu", true, false)
	if menu == null or not menu.visible:
		_hold(KEY_SHIFT, false)
		print("[wr] armory did not open here (out of zone) — direct apply later")
		return false
	await _settle(20)
	await _capture("00_armory_open_live_play.png")

	var primary := menu.find_child("PRIMARY", true, false)
	var row := -1
	if primary != null:
		for i in range(primary.get_item_count()):
			if "BARRET" in String(primary.get_item_text(i)).to_upper():
				row = i
				break
		print("[wr] PRIMARY rows=%d barrett_row=%d" % [primary.get_item_count(), row])
	if row >= 0:
		primary.select(row)
		await _settle(10)

	# The ACCEPT hotkey: the opener is still HELD from the open — release once to
	# arm [orig: @0x4de2d0], press again to ACCEPT [orig: @0x5674a8].
	_hold(KEY_SHIFT, false)
	await _settle(5)
	_hold(KEY_SHIFT, true)
	await _settle(3)
	_hold(KEY_SHIFT, false)
	await _settle(60)
	print("[wr] Shift ACCEPT accelerator: menu.visible=%s" % str(menu.visible))
	return row >= 0 and not menu.visible


func _equip_direct(weapon: String) -> void:
	# The ACCEPT plumbing minus the UI: install the FSM + rebuild the viewmodel
	# (game_world.set_local_player_weapon_by_name = the armory apply path).
	if _world == null or not _world.has_method("set_local_player_weapon_by_name"):
		return
	if not _world.set_local_player_weapon_by_name(weapon):
		print("[wr] %s not in this root's weapon.def — staying on the default" % weapon)
		return
	if _presenter != null and _presenter.has_method("refresh_viewmodel"):
		_presenter.refresh_viewmodel()
	await _settle(90)


# Hold LMB and log the FSM/audio state — which layer breaks: the FSM (serials),
# the ammo (clip), or the sound (bank lookup).
# Per-frame viewmodel playhead trace: hold LMB ~70 frames, then release and watch
# ~50 more. Prints the active clip key + playhead seconds per frame alongside the
# FSM view — per-shot restarts must show the playhead snapping back near 0 at the
# fire cadence, and the release must NOT be the first visible kick.
func _anim_trace() -> void:
	var tracef := FileAccess.open(_out_abs + "/animtrace.log", FileAccess.WRITE)
	var part: Node = null
	for _attempt in range(120):  # the equip rebuilds the viewmodel over several frames
		if _presenter != null and _presenter.vm_parts().size() > 0:
			part = _presenter.vm_parts()[0]
			break
		await get_tree().process_frame
	if part == null:
		var diag := "no viewmodel part: presenter=%s vm=%s def=%s" % [
				str(_presenter != null),
				str(_presenter.viewmodel()) if _presenter != null else "-",
				str(_world.local_player_viewmodel_def() != null)
						if _world.has_method("local_player_viewmodel_def") else "?"]
		print("[wr] ANIMTRACE: ", diag)
		if tracef != null:
			tracef.store_line(diag)
			tracef.close()
		return
	var v0 = _world.local_player_weapon_view()
	var pre := "[wr] ANIMTRACE pre: key=%s t=%.3f clip=%d act=%d" % [
			part.get_active_body_clip(), part.get_animation_time(),
			v0.clip, v0.current_action]
	print(pre)
	if tracef != null: tracef.store_line(pre)
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	for i in range(70):
		await get_tree().process_frame
		var v = _world.local_player_weapon_view()
		var hl := "[wr] HOLD f=%02d key=%-16s t=%.3f fired=%d act=%d clip=%d" % [
				i, part.get_active_body_clip(), part.get_animation_time(),
				v.fired_serial, v.current_action, v.clip]
		print(hl)
		if tracef != null: tracef.store_line(hl)
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	print("[wr] RELEASE")
	if tracef != null: tracef.store_line("[wr] RELEASE")
	for i in range(50):
		await get_tree().process_frame
		var v = _world.local_player_weapon_view()
		var rl := "[wr] REL  f=%02d key=%-16s t=%.3f fired=%d act=%d" % [
				i, part.get_active_body_clip(), part.get_animation_time(),
				v.fired_serial, v.current_action]
		print(rl)
		if tracef != null: tracef.store_line(rl)


	if tracef != null:
		tracef.close()


func _fire_diag() -> void:
	var audio = _world.get_mission_audio() if _world.has_method("get_mission_audio") else null
	if audio != null:
		for set_name in ["GS_M4", "GF_RL_AR15_2", "SHELLDROP", "DRY_TRIGGER"]:
			print("[wr] bank probe %-14s -> %s" % [set_name,
					str(audio.fire_soundset(set_name, _world.get_sim().get_local_player_position()))])
	var t0 = _world.local_player_weapon_view()
	print("[wr] pre-fire: active=%s clip=%s reserve=%s act=%s" % [
			str(t0.active), str(t0.clip), str(t0.reserve), str(t0.current_action)])
	var vdef: PlayerViewmodelDef = _world.local_player_viewmodel_def() \
			if _world.has_method("local_player_viewmodel_def") else null
	if vdef != null:
		print("[wr] vmdef %s: pos=%s rot=%s tpos=%s fov=%s gfx1=%s adm=%s" % [
				vdef.weapon_name, str(vdef.pos_units), str(vdef.rot_bias_deg),
				str(vdef.tpos_units), str(vdef.renderfov_h_deg), vdef.gfx1, vdef.animadm])
	await _capture("90_diag_hip.png")
	# ADS alignment check: raise, settle, capture, drop (the Sighted tpos view).
	_mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _settle(3)
	_mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _settle(40)
	_log_view("diag ADS settled")
	await _capture("91_diag_ads.png")
	_mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _settle(3)
	_mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _settle(30)
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	for i in range(10):
		await _settle(10)
		var v = _world.local_player_weapon_view()
		print("[wr] fire t+%03d: act=%d fired=%d dry=%d clip=%d res=%d endss=%s" % [
				(i + 1) * 10, v.current_action, v.fired_serial, v.dry_serial,
				v.clip, v.reserve, v.action_end_soundset])
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	await _settle(10)
	var vf = _world.local_player_weapon_view()
	print("[wr] post-fire: act=%d fired=%d clip=%d res=%d" % [
			vf.current_action, vf.fired_serial, vf.clip, vf.reserve])
	# Variant-ring rotation: reload three times and log the served reload variant per
	# play — REVVY M4's row is "m4_1r" "m4_1r" "m4_1r2", so the plays must walk the
	# ring in .adm order from wherever the bake's auto-delay reads left the head.
	# [orig: AnimMap_PlayAnimBySlot @0x40bda0]
	for r in range(3):
		var press := true
		var seen_serial: int = _world.local_player_weapon_view().play_serial
		_hold(KEY_R, true)
		await _settle(2)
		_hold(KEY_R, false)
		for _w in range(30):
			await _settle(5)
			var rv = _world.local_player_weapon_view()
			if rv.anim_key.nocasecmp_to("anim_wpn_reload") == 0 and rv.play_serial != seen_serial:
				print("[wr] reload %d: key=%s variant=%d clip=%d" % [r, rv.anim_key, rv.anim_variant, rv.clip])
				press = false
				break
		if press:
			print("[wr] reload %d: NOT OBSERVED" % r)
		await _settle(90)  # let the reload finish before the next request
		_mouse_btn(MOUSE_BUTTON_LEFT, true)  # spend a round so the next reload is allowed
		await _settle(6)
		_mouse_btn(MOUSE_BUTTON_LEFT, false)
		await _settle(20)
	# The armory-name thread: is the loadout text table resolvable on this root?
	var root: NovaResourceRoot = _world.get_resource_root() \
			if _world.has_method("get_resource_root") else null
	if root != null:
		for f in ["menutxt.BIN", "Game.bin", "gametext.bin"]:
			print("[wr] root has %-12s -> %s" % [f, str(root.has_file(f))])
	for table in ["menutxt", "gametext"]:
		var tb = NovaStrings.get_table(table)
		print("[wr] strings %-9s -> %s  WepDes/WEAP_SHORT_M4=%s" % [table, str(tb != null),
				NovaStrings.lookup(table, "WepDes", "WEAP_SHORT_M4") if tb != null else "<no table>"])


func _log_view(stage: String) -> void:
	var pv = _world.local_player_view() if _world != null and _world.has_method("local_player_view") else null
	var wv = _world.local_player_weapon_view() if _world != null else null
	print("[wr] %s: engaged=%s fraction=%.2f card=%s clip=%s" % [
		stage,
		str(pv.scope_engaged) if pv != null else "<null>",
		pv.scope_fraction if pv != null else -1.0,
		str(pv.scope_card_active) if pv != null else "<null>",
		str(wv.clip) if wv != null else "<null>"])


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for ch in node.get_children():
		var f := _find_by_method(ch, method)
		if f != null:
			return f
	return null


func _settle(n: int) -> void:
	for _i in n:
		await get_tree().process_frame


func _hold(k: Key, down: bool) -> void:
	var e := InputEventKey.new(); e.keycode = k; e.physical_keycode = k; e.pressed = down
	Input.parse_input_event(e)


func _mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = b
	e.pressed = down
	Input.parse_input_event(e)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new(); mm.relative = total / 10.0
		Input.parse_input_event(mm)


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[wr] wrote ", name)
