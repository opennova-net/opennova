class_name GameHudPresenter
extends Node

## Owns the in-game HUD (the native HudOverlay over the engine HudFrameCompiler)
## for a live GameWorld in the game shell. Owns the lazy build (hudpos.def layout
## + string tables), the per-frame typed state rebuild from the authoritative
## local player, and the mission text feed — the shell-side mirror of the
## original's per-frame HUD info struct. The shells only say when the player is
## in-world (they gate tick()).
## [orig: HUD_RenderAllOverlays @0x5a8070; HUD_BuildEntityInfo @0x4b8440;
##  gametext Game_InitSubsystems @0x4a6cd0; mission .bin
##  TextResource_LoadMissionTextBin @0x51ed90]

const HudSightsCardScript := preload("res://game/world/hud_sights_card.gd")
const PlayerViewEffectsScript := preload("res://game/world/player_view_effects.gd")
const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")

var _world: GameWorld = null
var _player_presenter = null     # LocalPlayerPresenter (reserved for the weapon-round anchors)
var _ui_parent: Node = null

# The HUD's message ring has 40 physical slots; keep no more pre-HUD messages
# than it can ever present (net spectators may never acquire a local-player HUD).
const MAX_PENDING_HUD_MESSAGES := 40

var _game_hud = null        # HudOverlay, built on the first frame a mission has a local player
var _sights_card = null     # HudSightsCard child of the overlay (per-row blend controls)
var _view_effects = null    # PlayerViewEffects child of the overlay (binocular/NVG stack)
var _warned_no_player := false
var _hud_weapon_name := ""  # equipped-weapon cache (re-resolves WepDes on change)
# Latest player-facing mission text. Presentation rides the message feed; this is
# the public ADR 0018 read seam used by parity tests and future HUD consumers.
var _hud_objective := ""
var _pending_hud_messages: Array[Dictionary] = []
# The end-of-round banner line (the WAC Lose cause). Persists until teardown so the
# MISSION FAILED screen can compose it. [orig: g_banner_text @0x28E3DA0, written by
# GameMsg_SetBannerText @0x5ba200, cleared by the round-start HUD reset @0x5b71b0]
var _endround_banner := ""
# The objectives-panel toggle (the shell's objectives key flips it; retail toggles
# an alpha byte 0<->255). [orig: input action case @0x49b68b — dword_24C18CC ^= 0xFF
# in co-op; the binding row itself is the unported input-binding layer]
var _objectives_visible := false
# The friendly-tags mode, held here so it survives the per-mission HUD rebuild
# like retail's process-lifetime global. Boot default 2 = FULL.
# [orig: g_friendlyTagsMode @0x24C18C4; default @0x4a7fed]
var _friendly_tag_mode := 2

# The HUD color-scheme index, persisted like retail's config token (read at
# boot, written back on cycle). Default 2 = the hudpos hud_textcolor scheme.
# [orig: config token "hud_color_index" @0x5502eb, default 2 @0x54d2a6; applied
# to the live index @0x55152f; cycled 0..5 by input action case 10 @0x49afc7]
const HUD_COLOR_CONFIG_PATH := "user://settings.cfg"
const HUD_COLOR_SECTION := "hud"
const HUD_COLOR_CONFIG_KEY := "hud_color_index"
var _hud_color_index: int = clampi(
		int(ConfigStore.read(HUD_COLOR_CONFIG_PATH, HUD_COLOR_SECTION,
				HUD_COLOR_CONFIG_KEY, 2)), 0, 5)

# The gameplay key bindings the shell routes here via handle_gameplay_key. Each
# action is witnessed; the authored default binding rows ride the unported
# input-binding layer (D-CTRL-3), so the keys themselves are reimpl mappings.
# Objectives = the co-op alpha toggle [orig: @0x49b68b ->
# HUD_DrawWinConditions @0x5be163]; friendly tags KEY_F, N is NVG [orig:
# action 30 @0x49b573]; color-scheme cycle [orig: input case 10 @0x49afc7].
const OBJECTIVES_KEY := KEY_O
const FRIENDLY_TAGS_KEY := KEY_F
const HUD_COLOR_KEY := KEY_H


## Route one gameplay keycode to its HUD action; false = not a HUD key (the
## shell lets it fall through to the other handlers).
func handle_gameplay_key(keycode: int) -> bool:
	match keycode:
		OBJECTIVES_KEY:
			toggle_objectives()
		FRIENDLY_TAGS_KEY:
			cycle_friendly_tags()
		HUD_COLOR_KEY:
			cycle_hud_color()
		_:
			return false
	return true


