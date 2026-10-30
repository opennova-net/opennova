class_name GameHudPresenter
extends Node

## The in-game HUD's device side for a live GameWorld in the game shell: it builds, loads and
## mounts the native HudOverlay and its child controls (the SIGHTS card, the scoped circle mask,
## the binocular/NVG view effects, the Inset camera), and drives them each frame from the HUD
## session (HudSession over engine runtime/hud/hud_session.h), which owns the process's toggles
## and chat line, the mission effects' routing, the toggle events' work, the declutter and pause
## words and the frame's whole overlay state. The shells only say when the player is in-world
## (they gate tick()).

const HudSightsCardScript := preload("res://game/world/hud_sights_card.gd")

## The commander_menu row fired (the poll ran its gates and the respawn init): the shell opens
## the CMAP screen (HudSession.WORK_COMMAND_MAP).
signal command_map_requested
## The quit dialog's yes key ran the Exit Mission action (engine hud_toggles.h
## hud_toggles_quit_dialog_key): the shell plays the exit tone and leaves the mission.
signal quit_confirmed
## The Exit Mission action's interface tone (the trigger-set registry's PU_EXIT_CONFIRM row;
## engine hud_toggles.h carries the witness).
const QUIT_CONFIRM_SOUNDSET := "PU_EXIT_CONFIRM"
const HudScopeCircleMaskScript := preload("res://game/world/hud_scope_circle_mask.gd")
const PlayerViewEffectsScript := preload("res://game/world/player_view_effects.gd")
# The view veil's two programs (HudOverlay's, which loads the same shaders by path): the sun
# veil's, and the revive tint's multiply, a Shader rather than a CanvasItemMaterial, whose program
# waits for the first idle frame past a mission load (the reveal); a Shader makes its device
# program at its first get_rid(), and the load's HUD leg makes both (_make_canvas_programs,
# D-NET-1367).
const SUN_VEIL_SHADER := preload("res://shaders/sun_veil_overlay.gdshader")
const REVIVE_TINT_SHADER := preload("res://shaders/canvas_blend_mul.gdshader")

const HudHiddenCaptureWitness := preload(
		"res://game/world/hud_hidden_capture_witness.gd")

var _world: GameWorld = null
var _player_presenter: LocalPlayerPresenter = null  # the aim point + the frame's projection
var _ui_parent: Node = null
# PlayerOptions seeds these before the lazy native HUD exists; they survive teardown so the next
# mission build uses the same process-lifetime choice.
var _crosshair_style := HudOverlay.MIN_CROSSHAIR_STYLE
var _crosshair_color: int = PlayerOptions.DEFAULT_CROSSHAIR_COLOR
var _aspect_mode := -1
var _crosshair_spread: bool = PlayerOptions.DEFAULT_CROSSHAIR_SPREAD

# The HUD session: process-lifetime like retail's globals, seeded from the cfg block's persisted
# words, so its toggles and chat line survive the per-mission HUD rebuild.
var _session: HudSession = HudSession.from_config(GameCfgStore.block())

# HudOverlay, built and loaded inside every mission start's load (the world's hud_overlay_init /
# hud_textures_load legs) and mounted on the first frame the mission has a local player
# (ensure_game_hud(); the GUT files build the real one over a staged root).
var _game_hud: HudOverlay = null
# Whether the load's forced frame mounted the overlay (_on_hud_pipeline_warm), which the frame's
# end takes off the tree again.
var _hud_warm_mounted := false
# Whether the forced frame's draws stand on the overlay's own item and the veil's (has_warm_draw).
var _hud_warm_drawn := false
# The canvas programs the load's HUD leg made (_make_canvas_programs).
var _load_canvas_programs: Array[RID] = []
# The HUD's process words across the per-mission overlay rebuild (engine hud_frame.h
# HudProcessWords carries the witness).
var _process_words: HudProcessWordsCarry = null
# Whether the HUD's textures leg ran the full arm (false: a dedicated host's reduced arm, which
# loads no terrain picture).
var _hud_full_arm := true
var _hud_pos: HudPos = null  # the loaded hudpos.def
var _inset_scope: HudInsetScope = null
var _sights_card: HudSightsCard = null # child of the overlay (per-row blend controls)
var _nvg_scene_card_published := false
var _scope_circle_mask: HudScopeCircleMask = null # child of the overlay (the scoped annulus)
var _view_effects: PlayerViewEffects = null # child of the overlay (binocular/NVG stack)
var _warned_no_player := false
# The shared F3 frame-stats board (null outside the game shell): while its Stats tab captures,
# the frame's phase spans land there as HUD_* slots.
var _frame_stats: FrameStats = null


