extends GutTest

# A discovered retail/OpenNova host carrying ServerHello.P2 spectator flags
# must stop before world loading or ClientAuth and ask for the join role.


func after_each() -> void:
	LaunchFlags.clear_args_override()


func _controller(panel_layer: Control) -> NetSessionController:
	var controller := NetSessionController.new()
	add_child_autofree(controller)
	controller.setup(null, null, null, panel_layer)
	return controller


func _target(flags: int) -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 32768
	target.server_name = "Spectator Host"
	target.server_flags = flags
	return target


func test_discovered_spectator_host_prompts_before_joining() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(
			JoinTarget.FLAG_ALLOW_SPECTATORS
			| JoinTarget.FLAG_SPECTATOR_PASSWORD)

	controller.join_lan_server(target)

	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt,
			"an enabled ServerHello.P2 opens the retail role decision")
	assert_false(target.role_explicit,
			"no join role is committed before the user chooses")
	if prompt == null:
		return
	var player := prompt.find_child("JoinAsPlayer", true, false) as Button
	var spectator := prompt.find_child("JoinAsSpectator", true, false) as Button
	var password := prompt.find_child("SpectatorPassword", true, false) as LineEdit
	assert_not_null(player)
	assert_not_null(spectator)
	assert_not_null(password,
			"P2 bit 0x4000 reveals the spectator-password field")
	if password != null:
		assert_true(password.secret)
		assert_eq(password.max_length, 17,
				"the prompt preserves retail's 17-character password limit")


# `--integrity-profile` reaches EVERY joiner entry (the NovaWorld browser's
# targets carry none), while an explicit target-set profile is never clobbered.
# The default stays empty = anti-cheat silence (D-NET-181).
func test_integrity_profile_flag_covers_browser_joins() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	LaunchFlags.set_args_override(PackedStringArray(
			["--integrity-profile", "retail-revx02-024f56f2-2d087374"]))
	var target := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)
	controller.join_lan_server(target)  # stops at the role prompt — no world load
	assert_eq(target.integrity_profile, "retail-revx02-024f56f2-2d087374",
			"a profile-less target inherits the launch-flag opt-in")

	var explicit := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)
	explicit.integrity_profile = "operator-pinned"
	controller.join_lan_server(explicit)
	assert_eq(explicit.integrity_profile, "operator-pinned",
			"an explicit target profile is never overwritten by the flag")

	LaunchFlags.clear_args_override()
	var unflagged := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)
	controller.join_lan_server(unflagged)
	assert_eq(unflagged.integrity_profile, "",
			"without the flag the joiner keeps the silent default")


func test_unprotected_spectator_host_omits_password_field_and_cancel_closes() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_ALLOW_SPECTATORS)

	controller.join_lan_server(target)

	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt)
	if prompt == null:
		return
	assert_null(prompt.find_child("SpectatorPassword", true, false))
	var cancel := prompt.find_child("CancelJoin", true, false) as Button
	assert_not_null(cancel)
	if cancel != null:
		cancel.pressed.emit()
	assert_false(target.role_explicit)