func setup(world, player_presenter_in, ui_parent: Node) -> void:
	_world = world
	_player_presenter = player_presenter_in
	_ui_parent = ui_parent
	# Connect before any world can tick: PreMission/WAC effects may drain on the
	# first runtime tick, while the local-player HUD is deliberately built only
	# after that tick (the pending queue holds them). The connect persists for the
	# presenter's lifetime — teardown only resets per-mission state.
	if _world != null and not _world.mission_effects.is_connected(apply_mission_effects):
		_world.mission_effects.connect(apply_mission_effects)


## Undo everything a mission built: the HUD node (its card/effects children go
## with it), its caches, the per-mission string table, and the mission-effects
## tap (the built menu-era teardown main_game carried).
func teardown() -> void:
	if _game_hud != null:
		_game_hud.queue_free()
		_game_hud = null
	_sights_card = null
	_view_effects = null
	_hud_weapon_name = ""
	_hud_objective = ""
	_endround_banner = ""
	_objectives_visible = false
	_pending_hud_messages.clear()
	Strings.register_table("mission", null)
	_warned_no_player = false


func get_hud():
	return _game_hud


## The USER crosshair style (Options); applied to a built HUD immediately, else
## picked up from settings on the next build.
func set_crosshair_style(style: int) -> void:
	if _game_hud != null:
		_game_hud.set_crosshair_style(style)


# The in-game HUD over the live runtime: built lazily the first frame a mission has a
# local player (so net spectators, which have none, never get it). Reads the witnessed
# hudpos.def layout from the world's mounted VFS. The SIGHTS card and the
# PlayerViewEffects post stack mount as behind-parent children of the overlay,
# exactly the child-control stack the ported shell HUD carried.
# [orig: HUD_RenderAllOverlays @0x5a8070]
func _ensure_game_hud() -> void:
	if _game_hud != null:
		return
	_game_hud = HudOverlay.new()
	_game_hud.name = "GameHud"
	_game_hud.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var mount: Node = _ui_parent if _ui_parent != null else self
	mount.add_child(_game_hud)
	# anchors AND offsets: an anchors-only preset keeps a fresh Control's
	# zero rect, and a clipping UI parent can then clip every HUD element to
	# nothing.
	_game_hud.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	# Internal children stay out of scene ownership; INTERNAL_MODE_BACK plus
	# show_behind_parent keeps the post-process/masks a stable layer below the
	# card rows and the overlay's own draw list.
	_view_effects = PlayerViewEffectsScript.new()
	_view_effects.name = "PlayerViewEffects"
	_view_effects.show_behind_parent = true
	_view_effects.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_game_hud.add_child(_view_effects, false, Node.INTERNAL_MODE_BACK)
	_view_effects.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_sights_card = HudSightsCardScript.new()
	_sights_card.name = "SightsCard"
	_sights_card.show_behind_parent = true
	_sights_card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_game_hud.add_child(_sights_card)
	_sights_card.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var hudpos := HudPos.new()
	var root: ResourceRoot = _world.get_resource_root() \
			if _world != null else null
	if root == null:
		push_warning("GameHud: world exposed no resource root; the HUD layout cannot load.")
	elif hudpos.load_from_resource_root(root, "hudpos.def") != OK:
		push_warning("GameHud: hudpos.def did not load: %s" % hudpos.get_last_error())
	_game_hud.set_crosshair_style(ResourceDirSettings.get_crosshair_style())
	_game_hud.configure(hudpos, root)
	_view_effects.set_resource_root(root)
	_load_hud_text_tables(root)
	# The objectives-panel header, resolved once against the freshly registered
	# gametext table. [orig: STROVER_MISSIONOBJECTIVES @0x5ba986]
	var t: RtxtStringFile = Strings.get_table("gametext")
	if t != null and t.has_string_in_section("Overlays", "STROVER_MISSIONOBJECTIVES"):
		_game_hud.set_objectives_header(
				t.get_string_in_section("Overlays", "STROVER_MISSIONOBJECTIVES"))
	# The presenter-held friendly-tags mode survives the per-mission rebuild
	# like retail's process-lifetime global [orig: g_friendlyTagsMode @0x24C18C4].
	_game_hud.set_friendly_tag_mode(_friendly_tag_mode)
	_game_hud.set_hud_color_index(_hud_color_index)


