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
const HudScopeCircleMaskScript := preload("res://game/world/hud_scope_circle_mask.gd")
const PlayerViewEffectsScript := preload("res://game/world/player_view_effects.gd")

const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")
const ScoreboardPresenterScript := preload("res://game/world/scoreboard_presenter.gd")
const VehiclePanelPresenterScript := preload("res://game/world/vehicle_panel_presenter.gd")
const MessageLogPresenterScript := preload("res://game/world/message_log_presenter.gd")
const EndRoundStatisticsPresenterScript := preload("res://game/world/end_round_statistics_presenter.gd")
const HudTextTables := preload("res://game/world/hud_text_tables.gd")
const LfpPanelPresenterScript := preload("res://game/world/lfp_panel_presenter.gd")

var _world: GameWorld = null
var _player_presenter: LocalPlayerPresenter = null  # the aim point + the frame's projection
var _ui_parent: Node = null
# PlayerOptions seeds this before the lazy native HUD exists; it survives
# teardown so the next mission build uses the same process-lifetime choice.
var _crosshair_style := HudOverlay.MIN_CROSSHAIR_STYLE
var _crosshair_color: int = PlayerOptions.DEFAULT_CROSSHAIR_COLOR
var _aspect_mode := -1
var _crosshair_spread: bool = PlayerOptions.DEFAULT_CROSSHAIR_SPREAD

# The HUD's message ring has 40 physical slots; keep no more pre-HUD messages
# than it can ever present (net spectators may never acquire a local-player HUD).
const MAX_PENDING_HUD_MESSAGES := 40
# The script chat lines post into the CHAT ring with color -1 (raw white): WAC
# text/ptext/text# through Chat_AddSystemMessage, the WAC lose line and the BMS
# subgoal won/lost lines through GameMsg_AddChatLineAndRelay, all into
# Chat_AddMessageChannel1 (the witnesses sit with their engine producers:
# wac/remote_command.cpp, wac/vm.cpp, hud/hud_frame.h push_chat_line).
const SCRIPT_CHAT_ARGB := -1

# HudOverlay, built on the first frame a mission has a local player
# (ensure_game_hud(); the GUT files build the real one over a staged root).
var _game_hud: HudOverlay = null
var _scoreboard := ScoreboardPresenterScript.new()  # the Tab player list lane
var _vehicle_panel := VehiclePanelPresenterScript.new()  # the mounted-vehicle panel lane
var _message_log := MessageLogPresenterScript.new()  # the Recent Messages (J) lane + chat drain
var _end_round_stats := EndRoundStatisticsPresenterScript.new()  # the SP Show Score (F5) panel lane
var _lfp_panel := LfpPanelPresenterScript.new()  # the AAS zone status panel lane
var _hud_pos: HudPos = null  # the loaded hudpos.def (VEHICLE_HUD blocks for the panel lane)
var _inset_scope: HudInsetScope = null
var _sights_card: HudSightsCard = null # child of the overlay (per-row blend controls)
var _scope_circle_mask: HudScopeCircleMask = null # child of the overlay (the scoped annulus)
var _view_effects: PlayerViewEffects = null # child of the overlay (binocular/NVG stack)
var _warned_no_player := false
var _hud_weapon_name := ""  # equipped-weapon cache (re-resolves WepDes on change)
# Latest player-facing mission text. Presentation rides the message feed; this is
# the public ADR 0018 read seam used by parity tests and future HUD consumers.
var _hud_objective := ""
var _pending_hud_messages: Array[PendingHudMessage] = []


## One queued HUD message: literal text, or a mission-table triggered-text id
## when the text is empty. A `chat` line posts into the CHAT ring; every other
## message posts into the SYSTEM ring.
class PendingHudMessage:
	extends RefCounted
	var text: String
	var text_id: int
	var chat: bool

	func _init(p_text: String, p_text_id: int, p_chat := false) -> void:
		text = p_text
		text_id = p_text_id
		chat = p_chat
# The end-of-round banner line (the WAC Lose cause). Persists until teardown so the
# MISSION FAILED screen can compose it. [orig: g_banner_text @0x28E3DA0, written by
# GameMsg_SetBannerText @0x5ba200, cleared by the round-start HUD reset @0x5b71b0]
var _endround_banner := ""
# The HUD color-scheme index is persisted like retail's config token (read at
# boot, written back on cycle); the cfg store is this presenter's device work.
const HUD_COLOR_CONFIG_PATH := "user://settings.cfg"
const HUD_COLOR_SECTION := "hud"
const HUD_COLOR_CONFIG_KEY := "hud_color_index"
var _score_fanfare := ScoreFanfarePresenter.new()  # the S2C 0x81 hit-confirm lane
# The HUD declutter level is TWO states in retail (hud-re.md, "HUD declutter"):
# the persisted config value `hud_detail` (the game.cfg token, read at boot and
# written only by the config round trip) and the LIVE layer level the huddetail
# cycle and the death screen write. Every mission start re-seeds the live level
# from the config value, so a death's forced blank never outlives its mission.
const HUD_DETAIL_CONFIG_KEY := "hud_detail"
# Stored verbatim: retail atol()s the token and applies it raw, so an
# out-of-range level blanks every gated element until the huddetail cycle
# wraps it (docs/interface/hud-re.md, the declutter section).
var _hud_detail_config: int = int(ConfigStore.read(HUD_COLOR_CONFIG_PATH, HUD_COLOR_SECTION,
		HUD_DETAIL_CONFIG_KEY, HudOverlay.hud_detail_level_default()))
# The key-driven HUD toggles and cycles (HudToggles over the engine's
# hud/hud_toggles.h): the color index, the LIVE declutter level, the showhud
# flags, the friendly-tags mode, the objectives panel and the three overlay
# windows with their edge latches -- process-lifetime like retail's globals, so
# they survive the per-mission HUD rebuild. Seeded from the persisted tokens
# at boot; this presenter samples the keys and applies the device side effects
# the poll's events name.
var _toggles: HudToggles = _seed_toggles(
		int(ConfigStore.read(HUD_COLOR_CONFIG_PATH, HUD_COLOR_SECTION,
				HUD_COLOR_CONFIG_KEY, HudOverlay.hud_color_index_default())),
		_hud_detail_config)