func setup(world: GameWorld, player_presenter_in: LocalPlayerPresenter, ui_parent: Node) -> void:
	_world = world
	_player_presenter = player_presenter_in
	_ui_parent = ui_parent
	_session.set_world(world)
	_session.set_player_presenter(player_presenter_in)
	# The F9 Emotes / F10 Radio menus and the talk rows ride the player input router's frames
	# (engine controls PlayerActions' menu arms carry the witness).
	if _player_presenter != null:
		_player_presenter.set_hud_toggles(_session.get_toggles())
		_player_presenter.set_hud_chat(_session.get_chat(), Strings.get_table(Strings.TABLE_GAMETEXT))
	# Connect before any world can tick: PreMission/WAC effects may drain on the first runtime
	# tick, while the local-player HUD is built only after it (the session queues them). The
	# connect persists for the presenter's lifetime; teardown only resets per-mission state.
	if _world != null and not _world.mission_effects.is_connected(apply_mission_effects):
		_world.mission_effects.connect(apply_mission_effects)
	if _world != null and not _world.minimap_water_changed.is_connected(
			_on_minimap_water_changed):
		_world.minimap_water_changed.connect(_on_minimap_water_changed)
	# The HUD builds and loads inside every mission start's load, at its two HUD legs (GameWorld
	# carries the witness, D-NET-1167).
	if _world != null and not _world.hud_overlay_init.is_connected(_on_hud_overlay_init):
		_world.hud_overlay_init.connect(_on_hud_overlay_init)
	if _world != null and not _world.hud_textures_load.is_connected(_on_hud_textures_load):
		_world.hud_textures_load.connect(_on_hud_textures_load)
	if _world != null and not _world.hud_pipeline_warm.is_connected(_on_hud_pipeline_warm):
		_world.hud_pipeline_warm.connect(_on_hud_pipeline_warm)


## The chat presets F1..F10 insert: the player profile's ten macros, which each mission start's
## session apply copies (engine: hud/hud_chat_entry.h).
func set_chat_presets(macros: PackedStringArray) -> void:
	_session.set_chat_presets(macros)


## Undo everything a mission built: the HUD node (its card/effects children go with it) and the
## session's per-mission state. `restart` is the SP restart's teardown (the tip's once-counters
## survive it).
func teardown(restart := false) -> void:
	_session.end_mission(restart)
	if _game_hud != null:
		_process_words = _game_hud.get_process_words()
		_game_hud.queue_free()
		_game_hud = null
	_hud_warm_mounted = false
	_hud_warm_drawn = false
	if _world != null:
		_world.set_inset_scope(null)
	_inset_scope = null
	_sync_second_scene_camera()
	_sights_card = null
	_scope_circle_mask = null
	_view_effects = null
	_hud_pos = null
	_warned_no_player = false


## The layer every HUD element hangs under (hiding it hides the whole HUD, siblings of the GameHud
## control included; the perf probe's canvas A/B).
func get_ui_parent() -> Node:
	return _ui_parent