# The string tables the HUD resolves against: the current root's gametext table
# (weapon "WepDes" names), and the per-mission text table (<mission>.bin, falling
# back to medmssn.bin) for WAC/BMS triggered text.
# [orig: Game_InitSubsystems @0x4a6cd0 (gametext.bin);
#  TextResource_LoadMissionTextBin @0x51ed90 (per mission start + medmssn fallback)]
func _load_hud_text_tables(root: ResourceRoot) -> void:
	if root == null:
		return
	# Refresh this global registry from the current world's root every build.
	# Otherwise a second runtime or direct-mount test can silently reuse the first root's
	# strings. The gametext table IS gametext.bin [orig: Game_InitSubsystems
	# @0x4a6cd0 — TextResource_LoadFromArchive("gametext.bin") -> g_TextGameText;
	# Game.bin is the SEPARATE menu resource (@0x552510) and carries no WepDes].
	Strings.register_table("gametext", _load_rtxt(root, "gametext.bin"))
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
	Strings.register_table("mission", mission_table)


func _load_rtxt(root: ResourceRoot, name: String) -> RtxtStringFile:
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
var _perf_probe_enabled := false
var _perf_probe_spans: Dictionary = {}

# The shared F3 frame-stats board (null outside the game shell): while its
# Stats tab captures, the tick's phase spans land there as HUD_* slots.
var _frame_stats: FrameStatsBoard = null


func set_frame_stats_board(board: FrameStatsBoard) -> void:
	_frame_stats = board


## Enables the intentionally costly per-phase clock sampling used by the manual
## fire probe. Normal HUD frames leave the span transport untouched and empty.
func set_perf_probe_enabled(enabled: bool) -> void:
	_perf_probe_enabled = enabled
	_perf_probe_spans.clear()


