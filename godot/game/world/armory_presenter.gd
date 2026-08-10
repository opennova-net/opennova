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
const STYLESHEET_FILE := "menu_style.mns"  # the canonical name (MenuShell's default)
const MUSIC_VAR_INDEX := 2

# The ACCEPT hotkey: the WEAPON screen's on-show registers the USE-ITEM binding
# row's runtime keys (retail default: the Shifts — the same row 177 the shells'
# open key mirrors) as ACCEPT accelerators on the ACCEPT control
# [orig: UI_InitTeamClassSelection @0x567370 finds control "ACCEPT" (@0x7C7650)
#  and adds word_81A468/word_81A46A @0x5674a8/@0x5674c0].
const ACCEPT_HOTKEY := KEY_SHIFT

signal opened
signal closed

var _world: GameWorld = null
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


## Wire the presenter to a world + player presenter and the node the menu overlays
## (the HUD layer in the game shell; tests pass their own Control parent).
func setup(world: GameWorld, player_presenter_in: LocalPlayerPresenter,
		ui_parent: Node) -> void:
	_world = world
	_player_presenter = player_presenter_in
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	_connect_layout_source()


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
	if _world == null:
		return false
	var sim: Simulation = _world.get_sim()
	if sim == null or not sim.local_player_in_armory_zone():
		return false  # [orig: Flags & 0x400000 gate @0x4e0b4d]
	return open()


## The post-zone-gate open leg: build + populate the WEAPON screen over live
## play. The shells arrive through try_open()'s zone gate; tests that stage the
## world without a type-6 volume drive this directly.
func open() -> bool:
	if is_open() or _world == null or _ui_parent == null:
		return false
	var sim: Simulation = _world.get_sim()
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
	var weapon_db: WeaponDatabase = _world.get_weapon_database()
	if weapon_db != null and weapon_db.is_loaded():
		current_primary = ""
		for value in sim.get_local_player_loadout():
			var row := value as Dictionary
			var weapon_name := String(row.get("name", ""))
			var index: int = weapon_db.find_weapon(weapon_name)
			if index < 0:
				continue
			match int(weapon_db.get_weapon(index).get("slot", -1)):
				WeaponDatabase.SLOT_PRIMARY:
					if current_primary.is_empty():
						current_primary = weapon_name
						current_parent_clips["PRIMARY"] = int(
								row.get("ammo_primary", -1))
				WeaponDatabase.SLOT_SECONDARY:
					if current_secondary.is_empty():
						current_secondary = weapon_name
						current_parent_clips["SECONDARY"] = int(
								row.get("ammo_primary", -1))
				WeaponDatabase.SLOT_ACCESSORY:
					if current_accessory.is_empty():
						current_accessory = weapon_name
						current_parent_clips["ACCESSORY"] = int(
								row.get("ammo_primary", -1))
				WeaponDatabase.SLOT_GRENADE:
					current_grenades.append(row.duplicate(true))
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
	var root: ResourceRoot = _world.get_resource_root()
	if root == null:
		return false
	var bytes := root.read_file(MENU_FILE)
	if bytes.is_empty():
		push_warning("ArmoryPresenter: %s not found in the resource root" % MENU_FILE)
		return false
	var doc := MnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		push_warning("ArmoryPresenter: %s did not parse" % MENU_FILE)
		return false
	_register_text_tables(root)
	# weapon.mnu shares the retail menu's fixed 800x600 design space and the
	# independent X/Y fill used by every front-end screen; the frame scales that
	# design space to its OWN size internally, so the fit just sizes the Control.
	# [orig: CUIScene_SetScreenScale @0x639480]
	_frame = MenuFrame.new()
	_frame.name = "ArmoryMenu"
	_frame.set_anchors_preset(Control.PRESET_TOP_LEFT)
	# Unlike MenuShell (a Control parent sampling for a full-rect child frame),
	# the presenter overlays a foreign HUD parent, so the frame itself is the
	# input surface: its gui_input forwards into the driver's pump.
	_frame.mouse_filter = Control.MOUSE_FILTER_STOP
	_frame.gui_input.connect(_on_frame_gui_input)
	_ui_parent.add_child(_frame)
	_recompute_fit()
	# Widget <SOUND> triggers play through the MenuAudio device leg.
	_audio = MenuAudio.new()
	_audio.name = "ArmoryMenuAudio"
	_audio.set_resource_root(root)
	_ui_parent.add_child(_audio)
	_driver = MenuDriver.new()
	_driver.attach(_frame, _audio)
	_driver.set_music_director(NovaMusicService.director())
	_driver.set_music_var_index(MUSIC_VAR_INDEX)
	var style := _load_style(root)
	var menu_text: RtxtStringFile = Strings.get_table("menutxt")
	if not _driver.open_document(doc, root, style, menu_text, MENU_FILE, MENU_SCREEN):
		push_warning("ArmoryPresenter: %s has no screens" % MENU_FILE)
		teardown()
		return false
	_menu_root = root
	_armory.set_weapon_database(_world.get_weapon_database())
	return true


