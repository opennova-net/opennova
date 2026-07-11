class_name NovaGameHudHost
extends Node

## Hosts the in-game HUD (GameHud) over a live GameWorld for BOTH shells — the game
## (main_game) and ONED play-in-editor (mission_play_controller) — one HUD code path
## (editor-runtime parity). Owns the lazy build (hudpos.def layout + string tables),
## the per-frame info rebuild from the authoritative local player, and the mission
## text feed. [orig: HUD_RenderAllOverlays @0x5a8070; HUD_BuildEntityInfo @0x4b8440;
## gametext Game_InitSubsystems @0x4a6cd0; mission .bin
## TextResource_LoadMissionTextBin @0x51ed90]

const GameHudScript := preload("res://engine/world/game_hud.gd")

var _world = null           # GameWorld
var _player_host = null     # LocalPlayerHost (aim point + view record source)
var _ui_parent: Node = null

var _game_hud = null        # GameHud, built on the first frame a mission has a local player
var _warned_no_player := false
var _hud_weapon_name := ""  # equipped-weapon cache (re-resolves WepDes on change)
# The latest mission-effect text line (ADR 0018 read seam via hud_objective_line()).
var _hud_objective := ""


func setup(world, player_host, ui_parent: Node) -> void:
	_world = world
	_player_host = player_host
	_ui_parent = ui_parent
	if _world != null and _world.has_signal("mission_effects") \
			and not _world.mission_effects.is_connected(apply_mission_effects):
		_world.mission_effects.connect(apply_mission_effects)


func teardown() -> void:
	if _world != null and _world.has_signal("mission_effects") \
			and _world.mission_effects.is_connected(apply_mission_effects):
		_world.mission_effects.disconnect(apply_mission_effects)
	if _game_hud != null:
		_game_hud.queue_free()
		_game_hud = null
	_hud_weapon_name = ""
	_hud_objective = ""
	_warned_no_player = false
	NovaStrings.register_table("mission", null)


func get_hud():
	return _game_hud


## ADR 0018 read seam: the last mission text line the HUD displayed.
func hud_objective_line() -> String:
	return _hud_objective


# The in-game HUD over the live runtime: built lazily the first frame a mission has a
# local player (so net spectators, which have none, never get it). Reads the witnessed
# hudpos.def layout from the world's mounted VFS. [orig: HUD_RenderAllOverlays @0x5a8070]
func _ensure_game_hud() -> void:
	if _game_hud != null:
		return
	_game_hud = GameHudScript.new()
	_game_hud.name = "GameHud"
	_game_hud.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var host: Node = _ui_parent if _ui_parent != null else self
	host.add_child(_game_hud)
	_game_hud.set_anchors_preset(Control.PRESET_FULL_RECT)
	var hudpos := NovaHudPos.new()
	var root: NovaResourceRoot = _world.get_resource_root() \
			if _world != null and _world.has_method("get_resource_root") else null
	if root == null:
		push_warning("GameHud: world exposed no resource root; the HUD layout cannot load.")
	elif hudpos.load_from_resource_root(root, "hudpos.def") != OK:
		push_warning("GameHud: hudpos.def did not load: %s" % hudpos.get_last_error())
	_game_hud.set_layout(hudpos, root)
	_load_hud_text_tables(root)


# The string tables the HUD resolves against: the gametext table (weapon "WepDes"
# names) if a menu shell has not already registered it, and the per-mission text
# table (<mission>.bin, falling back to medmssn.bin) for WAC/BMS triggered text.
# [orig: Game_InitSubsystems @0x4a6cd0 (gametext.bin);
#  TextResource_LoadMissionTextBin @0x51ed90 (per mission start + medmssn fallback)]
func _load_hud_text_tables(root: NovaResourceRoot) -> void:
	if root == null:
		return
	if NovaStrings.get_table("gametext") == null:
		var gametext := _load_rtxt(root, "gametext.bin")
		if gametext != null:
			NovaStrings.register_table("gametext", gametext)
	var mission_table: RtxtStringFile = null
	if _world != null and "mission_file" in _world:
		var base := String(_world.mission_file).get_basename()
		if not base.is_empty():
			mission_table = _load_rtxt(root, base + ".bin")
	if mission_table == null:
		mission_table = _load_rtxt(root, "medmssn.bin")
	NovaStrings.register_table("mission", mission_table)


func _load_rtxt(root: NovaResourceRoot, name: String) -> RtxtStringFile:
	var bytes := root.read_file(name)
	if bytes.is_empty():
		return null
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(bytes) != OK:
		return null
	return table