func tick() -> void:
	var probe_enabled := _perf_probe_enabled
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	var timing := probe_enabled or stats_on
	if probe_enabled:
		_perf_probe_spans.clear()
	if _world == null or not _world.is_loaded():
		return
	var sim = _world.get_sim()
	if sim == null or not sim.has_local_player():
		if not _warned_no_player:
			_warned_no_player = true
			push_warning("GameHud: world loaded but has no local player — the in-game HUD will not appear (net spectator, or the mission was not loaded as playable).")
		return
	_ensure_game_hud()
	if _game_hud == null:
		return
	var max_h: int = sim.get_local_player_max_health()
	var frac := float(sim.get_local_player_health()) / float(max_h) if max_h > 0 else 0.0
	# The stance icon from the sim's authoritative body state (0=stand,
	# 1=crouch, 2=prone). [orig: HUD_BuildEntityInfo @0x4b860c — entity+300
	# flags 0x200=crouch->1, 0x100=prone->2]
	var stance: int = sim.get_local_player_stance()

	# The equipped weapon's HUD slice: re-resolve on weapon change only. The
	# overlay takes the record's fields typed; the card takes the authored
	# SIGHTS rows. [orig: HUD_BuildEntityInfo @0x4b8561 weapon-def pointer;
	# textures HUD_LoadAllTextures @0x59e246]
	var weapon: PlayerHudWeaponDef = _world.local_player_hud_weapon_def()
	var weapon_name := weapon.weapon_name if weapon != null else ""
	if weapon_name != _hud_weapon_name:
		_hud_weapon_name = weapon_name
		if weapon != null:
			_game_hud.set_weapon(weapon.weapon_name,
					_resolve_weapon_display_name(weapon_name), weapon.round_type,
					weapon.clipsize, weapon.rounds_per_icon,
					weapon.clipgfx_texture, weapon.clipgfx_offset,
					weapon.rndgfx_texture, weapon.rndgfx_offset, weapon.rndgfx_step)
		else:
			_game_hud.clear_weapon()
		if _sights_card != null:
			_sights_card.set_weapon_sights(weapon.sights if weapon != null else [],
					_world.get_resource_root() if _world != null else null)

	# Live weapon/view state (the FSM clip/reserve + ADS + fov), mirroring the info
	# struct's ammo fields; an infinite-capacity weapon reads clip -1.
	# [orig: HUD_BuildEntityInfo @0x4b8573..0x4b85fa]
	var clip := -1
	var reserve := -1
	var weapon_active := false
	var wv: PlayerWeaponView = _world.local_player_weapon_view()
	if wv != null and wv.active:
		weapon_active = true
		clip = wv.clip if weapon == null or weapon.clipsize != -1 else -1
		# Capacity-1 weapons fold the chambered round into the displayed reserve
		# (hud_math.folded_reserve carries the witness).
		# [orig: HUD_BuildEntityInfo @0x4b85ef — hudInfo+52 += clip when def+88 == 1]
		reserve = HudPos.folded_reserve(clip, wv.reserve,
				weapon.clipsize if weapon != null else -1)
	var probe_t0 := Time.get_ticks_usec() if timing else 0
	var scope_card := false
	var fov_deg := 80.0
	var binoculars_view_active := false
	var binocular_range := 1
	var nvg_visible := false
	var nvg_gain := 0
	var vehicle_attack_context := false
	var lv: PlayerLocalView = _world.local_player_view()
	if lv != null:
		scope_card = lv.scope_card_active
		fov_deg = lv.fov_h_deg
		binoculars_view_active = lv.binoculars_view_active
		nvg_visible = lv.nvg_visible
		nvg_gain = lv.nvg_gain
		vehicle_attack_context = lv.vehicle_attack_context
	if binoculars_view_active and _player_presenter != null:
		binocular_range = clampi(int(_player_presenter.aim_range_units()), 1, 1000)

	var probe_t1 := Time.get_ticks_usec() if timing else 0
	_apply_attach_labels()
	_apply_friendly_tags()
	var probe_t2 := Time.get_ticks_usec() if timing else 0
	var waypoint := _build_waypoint_entry()
	var probe_t3 := Time.get_ticks_usec() if timing else 0
	# The typed per-frame state feed — the shell's mirror of the original
	# rebuilding its 576-byte HUD info struct each frame.
	# [orig: HUD_BuildEntityInfo @0x4b8440]
	_game_hud.set_player_state(_hud_ticks(), clampf(frac, 0.0, 1.0), stance, fov_deg)
	# Weapon-cluster state: clip/reserve as the info struct carried them, heat
	# 0..0xFFFF (only emplaced/vehicle heavy guns author heat_values, so 0 on
	# foot [orig: hudInfo+60 = WeaponSlot_CalcAccumulatedHeat @0x53f780,
	# @0x4b8533]); the exact crosshair dispersion including both live body
	# accumulators — the simulation owns the stance/aimed-shot row (body-state
	# predicates), the HUD owns only projection [orig: @0x592b07..0x592bf5];
	# the sim's promoted aimed-shot verdict gates the reticle [orig: the
	# @0x4de4f7 promoter; Player_CanFireWeapon @0x5cf780], with the modeled
	# vehicle attack context as the witnessed gunner/vehicle keep-up proxy; and
	# the PowerThrow windup driving the charge bar [orig: g_fireChargeStartTick
	# @0xB76800 read by HUD_DrawPowerThrowChargeBar @0x599830].
	var live := wv != null and wv.active
	_game_hud.set_weapon_state(weapon_active and weapon != null, clip, reserve,
			wv.heat if live else 0,
			wv.hud_spread_fp16 if live else 0,
			wv.aimed_shot_available if live else false,
			vehicle_attack_context,
			live and wv.windup_active,
			wv.windup_held_ticks if live else 0)
	# The crosshair's witnessed anchor: Vector2.INF in first person (the overlay
	# pins the design center @0x5928a0), the projected aim in 3P/spectate
	# (@0x592910).
	_game_hud.set_view_state(binoculars_view_active,
			_player_presenter.aim_screen_point() \
					if _player_presenter != null else Vector2.INF)
	if waypoint != null:
		_game_hud.set_waypoint(waypoint.text_name, waypoint.distance_m)
	else:
		_game_hud.clear_waypoint()
	_apply_objectives()
	# The SIGHTS card switch [orig: Player_IsEquippedWeaponScoped @0x4dcc80],
	# suppressed by the binocular view like the ported shell HUD folded it.
	if _sights_card != null:
		_sights_card.set_card_up(scope_card and not binoculars_view_active)
	if _view_effects != null:
		_view_effects.update_info({
			"binoculars_view_active": binoculars_view_active,
			"binocular_range": binocular_range,
			"nvg_visible": nvg_visible,
			"nvg_gain": nvg_gain,
		})
	var probe_t4 := Time.get_ticks_usec() if timing else 0
	# Effects drain synchronously during _world.tick(), before this HUD update.
	# Flush afterward so GameHud.push_message stamps the current 62 Hz tick.
	_flush_pending_hud_messages()
	if timing:
		var probe_t5 := Time.get_ticks_usec()
		if probe_enabled:
			_perf_probe_spans["scalars"] = probe_t1 - probe_t0
			_perf_probe_spans["attach"] = probe_t2 - probe_t1
			_perf_probe_spans["waypoint"] = probe_t3 - probe_t2
			_perf_probe_spans["update_info"] = probe_t4 - probe_t3
			_perf_probe_spans["flush"] = probe_t5 - probe_t4
		if stats_on:
			_frame_stats.add(FrameStatsBoard.HUD_SCALARS, probe_t1 - probe_t0)
			_frame_stats.add(FrameStatsBoard.HUD_ATTACH, probe_t2 - probe_t1)
			_frame_stats.add(FrameStatsBoard.HUD_WAYPOINT, probe_t3 - probe_t2)
			_frame_stats.add(FrameStatsBoard.HUD_INFO, probe_t4 - probe_t3)
			_frame_stats.add(FrameStatsBoard.HUD_FLUSH, probe_t5 - probe_t4)