func _on_minimap_water_changed(mask: ImageTexture) -> void:
	# A reduced arm's HUD holds no terrain picture (_load_hud_textures). The load makes the mask
	# with the terrain's textures, ahead of the HUD's: the textures leg takes it then, so an
	# overlay not yet loaded waits for that leg rather than making the map picture twice.
	if _game_hud == null or not _game_hud.is_configured() or not _hud_full_arm:
		return
	_game_hud.set_minimap_terrain(
			_world.get_terrain_data() if _world != null else null, mask,
			_session_texcompression_level())


## The session's texcompression_level the map's colormap quadrants and the SIGHTS card pictures
## were made under (GameWorld's mission-start copy; D-RMAT-24).
func _session_texcompression_level() -> int:
	return _world.get_session_texcompression_level() if _world != null \
			else GameWorld.texcompression_level_fresh_profile()


## Hand the world's particle renderer the weapon Inset pass's camera, or null while that pass is
## not rendering (scope down, HUD torn down): the second view needs the world's particles
## compiled for its own eye. A world that unloaded has no effect world left to tell.
func _sync_second_scene_camera() -> void:
	var effects: EffectWorld = _world.get_effect_world() if _world != null else null
	if effects == null:
		return
	effects.set_second_scene_camera(
			_inset_scope.get_active_render_camera() if _inset_scope != null else null)


## The two tip options (PlayerOptions keyboard_tips / gameplay_tips): the engine tip's show gates.
func set_tip_options(keyboard_tips: bool, gameplay_tips: bool) -> void:
	_session.set_tip_options(keyboard_tips, gameplay_tips)


## The USER crosshair options; cached even before the lazy HUD exists, then applied to an
## existing HUD.
func set_aspect_mode(mode: int) -> void:
	_aspect_mode = mode
	if _game_hud != null:
		_game_hud.set_aspect_mode(mode)


func set_crosshair_style(style: int) -> void:
	_crosshair_style = clampi(style, HudOverlay.MIN_CROSSHAIR_STYLE,
			HudOverlay.MAX_CROSSHAIR_STYLE)
	if _game_hud != null:
		_game_hud.set_crosshair_style(_crosshair_style)


# The in-game options' Accept: the crosshair art again in alpha mode
# (HudOverlay.reload_crosshair_alpha, D-HUD-48).
func reload_crosshair_alpha() -> void:
	if _game_hud != null:
		_game_hud.reload_crosshair_alpha()


func crosshair_style() -> int:
	return _crosshair_style


func set_crosshair_color(rgb: int) -> void:
	_crosshair_color = rgb & PlayerOptions.CROSSHAIR_COLOR_MASK
	if _game_hud != null:
		_game_hud.set_crosshair_color(_crosshair_color)


## The shell's open-menu-screen state (any in-game screen over live play), which the native HUD's
## preround armory line yields to (engine hud_frame.h HudFrameState::menu_screen_open).
func set_menu_screen_open(open: bool) -> void:
	_session.set_menu_screen_open(open)


func set_crosshair_spread_enabled(enabled: bool) -> void:
	_crosshair_spread = enabled
	if _game_hud != null:
		_game_hud.set_crosshair_spread_enabled(_crosshair_spread)


## Build, load and mount the overlay if the mission has none yet: the load's two HUD legs
## (_on_hud_overlay_init, _on_hud_textures_load) build and load it inside every mission start's
## load; the first frame with a local player (a dedicated host's first frame) mounts it; the GUT
## files run all three here over a staged root. The SIGHTS card and the PlayerViewEffects post
## stack ride as behind-parent children of the overlay.
func ensure_game_hud() -> void:
	if _game_hud == null:
		_init_overlay_system(_world.get_loaded_mission_file() if _world != null else "")
	if not _game_hud.is_configured():
		var sim: Simulation = _world.get_sim() if _world != null else null
		_load_hud_textures(sim == null or sim.session_role() != Simulation.ROLE_DEDICATED_HOST)
	_mount_game_hud()


## The load's overlay-init leg (GameWorld.hud_overlay_init, every mission start and every role,
## ahead of a joiner's first 0x2F): the overlay and its children are built off the tree and the
## HUD's layout file, string tables and map-grid origin are read (engine
## mission/mission_load_plan.h carries the witness).
func _on_hud_overlay_init(mission_file: String) -> void:
	if _game_hud != null:
		return
	_init_overlay_system(mission_file)