## Rebuild the HUD's per-frame info from the authoritative local player, mirroring the
## original rebuilding its HUD info struct each frame. Call once per frame while the
## player is in-world (the shells gate on their own state).
## [orig: HUD_BuildEntityInfo @0x4b8440]
func tick() -> void:
	if _world == null or not _world.is_loaded():
		return
	if not _world.has_local_player():
		if not _warned_no_player:
			_warned_no_player = true
			push_warning("GameHud: world loaded but has no local player — the in-game HUD will not appear (net spectator, or the mission was not loaded as playable).")
		return
	_ensure_game_hud()
	if _game_hud == null:
		return
	var max_h: int = _world.local_player_max_health()
	var frac := float(_world.local_player_health()) / float(max_h) if max_h > 0 else 0.0
	# Stance from the motor's selected anim-state (crouch/prone is encoded in the clip
	# key). Icon indices: 0=stand, 1=crouch, 2=prone. [orig: HUD_BuildEntityInfo
	# @0x4b860c — entity+300 flags 0x200=crouch->1, 0x100=prone->2]
	var anim_key: String = _world.local_player_anim_key()
	var stance := 0
	if "prone" in anim_key:
		stance = 2
	elif "crouch" in anim_key:
		stance = 1

	# The equipped weapon's HUD slice: re-resolve on weapon change only.
	var weapon: PlayerHudWeaponDef = _world.local_player_hud_weapon_def() \
			if _world.has_method("local_player_hud_weapon_def") else null
	var weapon_name := weapon.weapon_name if weapon != null else ""
	if weapon_name != _hud_weapon_name:
		_hud_weapon_name = weapon_name
		_game_hud.set_weapon(weapon, _resolve_weapon_display_name(weapon_name))

	# Live weapon/view state (the FSM clip/reserve + ADS + fov), mirroring the info
	# struct's ammo fields; an infinite-capacity weapon reads clip -1.
	# [orig: HUD_BuildEntityInfo @0x4b8573..0x4b85fa]
	var clip := -1
	var reserve := -1
	var weapon_active := false
	var wv: PlayerWeaponView = _world.local_player_weapon_view() \
			if _world.has_method("local_player_weapon_view") else null
	if wv != null and wv.active:
		weapon_active = true
		clip = wv.clip if weapon == null or weapon.clipsize != -1 else -1
		reserve = wv.reserve
	var scope_engaged := false
	var scope_fraction := 0.0
	var scope_card := false
	var fov_deg := 80.0
	var lv: PlayerLocalView = _world.local_player_view() \
			if _world.has_method("local_player_view") else null
	if lv != null:
		scope_engaged = lv.scope_engaged
		scope_fraction = lv.scope_fraction
		scope_card = lv.scope_card_active
		fov_deg = lv.fov_h_deg

	_game_hud.update_info({
		"health_fraction": clampf(frac, 0.0, 1.0),
		"stance": stance,
		"team": _world.local_player_team(),
		"objective": "",
		"weapon_active": weapon_active,
		"clip": clip,
		"reserve": reserve,
		"scope_engaged": scope_engaged,
		"scope_fraction": scope_fraction,
		# The SIGHTS card switch [orig: Player_IsEquippedWeaponScoped @0x4dcc80].
		"scope_card": scope_card,
		# The crosshair's witnessed anchor: the aim ray projected through the live
		# camera (screen px; INF = no projection this frame) [orig: HUD_DrawCrosshair
		# @0x592640 centers on the projected aim point].
		"aim_screen": _player_host.aim_screen_point() if _player_host != null else Vector2.INF,
		"fov_deg": fov_deg,
		"ticks": _hud_ticks(),
	})


# The HUD's 62 Hz presentation clock driving the fade/message timers.
# [orig: current_tick @0x24c1968]
func _hud_ticks() -> int:
	return int(Time.get_ticks_msec() * 0.062)


# The weapon's HUD display name: the raw weapon id resolved in the gametext table's
# "WepDes" section; a miss is the empty string (the element then draws nothing).
# [orig: GameText_GetString("WepDes", weapondef+20) @0x593b7f; miss "" @0x51ec00]
func _resolve_weapon_display_name(weapon_name: String) -> String:
	if weapon_name.is_empty():
		return ""
	var t: RtxtStringFile = NovaStrings.get_table("gametext")
	if t != null and t.has_string_in_section("WepDes", weapon_name):
		return t.get_string_in_section("WepDes", weapon_name)
	return ""


# Mission effects feed the HUD's text surfaces (the WAC/mission text the original
# routes to the HUD). Drained effects carry {kind, a..d, str}; the WAC text/consol
# family lands as kind=="text" — the literal form carries the string in "str"
# (a == 0), the id form carries the Triggered-Text id in "a". Public with the
# hud_objective_line() read seam (ADR 0018).
func apply_mission_effects(effects: Array) -> void:
	for e in effects:
		if e is Dictionary and String(e.get("kind", "")) == "text":
			var t := String(e.get("str", ""))
			if not t.is_empty():
				_hud_objective = t
				if _game_hud != null:
					_game_hud.push_message(t)
			elif int(e.get("a", 0)) != 0:
				_show_triggered_text(int(e.get("a", 0)))


# [orig: HUD_DisplayTriggeredText @0x51f190 — the mission table's "Triggered Text"
# section, key ID%03i, read directly (no override-table consult); a miss shows nothing]
func _show_triggered_text(text_id: int) -> void:
	if _game_hud == null:
		return
	var key := "ID%03d" % text_id
	var table: RtxtStringFile = NovaStrings.get_table("mission")
	var text := ""
	if table != null and table.has_string_in_section("Triggered Text", key):
		text = table.get_string_in_section("Triggered Text", key)
	if text.is_empty():
		push_warning("GameHud: mission text %s not found in the mission string table." % key)
		return
	_hud_objective = text
	_game_hud.push_message(text)