# The HUD's presentation clock driving the fade/message timers — the engine's
# ms -> tick quantum count (world/tick_accumulator.h ticks_from_ms carries the
# [orig: current_tick @0x24c1968] witness). Adopting the engine's exact 16 ms
# quantum corrects the old 0.062 ticks/ms approximation (62 Hz vs 62.5 Hz).
func _hud_ticks() -> int:
	return Simulation.ticks_from_ms(Time.get_ticks_msec())


# The waypoint label's entry: the sim's current track entry with its display
# name resolved and the 2D ground distance in meters. Null = the label hides
# (no track, ShowWaypoints off, or no current selection) — the compiler treats
# absence as the original's null-current / flag-off gates.
# [orig: HUD_DrawWaypointNameAndDistance @0x5947a0 gates @0x5a7daf (g_showWaypoints
#  + a current present in the list); distance @0x5947e5..0x594836 = 2D fixed sqrt
#  >> 16; name get_waypoint_name @0x594630]
func _build_waypoint_entry() -> WaypointHudEntry:
	if _world == null:
		return null
	var sim := _world.get_sim()
	if sim == null:
		return null
	var wp: Dictionary = sim.get_waypoint_hud_view()
	if not bool(wp.get("show", false)) or int(wp.get("current", -1)) < 0:
		return null
	var pos: Vector3 = wp.get("position", Vector3.ZERO)
	var player: Vector3 = sim.get_local_player_position()
	var entry := WaypointHudEntry.new()
	entry.text_name = _resolve_waypoint_name(int(wp.get("name_id", 0)))
	# Horizontal-only (mission X/Y deltas = the Godot ground plane), truncated
	# to whole meters natively. [orig: @0x594836 sar 16]
	entry.distance_m = HudPos.waypoint_distance_m(
			Vector2(pos.x - player.x, pos.z - player.z))
	return entry


# The waypoint display name. Our SP runtime is the co-op session shape (gametype
# 0x30020), whose `& 0x20000` branch keys STRWPNAME by the RAW authored id — the
# +1 remap belongs to the non-co-op MP gametypes, unported with them. The
# armory/target/flag specials key off MP POI entity types, not SP route markers.
# [orig: get_waypoint_name @0x594630 — index remap @0x594678; mission-table
#  fallback @0x59473d ("STRWPNAME%03i" in WPNames); empty or "null" ->
#  gametext WPNames/STRWPNAMEDEFAULT @0x59477b]
func _resolve_waypoint_name(name_id: int) -> String:
	var key := "STRWPNAME%03d" % name_id
	var mission_table: RtxtStringFile = Strings.get_table("mission")
	var name := ""
	if mission_table != null and mission_table.has_string_in_section("WPNames", key):
		name = mission_table.get_string_in_section("WPNames", key)
	if name.is_empty() or name.nocasecmp_to("null") == 0:
		var gametext: RtxtStringFile = Strings.get_table("gametext")
		if gametext != null and gametext.has_string_in_section("WPNames", "STRWPNAMEDEFAULT"):
			return gametext.get_string_in_section("WPNames", "STRWPNAMEDEFAULT")
		return ""
	return name