## The load's HUD textures leg (GameWorld.hud_textures_load, at the mission textures stage past
## the model freeze, on every arm, `full_arm` false on a dedicated host's reduced one).
func _on_hud_textures_load(full_arm: bool) -> void:
	if _game_hud != null and not _game_hud.is_configured():
		_load_hud_textures(full_arm)


## The load's one forced frame (GameWorld.hud_pipeline_warm, `drawing` true ahead of its draw,
## false once it drained): the overlay mounts for that frame alone and submits its draw at once
## (the SceneTree is blocked, so a queued redraw would reach the frame empty), so its canvas
## pipelines compile behind the loading screen, not in the first revealed frame (D-NET-1315). It
## leaves the tree again, and the frame's draws leave with it: Godot clears an item at a redraw
## only once it drew the item itself. The first frame with a local player mounts it as before.
func _on_hud_pipeline_warm(drawing: bool) -> void:
	if drawing:
		if _game_hud == null or not _game_hud.is_configured() or _game_hud.is_inside_tree():
			return
		_hud_warm_mounted = true
		var mount: Node = _ui_parent if _ui_parent != null else self
		mount.add_child(_game_hud)
		_game_hud.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		_view_effects.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		_game_hud.submit_draw_now()
		_hud_warm_drawn = true
		return
	if not _hud_warm_mounted:
		return
	_hud_warm_mounted = false
	if _game_hud != null:
		RenderingServer.canvas_item_clear(_game_hud.get_canvas_item())
		_hud_warm_drawn = false
		if _game_hud.is_inside_tree():
			_game_hud.get_parent().remove_child(_game_hud)


## Whether the load's forced frame's draws still stand on the overlay's item and the veil's (a
## read seam for the load pins: none outlives the frame).
func has_warm_draw() -> bool:
	return _hud_warm_drawn


# The overlay system's init: the overlay and its child controls, the hudpos.def parse, then the
# session's mission start over them (the string tables, the map-grid origin, the toggles'
# restamps). Nothing is mounted: the overlay draws from the first frame a mission has a player of
# its own (_mount_game_hud).
func _init_overlay_system(mission_file: String) -> void:
	_game_hud = HudOverlay.new()
	_game_hud.name = "GameHud"
	# The process words the last mission's overlay held; the new overlay's fade stamps and item
	# flash last tick stay zero, as the start's HUD init leaves them.
	_game_hud.carry_process_words(_process_words)
	# The /NOHUD launch flag clears the overlay master word for the process.
	_game_hud.set_no_hud(LaunchFlags.no_hud())
	# The F1 help pages rebuild from the live bindings at every mission start.
	ControlsBindings.model().build_help_screen()
	_game_hud.mouse_filter = Control.MOUSE_FILTER_IGNORE
	# Internal children stay out of scene ownership; INTERNAL_MODE_BACK plus show_behind_parent
	# keeps the post-process/masks a stable layer below the card rows and the overlay's own draw.
	_view_effects = PlayerViewEffectsScript.new()
	_view_effects.name = "PlayerViewEffects"
	_view_effects.show_behind_parent = true
	_view_effects.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_game_hud.add_child(_view_effects, false, Node.INTERNAL_MODE_BACK)
	_inset_scope = HudInsetScope.new()
	_inset_scope.name = "InsetScope"
	_inset_scope.show_behind_parent = true
	_inset_scope.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_inset_scope.visible = false
	_game_hud.add_child(_inset_scope)
	_sights_card = HudSightsCardScript.new()
	_sights_card.name = "SightsCard"
	_sights_card.show_behind_parent = true
	_sights_card.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_game_hud.add_child(_sights_card)
	# The scoped-view circle mask sits AFTER the card in the overlay's child order, so the annulus
	# covers the card's own corners: retail submits the SIGHTS card first and the mask straight
	# after it on the Scoped arm.
	_scope_circle_mask = HudScopeCircleMaskScript.new()
	_scope_circle_mask.name = "ScopeCircleMask"
	_scope_circle_mask.show_behind_parent = true
	_scope_circle_mask.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_scope_circle_mask.visible = false
	_game_hud.add_child(_scope_circle_mask)
	_push_sight_state()
	var hudpos := HudPos.new()
	var root: ResourceRoot = _world.get_resource_root() if _world != null else null
	if root == null:
		push_warning("GameHud: world exposed no resource root; the HUD layout cannot load.")
	elif hudpos.load_from_resource_root(root, "hudpos.def") != OK:
		push_warning("GameHud: hudpos.def did not load: %s" % hudpos.get_last_error())
		# Not there (one that does not parse is no miss): the line the editor's Play reads back
		# into a Problems row (ADR 0046 DI-27).
		if not root.has_file("hudpos.def"):
			ResourceRoot.report_missing("file", "hudpos.def")
	_hud_pos = hudpos
	_session.set_overlay(_game_hud)
	_session.set_hud_pos(hudpos)
	_session.begin_mission(mission_file)