# Render-comparison declutter is a reversible runtime transaction over the
# live level; it saves and restores the exact level around the capture.
var _hud_hidden_capture_active := false
var _hud_hidden_saved_detail_level := 0
# The SIGHTS card's `slide` multiplier for the equipped weapon at its default
# zero (PlayerHudWeaponDef.sight_slide_multiplier, the engine evaluator over
# the def's scope_max_zero table); re-resolved on weapon change.
var _sight_slide_multiplier := 0
# Whether the map grid origin (the type-2043 marker) has been resolved onto
# the HUD. A joiner's origin entity decodes from the world stream AFTER the
# HUD builds, so tick() keeps querying until it appears.
var _map_grid_origin_present := false
# Whether the static building/zone footprint feed reached the HUD (baked
# once per mission; re-queried until non-empty for joiners whose statics
# decode after the HUD builds). The retry is throttled to ~1 Hz: the query
# walks the whole entity registry and slices collision models, so an empty
# feed (a mission with no footprint statics) must not re-run it every frame.
var _map_footprints_fed := false
var _map_footprints_next_query_ticks := 0
var _map_grid_origin_next_query_ticks := 0
const MAP_FOOTPRINT_QUERY_INTERVAL_TICKS := 62

# The gameplay key bindings the shell routes here via handle_gameplay_key. Each
# action is witnessed; the authored default binding rows ride the unported
# input-binding layer (D-CTRL-3), so the keys themselves are reimpl mappings.
# Objectives = the co-op alpha toggle [orig: @0x49b68b ->
# HUD_DrawWinConditions @0x5be163], polled from its catalog row `Goals` (row
# 55, dispatch 31, default G) in tick() like the other HUD rows; friendly
# tags KEY_F, N is NVG [orig: action 30 @0x49b573]. Retail has NO
# HUD-visibility key: H is only the secondary `pause` binding (SP-only), and
# the boot /NOHUD switch is the sole whole-overlay master [orig: catalog row
# 70 vk2 0x48; case 25 @0x49b520; /NOHUD gates dword_840B18 & 2 @0x4a7a09 —
# a DIFFERENT global from the declutter level]. The
# huddetail/hudcolor/showhud/dotsize cycles ride their polled binding rows in
# tick() instead of shell keys.
const FRIENDLY_TAGS_KEY := KEY_F


## Route one gameplay keycode to its HUD action; false = not a HUD key (the
## shell lets it fall through to the other handlers).
func handle_gameplay_key(keycode: int) -> bool:
	match keycode:
		FRIENDLY_TAGS_KEY:
			cycle_friendly_tags()
		_:
			return false
	return true


func setup(world: GameWorld, player_presenter_in: LocalPlayerPresenter, ui_parent: Node) -> void:
	_world = world
	_player_presenter = player_presenter_in
	_ui_parent = ui_parent
	# Connect before any world can tick: PreMission/WAC effects may drain on the
	# first runtime tick, while the local-player HUD is deliberately built only
	# after that tick (the pending queue holds them). The connect persists for the
	# presenter's lifetime — teardown only resets per-mission state.
	if _world != null and not _world.mission_effects.is_connected(apply_mission_effects):
		_world.mission_effects.connect(apply_mission_effects)
	if _world != null and not _world.minimap_water_changed.is_connected(
			_on_minimap_water_changed):
		_world.minimap_water_changed.connect(_on_minimap_water_changed)


## Undo everything a mission built: the HUD node (its card/effects children go
## with it), its caches, the per-mission string table, and the mission-effects
## tap (the built menu-era teardown main_game carried).
func teardown() -> void:
	finish_hud_hidden_capture()
	if _game_hud != null:
		_game_hud.queue_free()
		_game_hud = null
	_inset_scope = null
	_sync_second_scene_camera()
	_sights_card = null
	_scope_circle_mask = null
	_view_effects = null
	_hud_weapon_name = ""
	_hud_objective = ""
	_endround_banner = ""
	_toggles.reset_mission()
	_scoreboard.reset()
	_vehicle_panel.reset()
	_message_log.reset()
	_end_round_stats.reset()
	_lfp_panel.reset()
	_hud_pos = null
	_pending_hud_messages.clear()
	Strings.register_table(Strings.TABLE_MISSION, null)
	_warned_no_player = false


## The layer every HUD element hangs under (hiding it hides the whole HUD,
## siblings of the GameHud control included; the perf probe's canvas A/B).
func get_ui_parent() -> Node:
	return _ui_parent


func _on_minimap_water_changed(mask: ImageTexture) -> void:
	if _game_hud == null:
		return
	_game_hud.set_minimap_terrain(
			_world.get_terrain_data() if _world != null else null, mask)


## Hand the world's particle renderer the weapon Inset pass's camera, or null
## while that pass is not rendering (scope down, HUD torn down): the original
## renders the aperture through its one scene routine, particle passes
## included, so the second view needs the world's particles compiled for its
## own eye. A world that unloaded has no effect world left to tell.
func _sync_second_scene_camera() -> void:
	var effects: EffectWorld = _world.get_effect_world() if _world != null else null
	if effects == null:
		return
	effects.set_second_scene_camera(
			_inset_scope.get_active_render_camera() if _inset_scope != null else null)


## The USER crosshair options; cache each even before the lazy HUD exists,
## then apply it immediately to an existing HUD.
func set_aspect_mode(mode: int) -> void:
	_aspect_mode = mode
	if _game_hud != null:
		_game_hud.set_aspect_mode(mode)


func set_crosshair_style(style: int) -> void:
	_crosshair_style = clampi(style, HudOverlay.MIN_CROSSHAIR_STYLE,
			HudOverlay.MAX_CROSSHAIR_STYLE)
	if _game_hud != null:
		_game_hud.set_crosshair_style(_crosshair_style)


func crosshair_style() -> int:
	return _crosshair_style


func set_crosshair_color(rgb: int) -> void:
	_crosshair_color = rgb & PlayerOptions.CROSSHAIR_COLOR_MASK
	if _game_hud != null:
		_game_hud.set_crosshair_color(_crosshair_color)


func set_crosshair_spread_enabled(enabled: bool) -> void:
	_crosshair_spread = enabled
	if _game_hud != null:
		_game_hud.set_crosshair_spread_enabled(_crosshair_spread)


