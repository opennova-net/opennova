class_name PlayerInfoMenuCompanion
extends MenuCompanion

# The device half of the JO PLAYER_INFO screen (player.mnu). The screen's
# controller is the engine's (PlayerInfoScreen over engine
# inmatch::PlayerInfoScreen, which carries the registrations, the populate,
# the cascades and the save, with their witnesses); the menu shell hands it
# every document its owner names ("player_info", MenuDriver.add_screen). This
# script gives the controller the tables it reads as each open begins
# (Avatars.def and weapon.def off the mounted root, the profile store, the text
# tables) and mounts what the screen asks for over the shared frame: the
# PLAYER_PREVIEW portrait and the weapon slot icons.

const AvatarPreviewScript := preload("res://game/avatar/avatar_preview.gd")

## The ACCEPT saved the dialog into the profile record: the selection for the
## next spawn (the profile Dictionary PlayerInfoScreen.snapshot describes).
signal avatar_chosen(profile: Dictionary)
## The PLAYER_PREVIEW portrait drew a frame: retail's draw stores 16:00 into the process's
## time of day, never restored, which the menus' remote admin reads (MainGame routes it to
## GameWorld.player_preview_drawn; the witness: engine inmatch::kPlayerPreviewTodFixed24).
signal player_preview_drawn

var _screen := PlayerInfoScreen.new()
var _db: AvatarDatabase
var _preview  # AvatarPreview mounted over PLAYER_PREVIEW (null until the screen opens)
var _preview_id := -1


func _init() -> void:
	_screen.opening.connect(_on_opening)
	_screen.opened.connect(_on_opened)
	_screen.closed.connect(_clear_mounts)
	_screen.preview_changed.connect(_refresh_preview)
	_screen.preview_hovered.connect(_on_preview_hovered)
	_screen.loadout_changed.connect(_update_icons)
	_screen.relayout.connect(_reposition_mounts)
	_screen.voice_requested.connect(_on_voice_requested)
	_screen.avatar_chosen.connect(avatar_chosen.emit)


## The engine controller the menu shell registers with its driver.
func screen() -> MenuScreenBinding:
	return _screen


## The driver the screen opens on and the root its tables are read from: the
## menu shell hands them over after its assembly and again when its root
## changes.
func attach(driver: MenuDriver, root: ResourceRoot) -> void:
	_driver = driver
	_root = root


## The active + per-side weapon.sav character snapshot the next open restores:
## the profile Dictionary (team, player_class, side_profiles [blue, red], each
## {team, nationality, division, combo, player_class, avatar_a, avatar_b,
## avatar_packed} or {} when unknown), the shape avatar_chosen hands back.
func set_persisted_profile(profile: Dictionary) -> void:
	var sides: Array[PackedInt32Array] = [PackedInt32Array(), PackedInt32Array()]
	var saved: Array = profile.get("side_profiles", [])
	for side in mini(saved.size(), 2):
		var row = saved[side]
		if row is Dictionary and not (row as Dictionary).is_empty():
			sides[side] = PackedInt32Array([int(row.get("team", -1)),
					int(row.get("nationality", -1)), int(row.get("division", -1)),
					int(row.get("combo", -1)),
					int(row.get("player_class", PlayerInfoScreen.DEFAULT_PLAYER_CLASS)),
					int(row.get("avatar_a", 0)), int(row.get("avatar_b", 0)),
					int(row.get("avatar_packed", 0))])
	_screen.set_persisted(profile.has("team"), int(profile.get("team", 0)),
			profile.has("player_class"), int(profile.get("player_class", 0)), sides[0], sides[1])


## Inject the avatar table directly (a unit without a resource root).
func set_database(db: AvatarDatabase) -> void:
	_db = db
	_screen.set_database(db)


## Inject a weapon table (tests, another shell-owned mount); null hands the load
## back to the root. The screen's recorded picks are keyed by the table's rows,
## so a swap drops them and an open screen fills its loadout again.
func set_weapon_database(weapons: WeaponDatabase) -> void:
	_set_weapons(weapons)
	_screen.set_weapon_database(weapons)


# --- The tables, as each open begins ---------------------------------------------

func _on_opening() -> void:
	_ensure_db()
	_screen.set_database(_db)
	_ensure_weapons("PlayerInfoMenuCompanion", "loadout combos stay empty")
	_screen.set_weapon_database(_weapons)
	_screen.set_profiles(PlayerProfile.store())
	_screen.set_text_tables(Strings.get_table(Strings.TABLE_GAMETEXT),
			Strings.get_table(Strings.TABLE_MENUTXT), Strings.get_table(Strings.TABLE_GAMEUI))
	_clear_mounts()


# Avatars.def off the root, best-effort: without it the lists stay empty.
func _ensure_db() -> void:
	if _db != null or _root == null:
		return
	_db = AvatarDatabase.new()
	if _db.load_from_resource_root(_root, "Avatars.def") != OK or not _db.is_loaded():
		push_warning("PlayerInfoMenuCompanion: Avatars.def not loaded (%s); avatar combos stay empty"
			% _db.get_last_error())
		_db = null


# --- The frame mounts (the portrait and the icons) --------------------------------

func _on_opened() -> void:
	var frame := _driver.get_frame() if _driver != null else null
	if frame != null and not frame.resized.is_connected(_reposition_mounts):
		frame.resized.connect(_reposition_mounts)
	_wire_preview()
	_update_icons()


# Mount the head/body 3D preview over the PLAYER_PREVIEW widget rect (a custom
# button surface in player.mnu) and feed it the current combo. A menu without the
# widget, or a driver without a frame, shows no preview.
func _wire_preview() -> void:
	var preview_id := _id("PLAYER_PREVIEW")
	var frame := _driver.get_frame() if _driver != null else null
	if preview_id < 0 or frame == null:
		return
	_preview_id = preview_id
	_preview = AvatarPreviewScript.new()
	_preview.name = "PlayerInfoAvatarPreview"
	_preview.drawn.connect(player_preview_drawn.emit)
	frame.add_child(_preview)
	_place_mount(_preview, preview_id)
	_preview.set_resource_root(_root)
	_refresh_preview()


func _refresh_preview() -> void:
	if _preview == null:
		return
	var combo := _screen.preview_combo()
	if combo != null:
		# The portrait stops at the resolved part geometry; D-PLAYERINFO-1
		# tracks the separate spawned-player model seam.
		_preview.load_combo(combo)


func _on_preview_hovered(hovered: bool) -> void:
	if _preview != null and is_instance_valid(_preview):
		_preview.set_hovered(hovered)


func _update_icons() -> void:
	_update_weapon_icons("LoadoutIcon", _selected_weapon)


func _selected_weapon(control: String) -> WeaponDef:
	return _weapons.get_weapon(_screen.selected_weapon(control)) if _weapons != null else null


func _on_voice_requested(trigger: String, bank: String) -> void:
	if _driver != null:
		_driver.play_widget_sound(trigger, bank)


func _clear_mounts() -> void:
	_clear_icon_mounts()
	if _preview != null and is_instance_valid(_preview):
		_preview.queue_free()
	_preview = null
	_preview_id = -1


func _reposition_mounts() -> void:
	# The persistent frame's resized signal can fire after another document
	# took the driver: ids of this one never place mounts against it.
	if _driver == null or not _screen.is_open():
		return
	_reposition_icon_mounts()
	if _preview != null and is_instance_valid(_preview) and _preview_id >= 0:
		_place_mount(_preview, _preview_id)
