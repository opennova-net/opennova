class_name NovaGameHudHost
extends Node

## Hosts the in-game HUD (GameHud) over a live GameWorld for BOTH shells — the game
## (main_game) and ONED play-in-editor (mission_play_controller) — one HUD code path
## (editor-runtime parity). Owns the lazy build (hudpos.def layout + string tables),
## the per-frame info rebuild from the authoritative local player, and the mission
## text feed. The shells only say when the player is in-world (they gate tick()).
## [orig: HUD_RenderAllOverlays @0x5a8070; HUD_BuildEntityInfo @0x4b8440;
##  gametext Game_InitSubsystems @0x4a6cd0; mission .bin
##  TextResource_LoadMissionTextBin @0x51ed90]

const GameHudScript := preload("res://engine/world/game_hud.gd")

var _world = null           # GameWorld
var _player_host = null     # LocalPlayerHost (reserved for the weapon-round anchors)
var _ui_parent: Node = null

# The HUD's message ring has 40 physical slots; keep no more pre-HUD messages
# than it can ever present (net spectators may never acquire a local-player HUD).
const MAX_PENDING_HUD_MESSAGES := 40

var _game_hud = null        # GameHud, built on the first frame a mission has a local player
var _warned_no_player := false
var _hud_weapon_name := ""  # equipped-weapon cache (re-resolves WepDes on change)
# Latest player-facing mission text. Presentation rides the message feed; this is
# the public ADR 0018 read seam used by parity tests and future HUD consumers.
var _hud_objective := ""
var _pending_hud_messages: Array[Dictionary] = []
var _crosshair_style := 0


func setup(world, player_host, ui_parent: Node) -> void:
	_world = world
	_player_host = player_host
	_ui_parent = ui_parent
	# Connect before any world can tick: PreMission/WAC effects may drain on the
	# first runtime tick, while the local-player HUD is deliberately built only
	# after that tick (the pending queue holds them). The connect persists for the
	# host's lifetime — teardown only resets per-mission state.
	if _world != null and _world.has_signal("mission_effects") 			and not _world.mission_effects.is_connected(apply_mission_effects):
		_world.mission_effects.connect(apply_mission_effects)


## Undo everything a mission built: the HUD node, its caches, the per-mission string
## table, and the mission-effects tap (the built menu-era teardown main_game carried).
func teardown() -> void:
	if _game_hud != null:
		_game_hud.queue_free()
		_game_hud = null
	_hud_weapon_name = ""
	_hud_objective = ""
	_pending_hud_messages.clear()
	NovaStrings.register_table("mission", null)
	_warned_no_player = false


func get_hud():
	return _game_hud


## The host supplies the user's crosshair style. Keep it across mission teardown
## so both game and ONED hosts can inject policy without engine-layer persistence.
func set_crosshair_style(style: int) -> void:
	_crosshair_style = clampi(style, 0, 24)
	if _game_hud != null:
		_game_hud.set_crosshair_style(_crosshair_style)


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
	# anchors AND offsets: an anchors-only preset keeps a fresh Control's
	# zero rect, and a clipping parent (ONED's GameplayOverlay) then clips
	# every HUD element to nothing.
	_game_hud.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var hudpos := NovaHudPos.new()
	var root: NovaResourceRoot = _world.get_resource_root() \
			if _world != null and _world.has_method("get_resource_root") else null
	if root == null:
		push_warning("GameHud: world exposed no resource root; the HUD layout cannot load.")
	elif hudpos.load_from_resource_root(root, "hudpos.def") != OK:
		push_warning("GameHud: hudpos.def did not load: %s" % hudpos.get_last_error())
	_game_hud.set_crosshair_style(_crosshair_style)
	_game_hud.set_layout(hudpos, root)
	_load_hud_text_tables(root)