# The HUD's textures, fonts and map pictures over the layout the init read (HudOverlay.configure:
# the HUD loader's textures, the overlay fonts, the stdbox frame), the terrain's colour map and
# the view effects' art. A dedicated host's reduced arm loads the fonts, the stdbox and the
# connection icons alone: no HUD art, no terrain picture, no view effects.
func _load_hud_textures(full_arm: bool) -> void:
	var root: ResourceRoot = _world.get_resource_root() if _world != null else null
	_game_hud.set_crosshair_style(_crosshair_style)
	_game_hud.set_crosshair_color(_crosshair_color)
	_game_hud.set_crosshair_spread_enabled(_crosshair_spread)
	_game_hud.set_aspect_mode(_aspect_mode)
	_hud_full_arm = full_arm
	_game_hud.configure(_hud_pos, root, full_arm)
	if not full_arm:
		return
	# TerrainData owns the TRN 16x16 sector routing table and the colormap texture; the native
	# overlay copies only the portable routing scalars. The raw colormap remains the sharp base;
	# depthspin supplies water only.
	_game_hud.set_minimap_terrain(
			_world.get_terrain_data() if _world != null else null,
			_world.get_minimap_water_mask() if _world != null else null,
			_session_texcompression_level())
	_view_effects.set_resource_root(root)
	# The red damage vignette's art, through the archive loader; retail loads it once into the
	# material behind the quad's mode-3 pass (docs/render/render-material-re.md "Texture loaders").
	_game_hud.set_damage_vignette(root.load_texture(
			HudPos.view_effect_texture(HudPos.VIEW_TEXTURE_VIGNETTE),
			ResourceRoot.TEXTURE_LOADER_ARCHIVE) if root != null else null)
	_make_canvas_programs()


# The HUD's canvas programs, made in the load (D-NET-1367): a Shader makes its device program (a
# shader-cache lookup, a compile on a fresh install) at its first get_rid(), when the first
# material binds it, which for the SIGHTS card's rows is their first build, past a mission load
# the reveal frame.
func _make_canvas_programs() -> void:
	_load_canvas_programs.clear()
	for shader: Shader in [HudSightsCard.ADD_SHADER, HudSightsCard.BLEND_AT_SHADER,
			HudSightsCard.ADD_AT_SHADER, HudSightsCard.MULTIPLY_SHADER,
			SUN_VEIL_SHADER, REVIVE_TINT_SHADER]:
		_load_canvas_programs.append(shader.get_rid())


## The canvas programs the load's HUD leg made, in _make_canvas_programs' order (a read seam for
## the load pins).
func get_load_canvas_programs() -> Array[RID]:
	return _load_canvas_programs


