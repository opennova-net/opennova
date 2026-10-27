class_name ArmoryPresenter
extends Node

## The in-world armory surface mounted by the game shell. Kept as a standalone
## presenter so every runtime entry uses one implementation.
##
## Owns a live compiled menu surface — a MenuFrame orchestrated by a MenuDriver —
## over the gameplay view showing weapon.mnu's WEAPON screen, driven by the
## ArmoryMenuCompanion companion, zone-gated on the type-6
## armory volume contact flag the collision resolver maintains. The world keeps
## ticking underneath — the witnessed armory has no world-stop leg (the screen is
## a live overlay; in an MP session the team scoreboard even draws over it
## [orig: Render_ProcessMainSceneFrame @0x5cae1c]).
## [orig: the USE-ITEM key (action 177 "useitem", default SHIFT) ->
##  UI_OpenMenuScreen("weapon.mnu", "WEAPON") — Input_HandleActionBinding_0 case
##  0xB1 @0x4e0b3f, gated on entity Flags & 0x400000; ACCEPT applies
##  WeaponLoadout_ApplyFromBuffer @0x565cd0 -> the WeaponSlot rebuild chain +
##  Player_MountWeaponSlot @0x4dfa40]

const MENU_FILE := "weapon.mnu"
const MENU_SCREEN := "WEAPON"

# The ACCEPT hotkeys: the WEAPON screen's on-show registers the USE-ITEM binding
# row's two runtime keys (retail default: the Shifts — the same row 177 the
# shells' open key reads, so a rebound Use Item opens AND accepts) as ACCEPT
# accelerators on the ACCEPT control, and the open debounce clears on either
# key's release [orig: UI_InitTeamClassSelection @0x567370 finds control "ACCEPT"
# (@0x7C7650) and adds g_UseItemBindingKey0/g_UseItemBindingKey1
# @0x5674a8/@0x5674c0; Input_HandleMenuKeyRelease @0x4de2d0 tests the same two].
static func is_accept_hotkey(keycode: Key) -> bool:
	return ControlsBindings.model().godot_keys_for_token("useitem").has(int(keycode))


# The key a binding row matches an event by: the layout's VK (Godot's keycode), as
# the polled rows read it (Input.is_key_pressed, ControlsModel::vk_down_); a
# synthetic event that carries none falls back to its physical_keycode.
static func bound_key_of(key: InputEventKey) -> Key:
	return key.keycode if key.keycode != KEY_NONE else key.physical_keycode

signal opened
signal closed

var _view: ArmoryWorldView = null
var _player_presenter: LocalPlayerPresenter = null  # viewmodel rebuild on ACCEPT
var _ui_parent: Node = null
# The layout source, converted ONCE at setup: a Control parent (test overlays)
# drives the fit from its own size/resized; a CanvasLayer parent (the game HUD)
# has no size, so the fit follows the viewport instead.
var _layout_control: Control = null
# The profile store the screen's class pages stage from (its current weapon.sav
# record); null stages empty pages (engine menu::ArmoryLoadoutBuffers).
var _profiles: PlayerProfiles = null
# The local player's class (5..9; 0 = unclassed SP spawn); the screen opens on it
# [orig: entity playerClass feeds Armory_ResolveSelectedClass @0x5642f0]. Stamped
# by the armory ACCEPT until the spawn path carries a class of its own.
var _player_class := 0

# The compiled menu surface: the frame draws + hit-tests, the audio node plays
# widget SFX, and the RefCounted driver orchestrates both over the parsed
# document (freed with the presenter; only the two nodes need queue_free).
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _menu_root: ResourceRoot = null  # the root the built menu was fed from
var _armory := ArmoryMenuCompanion.new()


func _init() -> void:
	_armory.loadout_accepted.connect(_on_loadout_accepted)
	_armory.armory_closed.connect(_on_cancel)


## Wire the presenter to its world view + player presenter and the node the menu
## overlays (the HUD layer in the game shell; tests pass their own Control parent).
func setup(view: ArmoryWorldView, player_presenter_in: LocalPlayerPresenter,
		ui_parent: Node) -> void:
	_view = view
	_player_presenter = player_presenter_in
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


func set_player_profiles(profiles: PlayerProfiles) -> void:
	_profiles = profiles


## The mission start's stage of the screen's class pages from the profile and the spawn
## buffer, which every playing process runs (a dedicated host has no presenter); the
## pages, counts and bytes otherwise outlive the mission (engine
## menu::ArmoryLoadoutBuffers::stage carries the witness).
func on_mission_started() -> void:
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null:
		return
	_armory.get_loadout().stage(_profiles, int(sim.get_local_player_team()),
			sim.get_local_player_loadout())


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## The live menu driver over the armory frame (ADR 0018 read seam for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


# --- MCP menu-driving seam (the game_menu tool while the armory is up) -----------
# MenuShell's seam shape over this surface (MenuFrameSurface's ops); a closed armory
# answers nothing.


