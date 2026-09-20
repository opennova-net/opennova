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

# The ACCEPT hotkey: the WEAPON screen's on-show registers the USE-ITEM binding
# row's runtime keys (retail default: the Shifts — the same row 177 the shells'
# open key mirrors) as ACCEPT accelerators on the ACCEPT control
# [orig: UI_InitTeamClassSelection @0x567370 finds control "ACCEPT" (@0x7C7650)
#  and adds word_81A468/g_useItemBindingKey1 @0x5674a8/@0x5674c0].
const ACCEPT_HOTKEY := KEY_SHIFT

signal opened
signal closed

var _view: ArmoryWorldView = null
var _player_presenter: LocalPlayerPresenter = null  # viewmodel rebuild on ACCEPT
var _ui_parent: Node = null
# The layout source, converted ONCE at setup: a Control parent (test overlays)
# drives the fit from its own size/resized; a CanvasLayer parent (the game HUD)
# has no size, so the fit follows the viewport instead.
var _layout_control: Control = null
var _team := 0
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
	_armory.armory_closed.connect(close)


## Wire the presenter to its world view + player presenter and the node the menu
## overlays (the HUD layer in the game shell; tests pass their own Control parent).
func setup(view: ArmoryWorldView, player_presenter_in: LocalPlayerPresenter,
		ui_parent: Node) -> void:
	_view = view
	_player_presenter = player_presenter_in
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


func set_player_team(team: int) -> void:
	_team = team
	_armory.set_player_team(team)


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## The live menu driver over the armory frame (ADR 0018 read seam for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


## The armory key: open weapon.mnu's WEAPON screen over LIVE play when the player
## stands in an armory zone. Returns false when out of zone or the menu cannot
## build (the key is then ignored, matching the original's silent gate).
func try_open() -> bool:
	if _view == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null or not sim.local_player_in_armory_zone():
		return false  # [orig: Flags & 0x400000 gate @0x4e0b4d]
	return open()


## The post-zone-gate open leg: build + populate the WEAPON screen over live
## play. The shells arrive through try_open()'s zone gate; tests that stage the
## world without a type-6 volume drive this directly.
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null:
		return false
	# MP is live: a joiner's ACCEPT re-submits C2S 0x2F from the applied kit (the
	# sim queues it — apply_local_player_loadout's in-match leg), the listen host's
	# apply is server-authoritative in-process. The client-side S2C 0x5A grant IS
	# applied (D-NET-170 fixed): Simulation::apply_joiner_authoritative_loadout
	# folds the server's availability/class-filtered slots into the local ones at the
	# joiner's recv-before-actions boundary. What stays a tracked divergence is this
	# screen's OPTIMISTIC local refill on ACCEPT, ahead of the grant — retail resets
	# the slots and waits for the echo.
	# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 is_in_session leg @0x565d94]
	if not _ensure_menu():
		return false
	# The on-show protocol: the screen re-resolves the class and repopulates every
	# open [orig: the WEAPON activate handler @0x567370 -> populate @0x566db0].
	# Re-read the spawned entity on every show. Entity teams use 1/3=blue and
	# 2/4=red; the menu filter uses the profile-side 0=blue, 1=red domain.
	var entity_team := int(sim.get_local_player_team())
	if entity_team == 1 or entity_team == 3:
		_team = 0
	elif entity_team == 2 or entity_team == 4:
		_team = 1
	_player_class = int(sim.get_local_player_class())
	_armory.set_player_team(_team)
	_armory.set_class_allow_mask(int(sim.get_class_allow_mask()))
	_armory.set_player_class(_player_class)
	# D-MNU-10 (deliberate divergence, user decision 2026-07-11): the class spin is
	# LIVE in offline play. Retail enables it only in a network session
	# [orig: UI_InitTeamClassSelection @0x567370 — the is_in_session branch;
	# UI_OpenWeaponScreenSinglePlayer @0x424390 exists because SP has no session,
	# and retail SP pins the class]. Our runtime is a listen session even offline
	# (ADR 0009), and the SP loadout flow wants the choice.
	_armory.set_class_selection_enabled(true)
	var fallback_primary := String(sim.get_local_player_weapon_name())
	# Retail resolves each visible parent tuple from the selected class's canonical
	# buffer and routes it by that parent's weapon_class. It never scans the expanded
	# runtime slot pool, whose hidden subclasses can occupy a different class.
	# [orig: g_armoryLoadoutBufferByClass -> populate_ammo_type_combo_boxes
	# @0x564930; name/catalog resolve @0x564A00; slot route @0x564B47]
	var current_primary := fallback_primary
	var current_secondary := ""
	var current_accessory := ""
	var current_grenades: Array = []
	var current_parent_clips := {
		"PRIMARY": -1,
		"SECONDARY": -1,
		"ACCESSORY": -1,
	}
	var weapon_db: WeaponDatabase = _view.weapon_database()
	if weapon_db != null and weapon_db.is_loaded():
		current_primary = ""
		for row: WeaponKitEntry in sim.get_local_player_loadout():
			var weapon_name := row.name
			var index: int = weapon_db.find_weapon(weapon_name)
			if index < 0:
				continue
			match weapon_db.get_weapon(index).slot:
				WeaponDatabase.SLOT_PRIMARY:
					if current_primary.is_empty():
						current_primary = weapon_name
						current_parent_clips["PRIMARY"] = row.ammo_primary
				WeaponDatabase.SLOT_SECONDARY:
					if current_secondary.is_empty():
						current_secondary = weapon_name
						current_parent_clips["SECONDARY"] = row.ammo_primary
				WeaponDatabase.SLOT_ACCESSORY:
					if current_accessory.is_empty():
						current_accessory = weapon_name
						current_parent_clips["ACCESSORY"] = row.ammo_primary
				WeaponDatabase.SLOT_GRENADE:
					# The companion's grenade rows keep the persisted-profile shape
					# (the loadout profile's "grenades" entries).
					current_grenades.append({
						"name": row.name,
						"ammo_primary": row.ammo_primary,
						"ammo_secondary": row.ammo_secondary,
						"flags": row.flags,
					})
	_armory.set_current_loadout(
			current_primary, current_secondary, current_accessory, current_grenades,
			current_parent_clips)
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
	set_process(false)
	closed.emit()


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The blink/marquee clock rides the OS tick like the original's GetTickCount
	# gate (the shell does the same for the front-end menus).
	_driver.tick(Time.get_ticks_msec())