# The first frame a mission has a player of its own (a dedicated host's first frame) puts the
# loaded overlay on the HUD layer; a later frame finds it there.
func _mount_game_hud() -> void:
	if _game_hud.is_inside_tree():
		return
	var mount: Node = _ui_parent if _ui_parent != null else self
	mount.add_child(_game_hud)
	# anchors AND offsets: an anchors-only preset keeps a fresh Control's zero rect, and a
	# clipping UI parent can then clip every HUD element to nothing.
	_game_hud.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_view_effects.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_inset_scope.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_sights_card.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_scope_circle_mask.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	# The world's local-view leg stamps and hands over the Inset camera from the view it just
	# composed, before every Inset leg of the frame reads it.
	if _world != null:
		_world.set_inset_scope(_inset_scope)


func set_frame_stats(board: FrameStats) -> void:
	_frame_stats = board


## Once per frame while the player is in-world (the shells gate on their own state): the
## session's frame over the overlay, then the overlay's child controls from the frame's HUD info
## record. A dedicated host has no player of ours: its frame is the status page.
func tick() -> void:
	if _world == null or not _world.is_loaded():
		return
	var sim: Simulation = _world.get_sim()
	if sim != null and not sim.has_local_player() \
			and sim.session_role() == Simulation.ROLE_DEDICATED_HOST:
		ensure_game_hud()
		_session.dedicated_host_frame()
		_apply_work()
		return
	if sim == null or not sim.has_local_player():
		if not _warned_no_player:
			_warned_no_player = true
			push_warning("GameHud: world loaded but has no local player — the in-game HUD will not appear (net spectator, or the mission was not loaded as playable).")
		return
	ensure_game_hud()
	var info: HudEntityInfo = _session.frame(ControlsBindings.model(), _frame_stats)
	_apply_work()
	if info == null:
		return
	if _session.is_weapon_changed() and _sights_card != null:
		var weapon: PlayerHudWeaponDef = _session.get_weapon()
		var sights: Array[WeaponSightRow] = weapon.sights if weapon != null else []
		_push_sight_state()
		_sights_card.set_weapon_sights(sights,
				_world.get_resource_root() if _world != null else null,
				_session_texcompression_level())
	if _inset_scope != null:
		_inset_scope.update_view(_player_presenter.presented_view() \
				if _player_presenter != null else _world.local_player_view(),
				_game_hud.get_viewport().get_camera_3d(), _aspect_mode)
	_sync_second_scene_camera()
	# The SIGHTS card (over the frame, or drawn into the NVG scene), the scoped-view circle mask
	# and the view effects, as the info record decides them.
	if _sights_card != null:
		_sights_card.set_card_up(info.sights_card_up)
		_publish_nvg_scene_card(info.nvg_sights_in_scene)
	if _scope_circle_mask != null:
		_scope_circle_mask.set_mask_state(info.scope_mask_up, info.scope_mask_draw_crosshair,
				info.scope_mask_nvg_lens)
	if _view_effects != null:
		_view_effects.update_view(info.binoculars_view_active, info.binocular_range,
				info.nvg_mask, info.nvg_gain)


# The device work the session's toggle events named: the CMAP screen's open, the SIGHTS card's
# rows after a sight-scale step.
func _apply_work() -> void:
	var work := _session.take_work()
	if work & HudSession.WORK_SIGHT_STATE:
		_push_sight_state()
	if work & HudSession.WORK_COMMAND_MAP:
		command_map_requested.emit()


## The mission's HudOverlay node: null until the load's overlay init builds it, on the HUD layer
## from the first frame the mission has a local player.
func get_game_hud() -> HudOverlay:
	return _game_hud


## The world's drained mission effects, routed by the session (queued: PreMission effects can
## arrive before the HUD and its mission table exist).
func apply_mission_effects(effects: Array) -> void:
	_session.apply_mission_effects(effects)


## The latest player-facing mission text (the ADR 0018 read seam).
func hud_objective_line() -> String:
	return _session.get_objective_line()