func menu_snapshot(include_widgets: bool = true) -> Dictionary:
	if not is_open():
		return {}
	return MenuFrameSurface.menu_state(_driver, _frame, {
		"visible": true,
		"surface": "armory",
		"file": MENU_FILE,
		"screen": _driver.get_current_screen(),
		"screens": _driver.get_screen_names(),
		"in_game": true,
		"stack_depth": _driver.get_screen_history_depth(),
	}, include_widgets)


func menu_press(widget_name: String) -> bool:
	return is_open() and MenuFrameSurface.press_widget(_driver, _frame, widget_name)


func menu_move_at(design_pos: Vector2, down: bool) -> int:
	return MenuFrameSurface.pump_at(_driver, _frame, design_pos, down) if is_open() else -1


func menu_press_at(design_pos: Vector2) -> int:
	return MenuFrameSurface.press_at(_driver, _frame, design_pos) if is_open() else -1


func menu_click_at(design_pos: Vector2) -> bool:
	if not is_open():
		return false
	MenuFrameSurface.click_at(_frame, design_pos)
	return true


# One key press as the screen takes it: the useitem key is the ACCEPT accelerator,
# the press latch permitting (_unhandled_key_input); the screen takes no other key.
func menu_key(keycode: int, unicode: int = 0) -> bool:
	if not is_open():
		return false
	var ev := InputEventKey.new()
	ev.keycode = keycode as Key
	ev.physical_keycode = keycode as Key
	ev.unicode = unicode
	ev.pressed = true
	if not is_accept_hotkey(bound_key_of(ev)) or not can_dismiss():
		return false
	_armory.trigger_accept()
	return true


## Whether the WEAPON screen's ACCEPT and CANCEL act: neither does while the
## engine's armory press latch is held (Simulation.is_armory_press_latched;
## engine LocalPlayer::armory_press_latched carries the witness).
func can_dismiss() -> bool:
	var sim: Simulation = _view.sim() if _view != null else null
	return sim == null or not sim.is_armory_press_latched()


## Build + populate the WEAPON screen over live play. The engine decides when
## (the held USE row's armory arm, engine controls::PlayerActions and
## world::LocalPlayer::take_use_item_press); the shell and the tests call this.
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null:
		return false
	# MP is live: in a session the ACCEPT sends one C2S 0x2F and applies nothing
	# locally, a joiner's and the listen host's own client's alike (the sim's
	# apply_local_player_loadout session leg); the host's S2C 0x5A grant rebuilds
	# the slots (D-NET-170).
	# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 is_in_session leg @0x565d94]
	if not _ensure_menu():
		return false
	# The on-show protocol: the screen re-resolves the class, stages its class pages
	# under its team gate and repopulates from the selected class's page every open
	# [orig: the WEAPON activate handler @0x567370 -> populate @0x566db0]. Re-read the
	# spawned entity on every show: its team (1 / 3 blue, 2 / 4 red) is the stage's side
	# and the lists' team filter.
	_player_class = int(sim.get_local_player_class())
	_armory.set_entity_team(int(sim.get_local_player_team()))
	_armory.set_class_allow_mask(int(sim.get_class_allow_mask()))
	_armory.set_player_class(_player_class)
	# D-MNU-10 (deliberate divergence, user decision 2026-07-11): the class spin is
	# LIVE in offline play. Retail enables it only in a network session
	# [orig: UI_InitTeamClassSelection @0x567370 — the is_in_session branch;
	# UI_OpenWeaponScreenSinglePlayer @0x424390 exists because SP has no session,
	# and retail SP pins the class]. Our runtime is a listen session even offline
	# (ADR 0009), and the SP loadout flow wants the choice.
	_armory.set_class_selection_enabled(true)
	_armory.set_stage_sources(_profiles, sim.get_local_player_loadout())
	_armory.set_availability_lookup(
			func(weapon_name: String) -> int:
				return int(sim.get_weapon_availability(weapon_name)))
	_armory.on_menu_built(_driver, MENU_FILE, MENU_SCREEN, _menu_root)
	# The menu draws over every HUD element (the lazily built GameHud may have been
	# added after us) [orig: the UI scene renders after HUD_DrawGameplayOverlays in
	# Render_ProcessMainSceneFrame @0x5ca0f0].
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	set_process(true)
	opened.emit()
	return true


func close() -> void:
	if not is_open():
		return
	# A dropdown left open would come back mid-popup on the next open (the frame
	# only hides; the driver's per-widget state persists across shows).
	_driver.close_active_combo_popup()
	_armory.on_menu_released()
	_frame.visible = false
	# The menu's pointer is the menu's alone (MenuFrameSurface.hide_frame).
	Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)
	set_process(false)
	closed.emit()


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The menu's pump, once a frame after the input events (MenuFrameSurface).
	_driver.pump_mouse()
	# The blink/marquee clock rides the OS tick like the original's GetTickCount
	# gate (the shell does the same for the front-end menus).
	_driver.tick(_view.frame_clock_ms)


