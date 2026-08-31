## The shell's menu front-end + resource-dir mount flow, split out of
## main_game.gd for the shipping-script size ratchet (the
## debug_controls_weather_rows.gd precedent): a method annex over MainGame's
## OWN state — every var stays on the shell and this object carries only the
## bodies, so the input callbacks and the GameShellSeams-bound surface stay on
## MainGame untouched.
extends RefCounted

const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")

var _shell: MainGame


func _init(shell: MainGame) -> void:
	_shell = shell


func is_picker_open() -> bool:
	return _shell._picker != null


# Returns false when the directory would not mount (the picker is raised and
# the shell holds no root) so boot continuations can gate on it.
func enter_menu(dir: String) -> bool:
	if _shell._root == null or _shell._root.get_root_dir() != dir:
		var root := BootRootMount.mount(dir, LaunchFlags.boot_loose_allowed(dir))
		if root == null:
			request_resource_dir()
			return false
		_shell._root = root
	var profile_root_key := "%s|%s" % [String(_shell._root.get_root_dir()),
			String(_shell._root.get_expansion()).to_lower()]
	if profile_root_key != _shell._profile_root_key:
		_shell._chosen_avatar = PlayerProfile.load_character_profile(_shell._root)
		_shell._profile_root_key = profile_root_key
	# The menu, loading screen, and world are one runtime resource session.
	# GameWorld must not remount from mutable persisted settings after boot.
	_shell._world.set_resource_root(_shell._root)
	_shell._state = MainGame.State.MENU
	_shell._shell_presentation.enter_menu(_shell._world, _shell._hud)
	wire_shell()
	if _shell._player_info_companion != null:
		_shell._player_info_companion.set_persisted_profile(_shell._chosen_avatar)
	if not _shell._menu_shell.setup(_shell._root):
		push_warning("MainGame: no menu found in resource dir (looked for %s)"
				% _shell._menu_shell.main_menu_file)
	_shell._menu_shell.show_menu()
	return true


func wire_shell() -> void:
	if _shell._shell_wired:
		return
	_shell._shell_wired = true
	_shell._menu_shell.start_requested.connect(_shell._on_start_requested)
	_shell._menu_shell.exit_to_desktop_requested.connect(_shell._on_exit_to_desktop)
	_shell._menu_shell.return_to_menu_requested.connect(_shell._on_return_to_menu)
	_shell._menu_shell.resume_requested.connect(_shell._on_resume)
	_shell._menu_shell.novaworld_requested.connect(_shell._net.open_novaworld_panel)
	# Delegate mp.mnu and player.mnu to their respective companions.
	_shell._mp_companion = MpMenuCompanion.new()
	_shell._player_info_companion = PlayerInfoMenuCompanion.new()
	_shell._lan_session = LanSession.new()
	_shell._lan_session.name = "LanSession"
	_shell.add_child(_shell._lan_session)
	_shell._mp_companion.set_lan_session(_shell._lan_session)
	_shell._menu_shell.add_companion(_shell._mp_companion)
	_shell._menu_shell.add_companion(_shell._player_info_companion)
	_shell._net.wire_menu_companions(_shell._mp_companion)
	_shell._player_info_companion.set_persisted_profile(_shell._chosen_avatar)
	_shell._player_info_companion.avatar_chosen.connect(_on_avatar_chosen)


# PLAYER_INFO ACCEPT persists both side records and the shared callsign, while
# the selected loadout continues through the existing spawn-kit seam.
func _on_avatar_chosen(profile: Dictionary) -> void:
	_shell.set_local_player_profile(profile)
	var typed_name := String(profile.get("name", "")).strip_edges()
	if not typed_name.is_empty():
		PlayerProfile.save_callsign(typed_name)
	if _shell._root != null:
		var save_error := PlayerProfile.save_character_profile(_shell._root, profile)
		if save_error != OK:
			push_warning("MainGame: could not save PLAYER_INFO profile (error %d)"
					% save_error)


func request_resource_dir() -> void:
	if DisplayServer.get_name() == "headless" or _shell._picker != null:
		return
	var picker := FileDialog.new()
	picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	picker.access = FileDialog.ACCESS_FILESYSTEM
	picker.use_native_dialog = true
	picker.title = "Select your OpenNova asset directory"
	picker.dir_selected.connect(_on_dir_selected)
	picker.canceled.connect(_on_dir_canceled)
	_shell._picker = picker
	_shell.add_child(picker)
	picker.popup_centered_ratio(0.6)


func _on_dir_selected(dir: String) -> void:
	cleanup_picker()
	apply_picked_resource_dir(dir, not LaunchFlags.resource_dir().is_empty())


## The picker's accept leg. `process_local` is resolved from --resource-dir at
## the signal callback above: an ONED-selected directory is process-local,
## so persisting a picker escape would overwrite the game's saved preference.
## Parameterized for
## the same ADR-0018 reason as BootRootMount.mount; returns false when the pick
## would not mount (the picker is re-raised).
func apply_picked_resource_dir(dir: String, process_local: bool) -> bool:
	var root := BootRootMount.mount(dir, LaunchFlags.boot_loose_allowed(dir))
	if root == null:
		request_resource_dir()
		return false
	_shell._root = root
	if not process_local:
		ResourceDirSettings.set_resource_dir(dir)
	enter_menu(dir)
	return true


func _on_dir_canceled() -> void:
	cleanup_picker()
	request_resource_dir()


func cleanup_picker() -> void:
	if _shell._picker != null:
		_shell._picker.queue_free()
		_shell._picker = null