## The stored end-of-round banner (the WAC Lose cause line), for the end screen.
func endround_banner_line() -> String:
	return _session.get_banner_line()


## Whether a chat line is open: every key belongs to the editor then.
func is_chat_capturing() -> bool:
	return _session.is_chat_capturing()


## One key event while the chat line is open (true = the editor took it).
func handle_chat_key(key: InputEventKey) -> bool:
	return _session.chat_key(key)


## One key-down through the special-key chain (engine inmatch::special_key_chain carries the
## chain, its order and its witness), over live play and in a pass-through screen's input frame,
## through the player presenter's input router. The device half of its outcome runs here: the
## quit dialog's Exit Mission (quit_confirmed), the page a PgUp / PgDn steps (the Tab board's, the
## F1 help's, the briefing's), and a taken key's consumption (ControlsModel.consume_key_press);
## the shell runs the rest. Null with no player presenter. The chain reads the key's one VK, the
## layout's (Godot's keycode, else the physical key), over `session` (null = the player
## presenter's world's).
func handle_special_key(key: InputEventKey, session: Simulation = null) -> SpecialKeyOutcome:
	if _player_presenter == null:
		return null
	var vk := ControlsModel.vk_from_godot_key(ArmoryPresenter.bound_key_of(key))
	var outcome: SpecialKeyOutcome = _player_presenter.special_key(session, vk)
	if outcome.is_quit_confirmed():
		quit_confirmed.emit()
	var forward := outcome.is_page_forward()
	var page := outcome.get_page()
	if page == SpecialKeyOutcome.PAGE_SCOREBOARD and _game_hud != null:
		# The chain's gate held (a session, the board up).
		_game_hud.scoreboard_page_key(forward, true, true)
	elif page == SpecialKeyOutcome.PAGE_HELP:
		ControlsBindings.model().cycle_help_page(forward)
	elif page == SpecialKeyOutcome.PAGE_BRIEFING and _game_hud != null:
		var sim: Simulation = _world.get_sim() if _world != null else null
		_game_hud.cycle_briefing_page(1 if forward else -1, sim != null and sim.is_mp_session())
	if outcome.is_taken():
		ControlsBindings.model().consume_key_press(vk)
	return outcome


## The escape key over live play: the HUD-window close chain closes the first open window (true =
## consumed); with none open it runs the respawn init and the shell opens the in-game menu.
func handle_escape() -> bool:
	var closed := _session.escape()
	_apply_work()
	return closed


## The SP end-of-round cine has started: its respawn init closes every HUD window.
func begin_end_of_round_cine() -> void:
	_session.begin_end_of_round_cine()
	_apply_work()


## The in-game menu's share of the single-player pause word (out of a session only).
func set_menu_pause(paused: bool) -> void:
	_session.set_menu_pause(paused)


## One hudcolor poll step over pre-sampled device state (the seam the tests drive); every other
## row idle.
func poll_hud_color_edge(color_down: bool, active: bool) -> void:
	_session.apply_toggle_events(_session.get_toggles().poll(
			(1 << HudToggles.ROW_HUD_COLOR) if color_down else 0, active, false, false))
	_apply_work()


func cycle_hud_color() -> void:
	_session.get_toggles().cycle_hud_color()
	_session.apply_toggle_events(HudToggles.EVENT_HUD_COLOR_CYCLED)


## One input frame's flipmouse and mousescale events with the mousescale fire's sign (the seam the
## tests drive), as the player input router dispatches the rows; every other row idle.
func dispatch_look_rows(flip_fired: bool, scale_fired: bool, negative: bool,
		active: bool) -> void:
	var rows_fired := ((1 << HudToggles.ROW_FLIP_MOUSE) if flip_fired else 0) \
			| ((1 << HudToggles.ROW_MOUSE_SCALE) if scale_fired else 0)
	_session.apply_toggle_events(_session.get_toggles().dispatch(rows_fired, active, false, false,
			true, false, false, negative))