# The string tables the HUD resolves against: the current root's gametext table
# (weapon "WepDes" names), and the per-mission text table (<mission>.bin, falling
# back to medmssn.bin) for WAC/BMS triggered text.
# [orig: Game_InitSubsystems @0x4a6cd0 (gametext.bin);
#  TextResource_LoadMissionTextBin @0x51ed90 (per mission start + medmssn fallback)]
func _load_hud_text_tables(root: NovaResourceRoot) -> void:
	if root == null:
		return
	# Refresh this global registry from the current world's root every build.
	# Otherwise a second ONED play session can silently reuse the first root's
	# strings. The gametext table IS gametext.bin [orig: Game_InitSubsystems
	# @0x4a6cd0 — TextResource_LoadFromArchive("gametext.bin") -> g_TextGameText;
	# Game.bin is the SEPARATE menu resource (@0x552510) and carries no WepDes].
	NovaStrings.register_table("gametext", _load_rtxt(root, "gametext.bin"))
	# The medmssn fallback fires only when the mission .bin does not EXIST — a
	# present-but-unparseable file loads to nothing with no fallback.
	# [orig: TextResource_LoadMissionTextBin @0x51ede3 — FileSystem_FileExists picks
	# the filename; the load result is stored either way]
	var mission_table: RtxtStringFile = null
	var mission_bin := ""
	if _world != null:
		var base: String = String(_world.get_loaded_mission_file()).get_basename()
		if not base.is_empty():
			mission_bin = base + ".bin"
	if not mission_bin.is_empty() and root.has_file(mission_bin):
		mission_table = _load_rtxt(root, mission_bin)
	else:
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
		# Capacity-1 weapons fold the chambered round into the displayed reserve
		# (the ammo text and the round icons both read the folded count).
		# [orig: HUD_BuildEntityInfo @0x4b85ef — hudInfo+52 += clip when def+88 == 1]
		if weapon != null and weapon.clipsize == 1 and clip >= 0 and reserve >= 0:
			reserve += clip
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
		# The ease progress: the crosshair yields only at the SETTLED sight view
		# (fraction 1) [orig: the @0x4de4f7 promoter; Player_CanFireWeapon @0x5cf780].
		"scope_fraction": scope_fraction,
		# The SIGHTS card switch [orig: Player_IsEquippedWeaponScoped @0x4dcc80].
		"scope_card": scope_card,
		# The crosshair's witnessed anchor: Vector2.INF in first person (the HUD pins
		# the design center @0x5928a0), the projected aim in 3P/spectate (@0x592910).
		"aim_screen": _player_host.aim_screen_point() \
				if _player_host != null and _player_host.has_method("aim_screen_point") \
				else Vector2.INF,
		"fov_deg": fov_deg,
		"ticks": _hud_ticks(),
	})
	# Effects drain synchronously during _world.tick(), before this HUD update.
	# Flush afterward so GameHud.push_message stamps the current 62 Hz tick.
	_flush_pending_hud_messages()


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


# Mission effects feed the HUD's text surfaces. Drained effects carry
# {kind, a..d, str}: WAC text/ptext carries a literal in `str`, while BMS
# OutputText carries a nonzero Triggered-Text id in `a`. consol/pconsol uses the
# distinct `debug_text` kind and remains off the player-facing feed. Queue both
# forms because PreMission effects can arrive before the lazy HUD and its mission
# table exist. Public with hud_objective_line() as the ADR 0018 read seam.
func apply_mission_effects(effects: Array) -> void:
	for e in effects:
		if e is Dictionary and String(e.get("kind", "")) == "text":
			var t := String(e.get("str", ""))
			if not t.is_empty():
				_hud_objective = t
				_queue_hud_message(t, 0)
			else:
				var text_id := int(e.get("a", 0))
				if text_id != 0:
					_queue_hud_message("", text_id)


func hud_objective_line() -> String:
	return _hud_objective


func _queue_hud_message(text: String, text_id: int) -> void:
	_pending_hud_messages.append({"text": text, "text_id": text_id})
	while _pending_hud_messages.size() > MAX_PENDING_HUD_MESSAGES:
		_pending_hud_messages.pop_front()


func _flush_pending_hud_messages() -> void:
	if _game_hud == null:
		return
	for pending in _pending_hud_messages:
		var text := String(pending.get("text", ""))
		if not text.is_empty():
			_game_hud.push_message(text)
		else:
			_show_triggered_text(int(pending.get("text_id", 0)))
	_pending_hud_messages.clear()


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