# The floating attach labels: the sim's selection (distance/LOS/occupancy/nearest,
# armory-zone mode) projected through the play camera to screen pixels, each with its
# resolved label text, fed to the overlay as parallel typed arrays. Behind-camera
# points drop at projection, mirroring the frustum clip.
# [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 — the projection
#  Math_FixedPointTransformPoint22 + clip_point_to_frustum_and_project @0x5a3655]
func _apply_attach_labels() -> void:
	if _game_hud == null:
		return
	var screens := PackedVector2Array()
	var texts := PackedStringArray()
	var nearest := PackedByteArray()
	var sim = _world.get_sim() if _world != null else null
	if sim != null:
		var labels: Array = sim.get_attach_labels()
		var camera: Camera3D = _game_hud.get_viewport().get_camera_3d() \
				if not labels.is_empty() else null
		if camera != null:
			for raw in labels:
				var l: Dictionary = raw
				var world_pos := MissionObjectPlacer.bms_to_godot_position(
						Vector3(l.get("position", Vector3.ZERO)))
				if camera.is_position_behind(world_pos):
					continue # [orig: clip_point_to_frustum_and_project nonzero = clipped @0x5a3655]
				screens.append(camera.unproject_position(world_pos))
				texts.append(_attach_label_text(int(l.get("seat_type", 0)),
						String(l.get("attach_text_key", ""))))
				nearest.append(1 if bool(l.get("nearest", false)) else 0)
	_game_hud.set_attach_labels(screens, texts, nearest)


# The overhead-anchor addend above the entity's eye offset: 0x4000 = 0.25 u
# [orig: anchor z = z + entity[+116] + 0x4000 @0x5a3a84..0x5a3a98 — +116 is the
# stance-driven eye offset the sim restamps per body tick and feeds per tag;
# docs/interface/hud-re.md D-HUD-20].
const FRIENDLY_TAG_LIFT := 0.25


# The overhead friendly tags (D-HUD-20): the sim's pool-0 gather projected
# through the play camera with its view distance, fed as parallel typed arrays;
# the environment's live fog distance rides along for the compiler's fog cull.
# Behind-camera anchors drop at projection, mirroring the frustum clip.
# [orig: HUD_DrawFriendlyTagsPass @0x5a4480 -> HUD_DrawEntityLabel @0x5a39b0 —
#  distance @0x5a3aba, projection Math_FixedPointTransformPoint22 +
#  clip_point_to_frustum_and_project @0x5a3b47, fog Env_FogDistCurrent
#  @0x5a3b28. The speaking-pulse level feed is the dialog-channel follow-up.]
func _apply_friendly_tags() -> void:
	if _game_hud == null:
		return
	var screens := PackedVector2Array()
	var dists := PackedInt32Array()
	var names := PackedStringArray()
	var ids := PackedInt32Array()
	var ratios := PackedInt32Array()
	var flags := PackedInt32Array()
	var sim = _world.get_sim() if _world != null else null
	if sim != null and _game_hud.get_friendly_tag_mode() != 0:
		var tags: Array = sim.get_friendly_tags()
		var camera: Camera3D = _game_hud.get_viewport().get_camera_3d() \
				if not tags.is_empty() else null
		if camera != null:
			var cam_pos := camera.global_position
			for raw in tags:
				var tag: Dictionary = raw
				var world_pos := MissionObjectPlacer.bms_to_godot_position(
						Vector3(tag.get("position", Vector3.ZERO)))
				world_pos.y += float(tag.get("eye_height", 0.0)) + FRIENDLY_TAG_LIFT
				if camera.is_position_behind(world_pos):
					continue # [orig: the nonzero-clip bail @0x5a3b80]
				screens.append(camera.unproject_position(world_pos))
				dists.append(int(cam_pos.distance_to(world_pos) * 65536.0))
				names.append(String(tag.get("name", "")))
				ids.append(int(tag.get("entity_id", 0)))
				ratios.append(int(tag.get("health_ratio_fp16", 0x10000)))
				flags.append(4 if bool(tag.get("player", false)) else 0)
	var fog_q16 := 0
	var env: MissionEnvironment = _world.get_environment_node() \
			if _world != null else null
	if env != null:
		fog_q16 = int(env.get_fog_distance() * 65536.0)
	_game_hud.set_friendly_tag_env(fog_q16, 0)
	_game_hud.set_friendly_tags(screens, dists, names, ids, ratios, flags)