## One input frame's huddetail event (the seam the tests drive); every other row idle.
func dispatch_hud_detail(fired: bool, active: bool) -> void:
	_session.apply_toggle_events(_session.get_toggles().dispatch(
			(1 << HudToggles.ROW_HUD_DETAIL) if fired else 0, active, false, false))


func cycle_hud_detail() -> void:
	_session.get_toggles().cycle_hud_detail()
	_session.set_hud_detail_level(_session.get_hud_detail_level())


## The one LIVE declutter-level write seam (the session's): it restamps a built HUD and never
## touches the persisted config value.
func set_hud_detail_level(level: int) -> void:
	_session.set_hud_detail_level(level)


## The mission-start apply: the live level re-seeded from the persisted config value.
func reapply_persisted_hud_detail() -> void:
	_session.reapply_persisted_hud_detail()


func hud_detail_level() -> int:
	return _session.get_hud_detail_level()


## The persisted config value (read at boot; the settings path owns writes).
func hud_detail_config() -> int:
	return _session.get_hud_detail_config()


## The persisted HUD color-scheme index (the token the colour cycle writes).
func hud_color_index() -> int:
	return _session.get_hud_color_index()


## The process-lifetime HUD toggle state (read seam: the window flags).
func toggles() -> HudToggles:
	return _session.get_toggles()


## The process-lifetime friendly-tags mode.
func friendly_tag_mode() -> int:
	return _session.get_friendly_tag_mode()


## Temporarily apply retail's blank HUD declutter level around a capture. The overlay, its
## PlayerViewEffects child, and the shell HUD CanvasLayer stay mounted and active; only compiled
## gameplay HUD commands are decluttered. Pair with finish_hud_hidden_capture().
func begin_hud_hidden_capture() -> Error:
	if _session.is_hidden_capture_active():
		return ERR_BUSY
	if _game_hud == null or not is_instance_valid(_game_hud) \
			or _view_effects == null or not is_instance_valid(_view_effects) \
			or _sights_card == null or not is_instance_valid(_sights_card):
		return ERR_UNCONFIGURED
	_session.begin_hidden_capture()
	return OK


## Idempotent capture cleanup: restore the exact live level that was active before the capture.
func finish_hud_hidden_capture() -> void:
	_session.finish_hidden_capture()


## Semantic presentation witness for a HUD-hidden capture. This compiles the live overlay and
## observes every gameplay draw family instead of inferring hidden state from the requested
## declutter number.
func hud_hidden_capture_witness() -> HudHiddenCaptureWitness:
	var witness := HudHiddenCaptureWitness.new()
	if not _session.is_hidden_capture_active():
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


## The 0x0F's death-screen leg forces the declutter level to max (the engine's rule).
func apply_death_screen_hud_detail() -> void:
	_session.apply_death_screen_hud_detail()


## The card rows the NVG Sighted arm draws into the scene, re-published every frame the arm is up
## (the rows follow the sight-scale and slide state) and cleared once when it drops.
func _publish_nvg_scene_card(up: bool) -> void:
	if not up and not _nvg_scene_card_published:
		return
	var frame_fx: FrameFx = _world.get_frame_fx() if _world != null else null
	if frame_fx == null or _game_hud == null:
		return
	_sights_card.publish_nvg_scene_rows(frame_fx, _game_hud.get_viewport_rect().size,
			_aspect_mode, up)
	_nvg_scene_card_published = up


## Live sight-scale and zero/rangefinder slide state from the equipped slot.
func _push_sight_state() -> void:
	if _sights_card == null or _game_hud == null:
		return
	_sights_card.set_sight_state(_game_hud.get_sight_scale_index(),
			_session.get_sight_slide_multiplier())


## Number of player-facing messages waiting for the lazy HUD to mount (the ADR 0018 read seam).
func pending_hud_message_count() -> int:
	return _session.get_pending_message_count()


## How many of those pending messages are CHAT ring lines (the same seam).
func pending_chat_line_count() -> int:
	return _session.get_pending_chat_line_count()