# A useitem key's press while the screen is open is the ACCEPT accelerator,
# unless the engine's armory press latch holds it: the press that opened the
# screen latched it, and only a useitem release clears it (MainGame._input;
# engine LocalPlayer::armory_press_latched carries the witness).
func _unhandled_key_input(event: InputEvent) -> void:
	if not is_open():
		return
	var key := event as InputEventKey
	if key == null or key.echo or not key.pressed or not is_accept_hotkey(bound_key_of(key)):
		return
	if can_dismiss():
		_armory.trigger_accept()
		get_viewport().set_input_as_handled()


# The screen's CANCEL: a close unless the press latch holds it open.
func _on_cancel() -> void:
	if can_dismiss():
		_reset_all_input_state()
		close()


# The ACCEPT's and CANCEL's head past the press latch: the input reset (engine
# inmatch/session.h Role::reset_all_input_state carries the witness).
func _reset_all_input_state() -> void:
	var sim: Simulation = _view.sim() if _view != null else null
	if sim != null:
		sim.reset_all_input_state()


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_menu_root = null


# Build the compiled menu surface over the gameplay view: the same
# MenuFrame + MenuAudio + MenuDriver stack the menu shell drives, fed weapon.mnu
# from the WORLD's mounted resource root, with the canonical stylesheet and the
# current root's menutxt/gametext tables. Direct/headless world owners may not
# have run the front-end text bootstrap.
func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "ArmoryMenu", "ArmoryPresenter", _on_frame_gui_input,
			func(_built: MenuDriver) -> void: _register_text_tables(root))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	_menu_root = root
	_armory.set_weapon_database(_view.weapon_database())
	return true


# The compiled frame is a passive surface; MenuFrameSurface.forward_gui_input
# forwards its gui input to the driver the way MenuShell does. The armory's
# lists also take the wheel.
func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)
	var button := event as InputEventMouseButton
	if button != null and button.pressed and (button.button_index == MOUSE_BUTTON_WHEEL_DOWN \
			or button.button_index == MOUSE_BUTTON_WHEEL_UP):
		# One notch = one row tick (D-MNU-18 deliberate divergence).
		if _driver.process_wheel(button.position,
				1 if button.button_index == MOUSE_BUTTON_WHEEL_DOWN else -1):
			_frame.accept_event()


# Armory ACCEPT: the class's page, the team's knife first (the companion's), rebuilds
# the sim's slot pool and becomes the respawn kit; the sim's commit event then
# reinstalls the FP viewmodel/FSM around the re-selected equipped weapon. SP-local
# apply — the MP client path rides the 0x2F/0x5A loadout service.
# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 -> the tuple parse + sub-weapon
#  expansion + WeaponSlot rebuild chain + Player_SelectWeaponSlot @0x4dd680 /
#  Player_MountWeaponSlot @0x4dfa40]
func _on_loadout_accepted(kit: Array[WeaponKitEntry], player_class: int) -> void:
	if not can_dismiss():
		return
	_reset_all_input_state()
	_player_class = player_class
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null or not bool(sim.apply_local_player_loadout(kit, player_class)):
		close()
		return
	# In a session the apply sent its 0x2F and selects nothing: the screen closes (the
	# apply's tail) and the host's grant mounts the kit (engine
	# inmatch/loadout_submit.h armory_accept_in_session carries the witness).
	if sim.is_mp_session():
		close()
		return
	# The apply's select committed and mounted the equipped slot in the sim, and its
	# mount request queued SWITCHRANK on that slot (engine world/player_loadout.cpp
	# select_current_slot carries the witness): present the viewmodel for it now, with
	# no second sim mount over that switch (the commit event would also catch up next
	# tick). A table the kit left bare equips nothing: no rendered/action weapon.
	var equipped := String(sim.get_local_player_inventory().equipped_name)
	if equipped.is_empty():
		_view.clear_local_player_weapon()
		if _player_presenter != null:
			_player_presenter.refresh_viewmodel()
	elif _view.present_local_player_weapon(equipped) and _player_presenter != null:
		_player_presenter.refresh_viewmodel()
	close()


# The armory's text lookups (WepDes weapon names, CHARCLASS_* rows, TOTAL_WEIGHT)
# ride the shared Strings registry. The game shell registers these at boot, but
# direct/headless world owners may not, so every menu build re-registers all
# three from the world's current root, replacing what the registry held; a table
# that root lacks is unregistered (register_table(null) erases it).
# [orig: Game_InitSubsystems @0x4a6cd0 loads menutxt/gametext at boot]
func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMETEXT, "gametext.bin"],
			[Strings.TABLE_GAMEUI, "Game.bin"]]:
		Strings.register_table(spec[0], Strings.load_rtxt(root, spec[1]))


func _recompute_fit() -> void:
	# MenuFrameSurface.fit_frame (shared with the other presenters).
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