# The label text per seat type, resolved in the gametext table's Overlays section with
# the witnessed missing-string fallbacks. The Gunner label prefers the weapon's
# attachtextid key: a PRESENT key resolves even to an empty string (the original stores
# the parse-time GameText_GetString result, "" on a miss, and draws it) — only an
# ABSENT key falls to the STROVER_USEGUN default.
# [orig: HUD_InitOverlaySystem @0x5a479c..0x5a481e — STROVER_SIT "!sit" /
#  STROVER_CONTROL "!Control" / STROVER_USEGUN "!UseGun" / STROVER_USEARMORY
#  "!UseArmory"; the USEGUN def-text pick @0x5a350c..0x5a3544; the parse resolve
#  @0x544d87. The STROVER_USEARMORYD "Armory in %d Seconds" delay variant is the MP
#  armory-delay state — deferred with it: docs/interface/hud-re.md (D-HUD-14).]
func _attach_label_text(seat_type: int, attach_text_key: String) -> String:
	var t: RtxtStringFile = Strings.get_table("gametext")
	match seat_type:
		1: # sitex [orig: dword_2723860]
			return _overlays_string(t, "STROVER_SIT", "!sit")
		2, 5: # ctrlx/drvrx share the Control label [orig: dword_2723864 @0x5a34db/0x5a34fb]
			return _overlays_string(t, "STROVER_CONTROL", "!Control")
		3: # UseGun [orig: def+0x3A0 else dword_2723868]
			if attach_text_key.is_empty():
				return _overlays_string(t, "STROVER_USEGUN", "!UseGun")
			if t != null and t.has_string_in_section("Overlays", attach_text_key):
				return t.get_string_in_section("Overlays", attach_text_key)
			return "" # the witnessed empty-label quirk (parse-miss stores "")
		4: # armory [orig: dword_272386C]
			return _overlays_string(t, "STROVER_USEARMORY", "!UseArmory")
	return ""


func _overlays_string(t: RtxtStringFile, key: String, fallback: String) -> String:
	if t != null and t.has_string_in_section("Overlays", key):
		return t.get_string_in_section("Overlays", key)
	return fallback


# The weapon's HUD display name: the raw weapon id resolved in the gametext table's
# "WepDes" section; a miss is the empty string (the element then draws nothing).
# [orig: GameText_GetString("WepDes", weapondef+20) @0x593b7f; miss "" @0x51ec00]
func _resolve_weapon_display_name(weapon_name: String) -> String:
	if weapon_name.is_empty():
		return ""
	var t: RtxtStringFile = Strings.get_table("gametext")
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
		if not (e is Dictionary):
			continue
		var kind := String(e.get("kind", ""))
		if kind == "text":
			var t := String(e.get("str", ""))
			if not t.is_empty():
				_hud_objective = t
				_queue_hud_message(t, 0)
			else:
				var text_id := int(e.get("a", 0))
				if text_id != 0:
					_queue_hud_message("", text_id)
		elif kind == "lose":
			# The WAC Lose banner trio [orig: WacAction_Lose @0x4ed3f0 ->
			# GameMsg_AddChatLineAndRelay @0x5ba170 (the chat-feed line; the KEY rides
			# the wire and each client re-resolves it) + GameMsg_SetBannerText @0x5ba200
			# / GameMsg_SetTeamBannerText @0x5ba1d0 (the persistent banner buffers the
			# MISSION FAILED screen composes, cleared at the next round start
			# @0x5b71b0)]. The effect carries the gametext key; resolve against the
			# 'Misc' section like the original.
			var key := String(e.get("str", ""))
			if not key.is_empty():
				var line := Strings.lookup_display("gametext", "Misc", key)
				_endround_banner = line
				_queue_hud_message(line, 0)
		elif kind == "subgoal_won" or kind == "subgoal_lost":
			# A subgoal resolved: the mission-text announcement rides the chat
			# feed (b = the header text id, c = the round-still-running gate);
			# a LOST subgoal also stamps the persistent banner. [orig: case 14
			# @0x454543 STRWINMSG chat; case 15 @0x454612 STRLOSEMSG chat +
			# GameMsg_SetBannerText @0x454647]
			if int(e.get("c", 0)) != 0:
				var lost := kind == "subgoal_lost"
				var msg_key := ("STRLOSEMSG%03d" if lost else "STRWINMSG%03d") \
						% int(e.get("b", 0))
				var section := "LoseConditions" if lost else "WinConditions"
				var t: RtxtStringFile = Strings.get_table("mission")
				if t != null and t.has_string_in_section(section, msg_key):
					var line := t.get_string_in_section(section, msg_key)
					if not line.is_empty():
						if lost:
							_endround_banner = line
						_queue_hud_message(line, 0)