# The in-game HUD over the live runtime: built lazily the first frame a mission has a
# local player (so net spectators, which have none, never get it). Reads the witnessed
# hudpos.def layout from the world's mounted VFS. The SIGHTS card and the
# PlayerViewEffects post stack mount as behind-parent children of the overlay,
# exactly the child-control stack the ported shell HUD carried.
# [orig: HUD_RenderAllOverlays @0x5a8070]
## Build the overlay if the mission has none yet (update() does it on the first
## frame with a local player; the GUT files build it over a staged root).
func ensure_game_hud() -> void:
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
	_view_effects.set_environment(
			_world.get_environment_node() if _world != null else null)
	_inset_scope = HudInsetScope.new()
	_inset_scope.name = "InsetScope"
	_inset_scope.show_behind_parent = true
	_inset_scope.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_inset_scope.visible = false
	_game_hud.add_child(_inset_scope)
	_inset_scope.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_sights_card = HudSightsCardScript.new()
	_sights_card.name = "SightsCard"
	_sights_card.show_behind_parent = true
	_sights_card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_game_hud.add_child(_sights_card)
	_sights_card.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	# The scoped-view circle mask sits AFTER the card in the overlay's child
	# order, so the annulus covers the card's own corners -- retail submits the
	# SIGHTS card first and the mask straight after it on the Scoped arm.
	_scope_circle_mask = HudScopeCircleMaskScript.new()
	_scope_circle_mask.name = "ScopeCircleMask"
	_scope_circle_mask.show_behind_parent = true
	_scope_circle_mask.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_scope_circle_mask.visible = false
	_game_hud.add_child(_scope_circle_mask)
	_scope_circle_mask.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_push_sight_state()
	var hudpos := HudPos.new()
	var root: ResourceRoot = _world.get_resource_root() \
			if _world != null else null
	if root == null:
		push_warning("GameHud: world exposed no resource root; the HUD layout cannot load.")
	elif hudpos.load_from_resource_root(root, "hudpos.def") != OK:
		push_warning("GameHud: hudpos.def did not load: %s" % hudpos.get_last_error())
	_game_hud.set_crosshair_style(_crosshair_style)
	_game_hud.set_crosshair_color(_crosshair_color)
	_game_hud.set_crosshair_spread_enabled(_crosshair_spread)
	_game_hud.set_aspect_mode(_aspect_mode)
	_game_hud.configure(hudpos, root)
	_hud_pos = hudpos
	# TerrainData owns the TRN 16x16 sector routing table and the colormap
	# texture; the native overlay copies only the portable routing scalars.
	# The raw colormap remains the sharp base; depthspin supplies water only.
	_game_hud.set_minimap_terrain(
			_world.get_terrain_data() if _world != null else null,
			_world.get_minimap_water_mask() if _world != null else null)
	# The grid-label origin marker (mission type-2043 entity), resolved once
	# per world. [orig: HUD_InitOverlaySystem @0x5a4999 pool scan]
	var sim_for_origin := _world.get_sim() if _world != null else null
	_map_footprints_fed = false
	_map_footprints_next_query_ticks = 0
	_map_grid_origin_next_query_ticks = 0
	if sim_for_origin != null:
		var origin := sim_for_origin.get_hud_map_grid_origin()
		var origin_pos := origin.position
		_map_grid_origin_present = origin.present
		_game_hud.set_minimap_grid_origin(
				Vector2(origin_pos.x, -origin_pos.z), _map_grid_origin_present)
	else:
		_map_grid_origin_present = false
		_game_hud.set_minimap_grid_origin(Vector2.ZERO, false)
	_view_effects.set_resource_root(root)
	HudTextTables.register(root, _world)
	# The objectives-panel header, resolved once against the freshly registered
	# gametext table. [orig: STROVER_MISSIONOBJECTIVES @0x5ba986]
	var t: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	if t != null and t.has_string_in_section(Strings.SECTION_OVERLAYS, "STROVER_MISSIONOBJECTIVES"):
		_game_hud.set_objectives_header(
				t.get_string_in_section(Strings.SECTION_OVERLAYS, "STROVER_MISSIONOBJECTIVES"))
	# The presenter-held friendly-tags mode survives the per-mission rebuild
	# like retail's process-lifetime global [orig: g_friendlyTagsMode @0x24C18C4].
	_game_hud.set_friendly_tag_mode(_toggles.get_friendly_tag_mode())
	_game_hud.set_hud_color_index(_toggles.get_hud_color_index())
	# The HUD build stamps the LIVE declutter level, mirroring the round-init
	# HUD reset re-applying the layer global. [orig: the re-apply
	# @0x59DD75 from Game_InitNewRound / HUD_InitOverlaySystem]
	_game_hud.set_hud_detail_level(_toggles.get_hud_detail_level())
	_push_showhud_flags()


# The shared F3 frame-stats board (null outside the game shell): while its
# Stats tab captures, the tick's phase spans land there as HUD_* slots.
var _frame_stats: FrameStats = null
var _hud_draw_timing_armed := false


func set_frame_stats(board: FrameStats) -> void:
	_frame_stats = board