# Route the armory-key edges to the companion's debounced ACCEPT accelerator
# while the screen is open (the companion owns the armed state — its
# on_menu_built stamp is the original's open-debounce [orig: @0x4e0b21]).
func _unhandled_key_input(event: InputEvent) -> void:
	if not is_open():
		return
	var key := event as InputEventKey
	if key == null or key.keycode != ACCEPT_HOTKEY or key.echo:
		return
	if _armory.accept_hotkey_edge(key.pressed):
		get_viewport().set_input_as_handled()


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
	if root == null:
		return false
	var doc := MenuFrameSurface.load_document(root, MENU_FILE, "ArmoryPresenter")
	if doc == null:
		return false
	_register_text_tables(root)
	var surface := MenuFrameSurface.build(root, _ui_parent, _layout_control,
			"ArmoryMenu", _on_frame_gui_input)
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	if not MenuFrameSurface.open_document(_driver, doc, root, MENU_FILE, MENU_SCREEN,
			"ArmoryPresenter"):
		teardown()
		return false
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


# Armory ACCEPT: the full multi-slot kit (primary/secondary/accessory/grenades + clip
# requests) rebuilds the sim's slot pool and becomes the respawn kit; the sim's
# commit event then reinstalls the FP viewmodel/FSM around the re-selected equipped
# weapon. SP-local apply — the MP client path rides the 0x2F/0x5A loadout service.
# [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 -> the tuple parse + sub-weapon
#  expansion + WeaponSlot rebuild chain + Player_SelectWeaponSlot @0x4dd680 /
#  Player_MountWeaponSlot @0x4dfa40]
func _on_loadout_accepted(loadout: Dictionary) -> void:
	var primary := String(loadout.get("primary", ""))
	_player_class = int(loadout.get("player_class", _player_class))
	if _view != null:
		var sim: Simulation = _view.sim()
		var kit: Array[WeaponKitEntry] = []
		for slot_key in ["primary", "secondary", "accessory"]:
			var weapon_name := String(loadout.get(slot_key, ""))
			if weapon_name.is_empty():
				continue
			kit.append(WeaponKitEntry.make(weapon_name,
					int(loadout.get(slot_key + "_clips", -1))))
		for value in loadout.get("grenades", []):
			var grenade := value as Dictionary
			var weapon_name := String(grenade.get("name", ""))
			if weapon_name.is_empty():
				continue
			kit.append(WeaponKitEntry.make(weapon_name,
					int(grenade.get("ammo_primary", -1)),
					int(grenade.get("ammo_secondary", -1)),
					int(grenade.get("flags", -1))))
		var applied := false
		if sim != null:
			applied = bool(sim.apply_local_player_loadout(
					kit, int(loadout.get("player_class", 0))))
		if not applied:
			close()
			return
		if kit.is_empty():
			# The all-NONE kit: no slots, nothing equipped [orig: an empty buffer
			# leaves the table bare; the knife fallback is the MISSION loader's rule,
			# not the armory's].
			_view.clear_local_player_weapon()
			if _player_presenter != null:
				_player_presenter.refresh_viewmodel()
			close()
			return
		# The sim re-selected + committed the equipped slot during the apply; install
		# the viewmodel for it now (the commit event would also catch up next tick).
		var equipped := primary
		if sim != null:
			var equipped_name := sim.get_local_player_inventory().equipped_name
			if not equipped_name.is_empty():
				equipped = equipped_name
		if not equipped.is_empty() \
				and _view.set_local_player_weapon_by_name(equipped) \
				and _player_presenter != null:
			_player_presenter.refresh_viewmodel()
	close()


# The armory's text lookups (WepDes weapon names, CHARCLASS_* rows, TOTAL_WEIGHT)
# ride the shared Strings registry; the game shell registers these at boot,
# while direct/headless world owners may not — fill only the missing tables.
# [orig: Game_InitSubsystems @0x4a6cd0 loads menutxt/gametext at boot]
func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMETEXT, "gametext.bin"],
			[Strings.TABLE_GAMEUI, "Game.bin"]]:
		Strings.register_table(spec[0], Strings.load_rtxt(root, spec[1]))


func _recompute_fit() -> void:
	# MenuFrameSurface.fit_frame (shared with the other presenters).
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