# The compiled frame is a passive surface — it draws and hit-tests but never
# pumps input itself; forward its gui input to the driver the way MenuShell
# does (event positions are frame-local, the space process_mouse expects).
func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	if event is InputEventMouseMotion:
		var motion := event as InputEventMouseMotion
		_driver.process_mouse(motion.position,
				(motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0)
	elif event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_LEFT:
			_driver.process_mouse(button.position, button.pressed)
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
	if _world != null:
		var sim: Simulation = _world.get_sim()
		var kit: Array[Dictionary] = []
		for slot_key in ["primary", "secondary", "accessory"]:
			var weapon_name := String(loadout.get(slot_key, ""))
			if weapon_name.is_empty():
				continue
			kit.append({
				"name": weapon_name,
				"ammo_primary": int(loadout.get(slot_key + "_clips", -1)),
				"ammo_secondary": -1,
				"flags": -1,
			})
		for value in loadout.get("grenades", []):
			var grenade := value as Dictionary
			var weapon_name := String(grenade.get("name", ""))
			if weapon_name.is_empty():
				continue
			kit.append({
				"name": weapon_name,
				"ammo_primary": int(grenade.get("ammo_primary", -1)),
				"ammo_secondary": int(grenade.get("ammo_secondary", -1)),
				"flags": int(grenade.get("flags", -1)),
			})
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
			_world.clear_local_player_weapon()
			if _player_presenter != null:
				_player_presenter.refresh_viewmodel()
			close()
			return
		# The sim re-selected + committed the equipped slot during the apply; install
		# the viewmodel for it now (the commit event would also catch up next tick).
		var equipped := primary
		if sim != null:
			var inv: Dictionary = sim.get_local_player_inventory()
			var equipped_name := String(inv.get("equipped_name", ""))
			if not equipped_name.is_empty():
				equipped = equipped_name
		if not equipped.is_empty() \
				and _world.set_local_player_weapon_by_name(equipped) \
				and _player_presenter != null:
			_player_presenter.refresh_viewmodel()
	close()


# The armory's text lookups (WepDes weapon names, CHARCLASS_* rows, TOTAL_WEIGHT)
# ride the shared Strings registry; the game shell registers these at boot,
# while direct/headless world owners may not — fill only the missing tables.
# [orig: Game_InitSubsystems @0x4a6cd0 loads menutxt/gametext at boot]
func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [["menutxt", "menutxt.BIN"], ["gametext", "gametext.bin"],
			["gameui", "Game.bin"]]:
		var bytes := root.read_file(spec[1])
		var table: RtxtStringFile = null
		if not bytes.is_empty():
			var loaded := RtxtStringFile.new()
			if loaded.load_from_byte_array(bytes) == OK:
				table = loaded
		Strings.register_table(spec[0], table)


# The canonical menu stylesheet name the original engine looks for.
func _load_style(root: ResourceRoot) -> MnsStyleSheet:
	var bytes := root.read_file(STYLESHEET_FILE)
	if bytes.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_bytes(bytes) == OK and s.is_runtime_valid() else null


func _connect_layout_source() -> void:
	if _layout_control != null:
		if not _layout_control.resized.is_connected(_recompute_fit):
			_layout_control.resized.connect(_recompute_fit)
		return
	var viewport := _ui_parent.get_viewport() if _ui_parent != null else null
	if viewport != null and not viewport.size_changed.is_connected(_recompute_fit):
		viewport.size_changed.connect(_recompute_fit)


func _recompute_fit() -> void:
	if _frame == null or not is_instance_valid(_frame):
		return
	var target_size := Vector2.ZERO
	if _layout_control != null:
		target_size = _layout_control.size
	elif _ui_parent != null and _ui_parent.get_viewport() != null:
		target_size = _ui_parent.get_viewport().get_visible_rect().size
	if target_size.x <= 1.0 or target_size.y <= 1.0:
		return
	# The frame maps the 800x600 design space to its own rect internally
	# [orig: CUIScene_SetScreenScale @0x639480] — no Control scale math here.
	_frame.position = Vector2.ZERO
	_frame.size = target_size