func hud_objective_line() -> String:
	return _hud_objective


## The stored end-of-round banner (the WAC Lose cause line), for the end screen.
func endround_banner_line() -> String:
	return _endround_banner


## The waypoint label's current entry (null = label hidden) — the ADR 0018
## public read seam for probes and diagnostics, as the ADR 0017 typed record.
func waypoint_hud_entry() -> WaypointHudEntry:
	return _build_waypoint_entry()


## The objectives-panel toggle, flipped by the shell's objectives key.
## [orig: the co-op action toggle @0x49b68b]
func toggle_objectives() -> void:
	_objectives_visible = not _objectives_visible


# The retail toast keys, indexed by the mode they announce.
# [orig: @0x49b596/@0x49b5c1/@0x49b5d0/@0x49b5da]
const FRIENDLY_TAG_TOAST_KEYS: Array[String] = ["STRMISC_FRIENDLYTAGS_OFF",
		"STRMISC_FRIENDLYTAGS_FARBRIEF", "STRMISC_FRIENDLYTAGS_FULL",
		"STRMISC_FRIENDLYTAGS_BRIEF"]


## The friendly-tags mode cycle 0->1->2->3->0 with the retail toast through the
## message feed. [orig: Input_HandleActionBinding case 30 @0x49b573 ->
##  GameText("Misc", STRMISC_FRIENDLYTAGS_*) -> Chat_AddDebugMessage @0x49bc60]
func cycle_friendly_tags() -> void:
	_friendly_tag_mode = (_friendly_tag_mode + 1) % 4
	if _game_hud == null:
		return
	_game_hud.set_friendly_tag_mode(_friendly_tag_mode)
	var t: RtxtStringFile = Strings.get_table("gametext")
	var key := FRIENDLY_TAG_TOAST_KEYS[_friendly_tag_mode]
	if t != null and t.has_string_in_section("Misc", key):
		_game_hud.push_message(t.get_string_in_section("Misc", key))


## The HUD color-scheme cycle 0..5 with wrap, written back to the config like
## retail's token round trip. Deliberately NO toast — the retail case only
## cycles and restamps the color. [orig: input action case 10 @0x49afc7 —
## idx+1, >5 wraps to 0, g_hudActiveColor = table[idx]]
func cycle_hud_color() -> void:
	_hud_color_index = (_hud_color_index + 1) % 6
	ConfigStore.write(HUD_COLOR_CONFIG_PATH, HUD_COLOR_SECTION,
			HUD_COLOR_CONFIG_KEY, _hud_color_index)
	if _game_hud != null:
		_game_hud.set_hud_color_index(_hud_color_index)


# The panel's resolved rows: shown win-condition slots with mission-text lines
# and their completed state, fed typed (an empty pair hides the panel — the
# retail toggle's off state). [orig: HUD_DrawWinConditions @0x5ba940 — rows from
# the header table walk, text = mission WinConditions/STRWINCOND%03i]
func _apply_objectives() -> void:
	if _game_hud == null:
		return
	var texts := PackedStringArray()
	var done := PackedByteArray()
	var sim = _world.get_sim() if _world != null else null
	if _objectives_visible and sim != null:
		var table: RtxtStringFile = Strings.get_table("mission")
		for raw in sim.get_objectives_view():
			var row: Dictionary = raw
			if not bool(row.get("shown", false)):
				continue
			var key := "STRWINCOND%03d" % int(row.get("text_id", 0))
			var text := ""
			if table != null and table.has_string_in_section("WinConditions", key):
				text = table.get_string_in_section("WinConditions", key)
			texts.append(text)
			done.append(1 if bool(row.get("done", false)) else 0)
	_game_hud.set_objectives(texts, done)


## Number of player-facing messages waiting for the lazy HUD to mount.
## This is the ADR 0018 read seam for presenter tests and diagnostics.
func pending_hud_message_count() -> int:
	return _pending_hud_messages.size()


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
	var table: RtxtStringFile = Strings.get_table("mission")
	var text := ""
	if table != null and table.has_string_in_section("Triggered Text", key):
		text = table.get_string_in_section("Triggered Text", key)
	if text.is_empty():
		push_warning("GameHud: mission text %s not found in the mission string table." % key)
		return
	_hud_objective = text
	_game_hud.push_message(text)