## Rebuild the HUD's per-frame info from the authoritative local player, mirroring the
## original rebuilding its HUD info struct each frame. Call once per frame while the
## player is in-world (the shells gate on their own state).
## [orig: HUD_BuildEntityInfo @0x4b8440]
func tick(gameplay_input_active: bool = false) -> void:
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	if _world == null or not _world.is_loaded():
		return
	var sim: Simulation = _world.get_sim()
	if sim == null or not sim.has_local_player():
		if not _warned_no_player:
			_warned_no_player = true
			push_warning("GameHud: world loaded but has no local player — the in-game HUD will not appear (net spectator, or the mission was not loaded as playable).")
		return
	ensure_game_hud()
	if _game_hud == null:
		return
	if stats_on:
		# HudOverlay._draw runs in Godot's deferred flush after this tick; the
		# previous frame's compile + canvas emit land here. Timing arms only
		# while the board captures (the setter is edge-gated natively).
		_game_hud.set_draw_timing_enabled(true)
		var draw_us: PackedInt64Array = _game_hud.consume_draw_timing_us()
		_frame_stats.add(FrameStats.HUD_DRAW_COMPILE, int(draw_us[0]))
		_frame_stats.add(FrameStats.HUD_DRAW_EMIT, int(draw_us[1]))
	elif _hud_draw_timing_armed:
		_game_hud.set_draw_timing_enabled(false)
	_hud_draw_timing_armed = stats_on
	var max_h: int = sim.get_local_player_max_health()
	var frac := float(sim.get_local_player_health()) / float(max_h) if max_h > 0 else 0.0
	# The view frame supplies the HUD stance, including seat/weapon overrides.
	# [orig: HUD_BuildEntityInfo @0x4b860c..0x4b8786]
	var stance := 0

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
			var sights: Array[WeaponSightRow] = weapon.sights if weapon != null else []
			_sight_slide_multiplier = weapon.sight_slide_multiplier if weapon != null else 0
			_push_sight_state()
			_sights_card.set_weapon_sights(sights,
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
	var probe_t0 := Time.get_ticks_usec() if stats_on else 0
	var scope_card := false
	var fov_deg := 80.0
	var binoculars_view_active := false
	var binocular_range := 1
	var nvg_visible := false
	var nvg_gain := 0
	var vehicle_attack_context := false
	var keep_crosshair_while_aimed := false
	# The three fullscreen damage-feedback quads plus the HUD-overlay early
	# return that rides the white one. The engine reduces the raw words to these
	# draw values (engine/runtime/world/player_view.h carries the witnesses).
	var flash_white := 0
	var flash_red := 0
	var flash_revive := 0
	var flash_revive_channel := 255
	var hud_overlays_suppressed := false
	# The live HUD consumes the frame the player presenter composed and
	# stamped this display frame. A standalone HUD without a camera presenter
	# observes the last composed view; neither read composes a camera.
	# docs/world/tank-parity-re.md (D-HUD-29).
	var lv: PlayerLocalView = _player_presenter.presented_view() \
			if _player_presenter != null else _world.local_player_view()
	if lv != null:
		scope_card = lv.scope_card_active
		_sight_slide_multiplier = lv.sight_slide_multiplier
		binocular_range = lv.aim_range_units
		fov_deg = lv.fov_h_deg
		binoculars_view_active = lv.binoculars_view_active
		nvg_visible = lv.nvg_visible
		nvg_gain = lv.nvg_gain
		vehicle_attack_context = lv.vehicle_attack_context
		stance = lv.hud_stance
		keep_crosshair_while_aimed = lv.hud_keep_crosshair_while_aimed
		flash_white = lv.screen_flash_white_alpha
		flash_red = lv.screen_flash_red_alpha
		flash_revive = lv.screen_flash_revive
		flash_revive_channel = lv.screen_flash_revive_channel
		hud_overlays_suppressed = lv.hud_overlays_suppressed

	var probe_t1 := Time.get_ticks_usec() if stats_on else 0
	_apply_attach_labels()
	_apply_friendly_tags()
	var probe_t2 := Time.get_ticks_usec() if stats_on else 0
	var waypoint := _build_waypoint_entry()
	var probe_t3 := Time.get_ticks_usec() if stats_on else 0
	# The typed per-frame state feed — the shell's mirror of the original
	# rebuilding its 576-byte HUD info struct each frame.
	# [orig: HUD_BuildEntityInfo @0x4b8440]
	_game_hud.set_player_state(_hud_ticks(), clampf(frac, 0.0, 1.0), stance, fov_deg)
	_game_hud.set_player_context(lv)
	var player_pos: Vector3 = sim.get_local_player_position()
	_game_hud.set_minimap_state(Vector2(player_pos.x, -player_pos.z),
			player_pos.y, sim.get_local_player_heading_bam(),
			sim.get_hud_radar_zoom_q16(), sim.get_hud_big_zoom_q16(),
			sim.get_hud_map_mode(), sim.get_hud_map_flip_180(),
			sim.get_hud_minimap_snapshot())
	# A joiner's type-2043 origin entity decodes from the world stream after
	# the HUD builds — keep querying until it appears (the host resolves the
	# origin at promotion, so this latches immediately there). Throttled to
	# ~1 Hz like the footprint retry: the query scans the decoded entity
	# rows, and a mission with no origin marker must not pay that scan every
	# display frame forever.
	if not _map_grid_origin_present:
		var origin_ticks := _hud_ticks()
		if origin_ticks >= _map_grid_origin_next_query_ticks:
			_map_grid_origin_next_query_ticks = \
					origin_ticks + MAP_FOOTPRINT_QUERY_INTERVAL_TICKS
			var origin := sim.get_hud_map_grid_origin()
			if origin.present:
				var origin_pos := origin.position
				_map_grid_origin_present = true
				_game_hud.set_minimap_grid_origin(
						Vector2(origin_pos.x, -origin_pos.z), true)
	# The static footprint polygons bake once per mission; latch on the
	# first non-empty feed (joiner statics can decode after the HUD builds),
	# retrying at most once a second.
	if not _map_footprints_fed:
		var hud_ticks := _hud_ticks()
		if hud_ticks >= _map_footprints_next_query_ticks:
			_map_footprints_next_query_ticks = \
					hud_ticks + MAP_FOOTPRINT_QUERY_INTERVAL_TICKS
			var footprints: PackedInt32Array = sim.get_hud_minimap_footprints()
			if footprints.size() >= 2 and footprints[1] > 0:
				_map_footprints_fed = true
				_game_hud.set_minimap_footprints(footprints)
	# The HUD binding rows. hudcolor (row 76) defaults to Ctrl+F6 beside
	# huddetail's bare F6 (row 50): the binding sampler's two passes keep them
	# apart (Ctrl+F6 fires only hudcolor, F6 only huddetail), and when a remap
	# lands both rows on one modifier-less key retail's first-match scan fires
	# only huddetail (row 50 < 76) -- poll_hud_keys carries that shadowing
	# (D-CTRL-4). showhud (row 27) ships unbound. Ctrl is a binding modifier,
	# never a chord guard here; Shift/Alt chords stay ours (the debug pick
	# rides Shift+F6).
	# [orig: the key scan's fire @0x49d42f (the modifier pass) and @0x49d488
	#  (the fallback); huddetail dispatch @0x4E0601; showhud dispatch @0x4E0561]
	# The key sampling is this presenter's; the edge latches, the cycles, the
	# window toggles and the shared-key shadowing are the engine's poll.
	var hud_keys_chorded := Input.is_key_pressed(KEY_SHIFT) or Input.is_key_pressed(KEY_ALT)
	var detail_down := ControlsBindings.pressed("huddetail")
	var color_down := ControlsBindings.pressed("hudcolor")
	var toggle_events := _toggles.poll(
			detail_down, color_down,
			detail_down and color_down and _hud_rows_share_key(),
			ControlsBindings.pressed("showhud"),
			ControlsBindings.pressed("dotsize"), ControlsBindings.pressed("Goals"),
			ControlsBindings.pressed("view1st"), ControlsBindings.pressed("viewwithgun"),
			ControlsBindings.pressed("viewchase"), ControlsBindings.pressed("playerlist_alt"),
			ControlsBindings.pressed("OldMessages"), ControlsBindings.pressed("ShowScore"),
			hud_keys_chorded, gameplay_input_active, sim.is_mp_session())
	# The window actions' respawn init also closes the sim's map overlay (the
	# witness rides hud_toggles.h kOverlayWindowsCleared).
	if toggle_events & HudToggles.EVENT_OVERLAY_WINDOWS_CLEARED:
		sim.request_hud_map_close()
	_apply_toggle_events(toggle_events)
	# Weapon-cluster state: clip/reserve as the info struct carried them, heat
	# 0..0xFFFF (only emplaced/vehicle heavy guns author heat_values, so 0 on
	# foot [orig: hudInfo+60 = WeaponSlot_CalcAccumulatedHeat @0x53f780,
	# @0x4b8533]); the exact crosshair dispersion including both live body
	# accumulators — the simulation owns the stance/aimed-shot row (body-state
	# predicates), the HUD owns only projection [orig: @0x592b07..0x592bf5];
	# the sim's promoted aimed-shot verdict gates the reticle [orig: the
	# @0x4de4f7 promoter; Player_CanFireWeapon @0x5cf780], with the equipped
	# weapon's Inset keep-up predicate from the native view frame; and
	# the PowerThrow windup driving the charge bar [orig: g_fireChargeStartTick
	# @0xB76800 read by HUD_DrawPowerThrowChargeBar @0x599830].
	var live := wv != null and wv.active
	_game_hud.set_weapon_state(weapon_active and weapon != null, clip, reserve,
			wv.heat if live else 0,
			wv.hud_spread_fp16 if live else 0,
			wv.aimed_shot_available if live else false,
			keep_crosshair_while_aimed,
			live and wv.windup_active,
			wv.windup_held_ticks if live else 0)
	# The crosshair's witnessed anchor: Vector2.INF in first person (the overlay
	# pins the design center @0x5928a0), the projected aim in 3P/spectate
	# (@0x592910).
	_game_hud.set_scope_state(lv, Strings.get_table(Strings.TABLE_GAMETEXT))
	var combat_camera := _game_hud.get_viewport().get_camera_3d()
	if _inset_scope != null:
		_inset_scope.update_view(lv, combat_camera, _aspect_mode)
	_sync_second_scene_camera()
	_game_hud.set_combat_state(lv,
			combat_camera.global_transform if combat_camera != null else Transform3D.IDENTITY,
			hud_view_projection(combat_camera) if combat_camera != null else Projection.IDENTITY,
			combat_camera != null, Strings.get_table(Strings.TABLE_GAMETEXT),
			ControlsBindings.model().display_text_for_token("useitem"))
	_game_hud.set_view_state(binoculars_view_active,
			_player_presenter.aim_screen_point() \
					if _player_presenter != null else Vector2.INF)
	_vehicle_panel.update(_game_hud, _hud_pos,
			_world.get_item_db() if _world != null else null, sim, stance)
	if waypoint != null:
		_game_hud.set_waypoint(waypoint.text_name, waypoint.distance_m,
				waypoint.mission_position, waypoint.altitude_wu)
	else:
		_game_hud.clear_waypoint()
	_apply_objectives()
	# The SIGHTS card switch [orig: Player_IsEquippedWeaponScoped @0x4dcc80],
	# suppressed by the binocular view like the ported shell HUD folded it.
	if _sights_card != null:
		_sights_card.set_card_up(scope_card and not binoculars_view_active)
	# The scoped-view circle mask. retail: the scene frame's overlay fork picks
	# binoculars, then the Sighted card, then the Scoped card -- and only the
	# Scoped arm chains the mask, unconditionally, with one argument saying the
	# card drew no AUTHORED row (a missing texture does not change that count).
	# The fork itself is the engine's (HudPos.scoped_view_overlay ->
	# runtime/hud/scope_circle_mask.h); the vehicle-attack context clears both
	# selector bytes before it. See docs/interface/hud-re.md.
	if _scope_circle_mask != null:
		var card_selectors := scope_card and not vehicle_attack_context \
				and weapon != null
		var overlay_branch := HudPos.scoped_view_overlay(binoculars_view_active,
				card_selectors and weapon.sighted_selector,
				card_selectors and weapon.scoped_selector)
		_scope_circle_mask.set_mask_state(overlay_branch == 3,
				weapon == null or weapon.sights.is_empty())
	if _view_effects != null:
		_view_effects.update_view(binoculars_view_active, binocular_range,
				nvg_visible, nvg_gain)
		_view_effects.update_damage_feedback(flash_white, flash_red,
				flash_revive, flash_revive_channel)
	# retail: while the white hit flash burns, the whole HUD overlay pass
	# early-returns, so a collision or explosion blanks the gameplay HUD for up
	# to 64 ticks. The overlay's own draw list is what that pass covers; its
	# children (this node's view effects, the SIGHTS card) belong to the scene
	# frame and keep drawing, which is exactly what self_modulate leaves alone.
	# The addressed witness lives in docs/interface/hud-re.md.
	_game_hud.self_modulate = Color(1.0, 1.0, 1.0, 0.0) if hud_overlays_suppressed \
			else Color.WHITE
	var probe_t4 := Time.get_ticks_usec() if stats_on else 0
	# Effects drain synchronously during _world.tick(), before this HUD update.
	# Flush afterward so GameHud.push_message stamps the current 62 Hz tick.
	_flush_pending_hud_messages()
	_flush_feed_events()
	_game_hud.set_kill_announcement(sim.get_kill_announcement_text(), sim.get_kill_announcement_tick(_hud_ticks()))
	_score_fanfare.update(sim, _world)
	# The three overlay windows follow the engine's toggle flags (the ShowScore
	# gate, the sibling close and the respawn clears are its rules).
	_message_log.update(_game_hud, sim, _toggles.is_message_log_open())
	_end_round_stats.update(_game_hud, sim, _toggles.is_end_round_stats_open())
	_lfp_panel.update(_game_hud, sim, _hud_ticks())
	_scoreboard.update(_game_hud, _world, _toggles.is_scoreboard_open(), _hud_ticks())
	if stats_on:
		var probe_t5 := Time.get_ticks_usec()
		_frame_stats.add(FrameStats.HUD_SCALARS, probe_t1 - probe_t0)
		_frame_stats.add(FrameStats.HUD_ATTACH, probe_t2 - probe_t1)
		_frame_stats.add(FrameStats.HUD_WAYPOINT, probe_t3 - probe_t2)
		_frame_stats.add(FrameStats.HUD_INFO, probe_t4 - probe_t3)
		_frame_stats.add(FrameStats.HUD_FLUSH, probe_t5 - probe_t4)


# The HUD's presentation clock driving the fade/message timers — the engine's
# ms -> tick quantum count (world/tick_accumulator.h ticks_from_ms carries the
# [orig: current_tick @0x24c1968] witness). Adopting the engine's exact 16 ms
# quantum corrects the old 0.062 ticks/ms approximation (62 Hz vs 62.5 Hz).
func _hud_ticks() -> int:
	return Simulation.ticks_from_ms(_world.frame_clock_ms)


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
	var wp := sim.get_waypoint_hud_view()
	if not wp.show or wp.current < 0:
		return null
	var pos := wp.position
	var player: Vector3 = sim.get_local_player_position()
	var entry := WaypointHudEntry.new()
	entry.text_name = _resolve_waypoint_name(wp.name_id)
	entry.mission_position = Vector2(pos.x, -pos.z)
	entry.altitude_wu = pos.y
	# Horizontal-only (mission X/Y deltas = the Godot ground plane), truncated
	# to whole meters natively. [orig: @0x594836 sar 16]
	entry.distance_m = HudPos.waypoint_distance_m(
			Vector2(pos.x - player.x, pos.z - player.z))
	return entry


# The waypoint display name (the engine's get_waypoint_name rule with its
# STRWPNAMEDEFAULT fallback; hud_game_text.h) over the mission and gametext tables.
func _resolve_waypoint_name(name_id: int) -> String:
	return HudPos.waypoint_display_name(Strings.get_table(Strings.TABLE_MISSION),
			Strings.get_table(Strings.TABLE_GAMETEXT), name_id)


# The projection the HUD projects world points through (the attach labels, the
# friendly tags): the local player presenter's frame projection -- while an
# aspect mode draws through its stretched target, the target camera's, whose
# pixels the blit stretches over the surface the overlay draws on; the play
# camera then carries only a CULLING SUPERSET of the frustum and would land a
# label off on one axis (LocalPlayerPresenter.view_projection) -- else the
# play camera's own. Public as the ADR 0018 read seam.
func hud_view_projection(camera: Camera3D) -> Projection:
	if _player_presenter != null and _player_presenter.camera() != null:
		return _player_presenter.view_projection()
	return camera.get_camera_projection()


# The floating attach labels: the sim's selection (distance/LOS/occupancy/nearest,
# armory-zone mode) projected through the frame's projection (hud_view_projection)
# to overlay pixels with the resolved label text — the overlay's own fill
# (HudOverlay.set_attach_labels carries the witness); no camera clears the labels.
func _apply_attach_labels() -> void:
	if _game_hud == null:
		return
	var sim: Simulation = _world.get_sim() if _world != null else null
	var camera: Camera3D = _game_hud.get_viewport().get_camera_3d() \
			if sim != null else null
	if camera == null:
		_game_hud.set_attach_labels(Transform3D.IDENTITY, Projection.IDENTITY, null, null)
		return
	_game_hud.set_attach_labels(camera.global_transform, hud_view_projection(camera),
			Strings.get_table(Strings.TABLE_GAMETEXT), sim)




## The live HudOverlay node (null until the first in-world HUD frame builds it).
func get_game_hud() -> HudOverlay:
	return _game_hud


# The overhead friendly tags (D-HUD-20): the sim's pool-0 gather projected
# through the frame's projection (hud_view_projection) with the environment's
# live fog distance — the overlay's own fill (HudOverlay.set_friendly_tags
# carries the witness); no camera clears the tags.
func _apply_friendly_tags() -> void:
	if _game_hud == null:
		return
	var fog_distance := 0.0
	var env: MissionEnvironment = _world.get_environment_node() \
			if _world != null else null
	if env != null:
		fog_distance = env.get_fog_distance()
	var sim: Simulation = _world.get_sim() if _world != null else null
	var camera: Camera3D = _game_hud.get_viewport().get_camera_3d() \
			if sim != null else null
	if camera == null:
		_game_hud.set_friendly_tags(false, Transform3D.IDENTITY, Projection.IDENTITY,
				fog_distance, null)
		_game_hud.set_radio_request_icon_viewer(false)
		return
	_game_hud.set_friendly_tags(true, camera.global_transform, hud_view_projection(camera),
			fog_distance, sim)
	_game_hud.set_radio_request_icon_viewer(sim.local_player_radio_request_icon_viewer())


# The weapon's HUD display name (the engine's WepDes rule with its miss;
# hud_game_text.h).
func _resolve_weapon_display_name(weapon_name: String) -> String:
	return HudPos.weapon_display_name(Strings.get_table(Strings.TABLE_GAMETEXT), weapon_name)


# Mission effects feed the HUD's text surfaces. Drained effects carry
# {kind, a..d, str}: WAC text/ptext/text# carries a literal in `str` and posts
# into the CHAT ring, while BMS OutputText carries a nonzero Triggered-Text id in
# `a` and posts into the SYSTEM ring. consol/pconsol/consol# and forceanim use
# the `debug_text` kind: the SYSTEM ring too, without touching the objective
# line. The lose and subgoal lines are CHAT ring lines. Queue every form because
# PreMission effects can arrive before the lazy HUD and its mission table exist.
# Public with hud_objective_line() as the ADR 0018 read seam.
func apply_mission_effects(effects: Array) -> void:
	for e_v in effects:
		var e := e_v as MissionEffect
		if e == null:
			continue
		var kind := e.kind
		if kind == "local_round_reset":
			if _game_hud != null:
				_game_hud.reset_overlay_buffers()
		elif kind == "text":
			var t := e.text
			if not t.is_empty():
				_hud_objective = t
				_queue_chat_line(t)
			else:
				var text_id := e.a
				if text_id != 0:
					_queue_hud_message("", text_id)
		elif kind == "debug_text":
			var line := e.text
			if not line.is_empty():
				_queue_hud_message(line, 0)
		elif kind == "lose":
			# The WAC Lose banner trio [orig: WacAction_Lose @0x4ed3f0 ->
			# GameMsg_AddChatLineAndRelay @0x5ba170 (the chat-feed line; the KEY rides
			# the wire and each client re-resolves it) + GameMsg_SetBannerText @0x5ba200
			# / GameMsg_SetTeamBannerText @0x5ba1d0 (the persistent banner buffers the
			# MISSION FAILED screen composes, cleared at the next round start
			# @0x5b71b0)]. The effect carries the gametext key; resolve against the
			# 'Misc' section like the original.
			var key := e.text
			if not key.is_empty():
				var line := Strings.lookup_display(Strings.TABLE_GAMETEXT, "Misc", key)
				_endround_banner = line
				_queue_chat_line(line)
		elif kind == "subgoal_won" or kind == "subgoal_lost":
			# A subgoal resolved: the mission-text announcement rides the chat
			# feed (b = the header text id, c = the round-still-running gate);
			# a LOST subgoal also stamps the persistent banner. The section and
			# the key are the engine's (hud_game_text.h subgoal_message).
			if e.c != 0:
				var lost := kind == "subgoal_lost"
				var line := HudPos.subgoal_message(
						Strings.get_table(Strings.TABLE_MISSION), lost, e.b)
				if not line.is_empty():
					if lost:
						_endround_banner = line
					_queue_chat_line(line)


func hud_objective_line() -> String:
	return _hud_objective


## The stored end-of-round banner (the WAC Lose cause line), for the end screen.
func endround_banner_line() -> String:
	return _endround_banner


## The objectives-panel toggle, flipped by the shell's objectives key
## (the engine's rule; applied by _apply_objectives each tick).
func toggle_objectives() -> void:
	_toggles.toggle_objectives()


## The friendly-tags mode cycle with the retail toast through the message
## feed: the cycle and the toast key are the engine's, the table lookup and
## the push are this presenter's.
func cycle_friendly_tags() -> void:
	var key := _toggles.cycle_friendly_tags()
	if _game_hud == null:
		return
	_game_hud.set_friendly_tag_mode(_toggles.get_friendly_tag_mode())
	var t: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	if t != null and t.has_string_in_section("Misc", key):
		_game_hud.push_message(t.get_string_in_section("Misc", key))


## Seed the toggles from the persisted config tokens (the color index clamps
## like retail's read-back; the declutter level is stored verbatim).
static func _seed_toggles(color_index: int, detail_level: int) -> HudToggles:
	var toggles := HudToggles.new()
	toggles.set_hud_color_index(HudOverlay.clamp_hud_color_index(color_index))
	toggles.set_hud_detail_level(detail_level)
	return toggles


## Apply the device side effects of one poll's events: the overlay restamps,
## the hudcolor config write (retail's token round trip; deliberately no
## toast), the SIGHTS card's scale cycle, the FP gun bit and the camera
## preference the view actions select.
func _apply_toggle_events(events: int) -> void:
	if events & HudToggles.EVENT_HUD_DETAIL_CYCLED:
		_push_hud_detail_level()
	if events & HudToggles.EVENT_HUD_COLOR_CYCLED:
		ConfigStore.write(HUD_COLOR_CONFIG_PATH, HUD_COLOR_SECTION,
				HUD_COLOR_CONFIG_KEY, _toggles.get_hud_color_index())
		if _game_hud != null:
			_game_hud.set_hud_color_index(_toggles.get_hud_color_index())
	if events & (HudToggles.EVENT_SHOWHUD_CYCLED | HudToggles.EVENT_GUN_BIT_CHANGED):
		_push_showhud_flags()
	if events & HudToggles.EVENT_DOTSIZE_CYCLED:
		cycle_sight_scale()
	if events & HudToggles.EVENT_FIRST_PERSON_SELECTED:
		_select_third_person(false)
	if events & HudToggles.EVENT_THIRD_PERSON_SELECTED:
		_select_third_person(true)


## One hudcolor poll step over pre-sampled device state (the seam the tests
## drive); every other row idle.
func poll_hud_color_edge(color_down: bool, chorded: bool, active: bool) -> void:
	_apply_toggle_events(_toggles.poll(false, color_down, false, false, false, false,
			false, false, false, false, false, false, chorded, active, false))


func cycle_hud_color() -> void:
	_toggles.cycle_hud_color()
	_apply_toggle_events(HudToggles.EVENT_HUD_COLOR_CYCLED)


## One poll step over the pre-sampled huddetail + hudcolor key states with the
## live shared-key shadowing (D-CTRL-4) -- the seam the tests drive.
func poll_hud_keys(detail_down: bool, color_down: bool, chorded: bool,
		active: bool) -> void:
	_apply_toggle_events(_toggles.poll(detail_down, color_down, _hud_rows_share_key(),
			false, false, false, false, false, false, false, false, false, chorded, active,
			false))


# Whether the huddetail and hudcolor rows currently resolve to a common bound
# key (the D-CTRL-4 shadowing predicate the engine poll applies; both default F6).
func _hud_rows_share_key() -> bool:
	var detail_keys: PackedInt32Array = \
			ControlsBindings.model().godot_keys_for_token("huddetail")
	for key in ControlsBindings.model().godot_keys_for_token("hudcolor"):
		if key != 0 and detail_keys.has(key):
			return true
	return false


## One huddetail poll step (the seam the tests drive); every other row idle.
func poll_hud_detail_edge(detail_down: bool, chorded: bool, active: bool) -> void:
	_apply_toggle_events(_toggles.poll(detail_down, false, false, false, false, false,
			false, false, false, false, false, false, chorded, active, false))


func cycle_hud_detail() -> void:
	_toggles.cycle_hud_detail()
	_push_hud_detail_level()


## The one LIVE declutter-level write seam: the cycle, the death-screen force
## and the mission-start re-seed all land here; it restamps a built HUD and
## never touches the persisted config value (retail's cycle and death force
## write the layer level only; game.cfg carries the config value).
func set_hud_detail_level(level: int) -> void:
	_toggles.set_hud_detail_level(level)
	_push_hud_detail_level()


func _push_hud_detail_level() -> void:
	if _game_hud != null:
		_game_hud.set_hud_detail_level(_toggles.get_hud_detail_level())


## The mission-start apply: the live level re-seeded from the persisted config
## value (retail applies its session settings to the globals at every mission
## start, so a death screen's forced blank ends with its mission).
func reapply_persisted_hud_detail() -> void:
	set_hud_detail_level(_hud_detail_config)


func hud_detail_level() -> int:
	return _toggles.get_hud_detail_level()


## The persisted config value (read at boot; the settings path owns writes).
func hud_detail_config() -> int:
	return _hud_detail_config


## The persisted HUD color-scheme index (the token cycle_hud_color writes).
func hud_color_index() -> int:
	return _toggles.get_hud_color_index()


## The process-lifetime friendly-tags mode (cycle_friendly_tags advances it).
func friendly_tag_mode() -> int:
	return _toggles.get_friendly_tag_mode()


## Temporarily apply retail's blank HUD declutter level around a capture. The
## overlay, its PlayerViewEffects child, and the shell HUD CanvasLayer stay
## mounted and active; only compiled gameplay HUD commands are decluttered.
## Pair with finish_hud_hidden_capture().
func begin_hud_hidden_capture() -> Error:
	if _hud_hidden_capture_active:
		return ERR_BUSY
	if _game_hud == null or not is_instance_valid(_game_hud) \
			or _view_effects == null or not is_instance_valid(_view_effects) \
			or _sights_card == null or not is_instance_valid(_sights_card):
		return ERR_UNCONFIGURED
	_hud_hidden_saved_detail_level = _toggles.get_hud_detail_level()
	_hud_hidden_capture_active = true
	_toggles.set_hud_detail_level(HudOverlay.hud_detail_level_blank())
	_game_hud.set_hud_detail_level(_toggles.get_hud_detail_level())
	return OK


## Idempotent capture cleanup: restore the exact live level that was active
## before the capture.
func finish_hud_hidden_capture() -> void:
	if not _hud_hidden_capture_active:
		return
	_hud_hidden_capture_active = false
	_toggles.set_hud_detail_level(_hud_hidden_saved_detail_level)
	if _game_hud != null and is_instance_valid(_game_hud):
		_game_hud.set_hud_detail_level(_toggles.get_hud_detail_level())


## Semantic presentation witness for a HUD-hidden capture. This compiles the
## live overlay and observes every gameplay draw family instead of inferring
## hidden state from the requested declutter number.
func hud_hidden_capture_witness() -> HudHiddenCaptureWitness:
	var witness := HudHiddenCaptureWitness.new()
	if not _hud_hidden_capture_active:
		witness.error = "HUD-hidden capture is not active"
		return witness
	if _game_hud == null or not is_instance_valid(_game_hud):
		witness.error = "gameplay HUD is unavailable"
		return witness
	var stats: HudDrawListStats = _game_hud.get_draw_list_stats()
	var gameplay_draw_count := stats.quads + stats.tris + stats.lines + stats.glyphs \
			+ stats.underlines + stats.elements_drawn
	var map_active := stats.map_visible
	var big_map_active := stats.big_map_visible
	witness.hud_detail_level = int(_game_hud.get_hud_detail_level())
	witness.gameplay_hud_visible = gameplay_draw_count > 0 \
			or map_active or big_map_active
	witness.player_view_effects_active = _view_effects != null \
			and is_instance_valid(_view_effects) \
			and _view_effects.is_visible_in_tree()
	witness.ads_active = _sights_card != null \
			and is_instance_valid(_sights_card) \
			and bool(_sights_card.is_card_up())
	witness.big_map_active = big_map_active
	return witness


## The death-screen edge forces the declutter level to max through the same
## seam the cycle uses (the engine's rule; restamped like every live write).
func apply_death_screen_hud_detail() -> void:
	_toggles.force_death_screen_hud_detail()
	_push_hud_detail_level()


## The showhud edge poll (the seam the tests drive); every other row idle.
func poll_showhud_edge(showhud_down: bool, chorded: bool, active: bool) -> void:
	_apply_toggle_events(_toggles.poll(false, false, false, showhud_down, false, false,
			false, false, false, false, false, false, chorded, active, false))


## The showhud cycle (the engine's flags rule): bit 1 feeds the overlay's
## corner spinmap block, bit 0 the FP viewmodel rig through the player presenter.
func cycle_showhud() -> void:
	_toggles.cycle_showhud()
	_push_showhud_flags()


func _push_showhud_flags() -> void:
	if _game_hud != null:
		_game_hud.set_showhud_flags(_toggles.get_showhud_flags())
	_apply_fp_gun_visible()


## The dotsize edge poll (the seam the tests drive); every other row idle.
func poll_dotsize_edge(dotsize_down: bool, chorded: bool, active: bool) -> void:
	_apply_toggle_events(_toggles.poll(false, false, false, false, dotsize_down, false,
			false, false, false, false, false, false, chorded, active, false))


## The dotsize cycle: the overlay advances its per-player sight-scale index
## (the engine's policy, runtime/hud/sight_overlay.h: default 1, then
## 2 -> 0 -> 1 ...) and the SIGHTS card re-resolves its `scale` rows.
func cycle_sight_scale() -> void:
	if _game_hud == null:
		return
	_game_hud.cycle_sight_scale()
	_push_sight_state()


## Live sight-scale and zero/rangefinder slide state from the equipped slot.
func _push_sight_state() -> void:
	if _sights_card == null or _game_hud == null:
		return
	_sights_card.set_sight_state(_game_hud.get_sight_scale_index(),
			_sight_slide_multiplier)


## The Goals edge poll (the seam the tests drive); every other row idle.
func poll_goals_edge(goals_down: bool, chorded: bool, active: bool) -> void:
	_apply_toggle_events(_toggles.poll(false, false, false, false, false, goals_down,
			false, false, false, false, false, false, chorded, active, false))


## The view-action rows (view1st / viewwithgun / viewchase) poll over
## pre-sampled state (the seam the tests drive); every other row idle.
func poll_view_action_edges(view1st_down: bool, viewwithgun_down: bool,
		viewchase_down: bool, chorded: bool, active: bool) -> void:
	_apply_toggle_events(_toggles.poll(false, false, false, false, false, false,
			view1st_down, viewwithgun_down, viewchase_down, false, false, false, chorded,
			active, false))


func _select_third_person(selected: bool) -> void:
	if _player_presenter != null:
		_player_presenter.set_third_person_selected(selected)


func _apply_fp_gun_visible() -> void:
	if _player_presenter != null:
		_player_presenter.set_fp_gun_visible(
				(_toggles.get_showhud_flags() & HudOverlay.SHOWHUD_FLAG_GUN) != 0)


# The objectives panel: the sim's shown win-condition rows resolved through
# the mission text table by the overlay's own fill (Simulation.fill_objectives
# carries the witness); the toggle's off state clears the panel.
func _apply_objectives() -> void:
	if _game_hud == null:
		return
	var sim: Simulation = _world.get_sim() if _world != null else null
	_game_hud.set_objectives(_toggles.is_objectives_visible(),
			Strings.get_table(Strings.TABLE_MISSION), sim)


## Number of player-facing messages waiting for the lazy HUD to mount.
## This is the ADR 0018 read seam for presenter tests and diagnostics.
func pending_hud_message_count() -> int:
	return _pending_hud_messages.size()


## How many of those pending messages are CHAT ring lines (the same seam).
func pending_chat_line_count() -> int:
	var count := 0
	for pending in _pending_hud_messages:
		if pending.chat:
			count += 1
	return count


func _queue_hud_message(text: String, text_id: int) -> void:
	_pending_hud_messages.append(PendingHudMessage.new(text, text_id))
	while _pending_hud_messages.size() > MAX_PENDING_HUD_MESSAGES:
		_pending_hud_messages.pop_front()


func _queue_chat_line(text: String) -> void:
	_pending_hud_messages.append(PendingHudMessage.new(text, 0, true))
	while _pending_hud_messages.size() > MAX_PENDING_HUD_MESSAGES:
		_pending_hud_messages.pop_front()


## The message feed: this frame's folded S2C 0x1E game events, each resolved
## into the game's own canned sentence and posted to the SYSTEM ring. The sim
## hands over the rows; the gametext resolve and the witnessed substitution
## are the engine's (FeedRow.resolve_line over hud::feed_row_line), so the
## sentence is always the game's own text and never one we compose.
func _flush_feed_events() -> void:
	if _game_hud == null or _world == null:
		return
	var sim: Simulation = _world.get_sim()
	if sim == null:
		return
	var rows: Array[FeedRow] = sim.drain_feed_events()
	if rows.is_empty():
		return
	var table: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	if table == null:
		return
	for row: FeedRow in rows:
		var line := row.resolve_line(table)
		if line.is_empty():
			continue
		_game_hud.push_feed_line(line, row.get_color())
		if row.is_announcement():
			sim.retain_feed_announcement(line, _hud_ticks())


func _flush_pending_hud_messages() -> void:
	if _game_hud == null:
		return
	for pending in _pending_hud_messages:
		if pending.chat:
			_game_hud.push_chat_line(pending.text, SCRIPT_CHAT_ARGB)
		elif not pending.text.is_empty():
			_game_hud.push_message(pending.text)
		else:
			_show_triggered_text(pending.text_id)
	_pending_hud_messages.clear()


# The mission's "Triggered Text" line (the engine's key rule; hud_game_text.h
# triggered_text): a miss shows nothing.
func _show_triggered_text(text_id: int) -> void:
	if _game_hud == null:
		return
	var text := HudPos.triggered_text(Strings.get_table(Strings.TABLE_MISSION), text_id)
	if text.is_empty():
		push_warning("GameHud: mission text ID%03d not found in the mission string table." % text_id)
		return
	_hud_objective = text
	_game_hud.push_message(text)
