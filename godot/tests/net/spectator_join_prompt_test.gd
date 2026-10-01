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


func test_server_password_prompt_is_independent_of_spectator_access() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_SERVER_PASSWORD)
	controller.join_lan_server(target)
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt)
	if prompt == null:
		return
	var password := prompt.find_child("ServerPassword", true, false) as LineEdit
	assert_not_null(password)
	if password != null:
		assert_true(password.secret)
	assert_null(prompt.find_child("SpectatorPassword", true, false))
	var spectator := prompt.find_child("JoinAsSpectator", true, false) as Button
	assert_false(spectator.visible)
	assert_false(target.role_explicit)


func test_both_passwords_have_distinct_controls() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_SERVER_PASSWORD
			| JoinTarget.FLAG_ALLOW_SPECTATORS | JoinTarget.FLAG_SPECTATOR_PASSWORD)
	controller.join_lan_server(target)
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt)
	if prompt != null:
		assert_not_null(prompt.find_child("ServerPassword", true, false))
		assert_not_null(prompt.find_child("SpectatorPassword", true, false))


func test_team_password_and_team_choice_prompt_before_admission() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_TEAM_CHOICE | JoinTarget.FLAG_BLUE_PASSWORD
			| JoinTarget.FLAG_RED_PASSWORD | JoinTarget.FLAG_SERVER_PASSWORD)
	target.join_password = "prefilled"
	target.team_request = 1
	controller.join_lan_server(target)
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt, "protected sides prompt even without spectator access")
	if prompt == null:
		return
	var password := prompt.find_child("JoinPassword", true, false) as LineEdit
	var choice := prompt.find_child("TeamChoice", true, false) as OptionButton
	assert_not_null(password)
	assert_not_null(choice)
	assert_not_null(prompt.find_child("ServerPassword", true, false))
	if password != null:
		assert_true(password.secret)
		assert_eq(password.text, "prefilled")
		assert_eq(password.max_length, 63)
		password.text = "unsubmitted"
	if choice != null:
		assert_eq(choice.item_count, 3)
		assert_eq(choice.selected, 2, "red is the second explicit side")
		choice.select(0)
	var cancel := prompt.find_child("CancelJoin", true, false) as Button
	cancel.pressed.emit()
	assert_eq(target.join_password, "prefilled", "cancel does not commit credentials")
	assert_eq(target.team_request, 1, "cancel does not change team preference")
	assert_false(target.role_explicit)


func test_team_choice_without_password_keeps_credentials_absent() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var target := _target(JoinTarget.FLAG_TEAM_CHOICE)
	controller.join_lan_server(target)
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt)
	if prompt != null:
		assert_not_null(prompt.find_child("TeamChoice", true, false))
		assert_null(prompt.find_child("JoinPassword", true, false))
	assert_eq(target.team_request, -1, "automatic is the default")


# The NovaWorld lobby session survives the panel at the in-match handoff
# (D-NET-220): the controller holds the panel's client until the joiner load
# takes it, and an abandoned join (the role prompt cancelled) leaves the play
# and frees it.
func test_abandoned_novaworld_join_frees_the_held_session() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	controller.open_novaworld_panel()
	var panel := layer.get_node_or_null("NovaWorldPanel") as NovaWorldPanel
	assert_not_null(panel, "the NovaWorld panel opens")
	if panel == null:
		return
	var client := panel.client_for_test()
	panel.join_in_match_requested.emit(_target(JoinTarget.FLAG_ALLOW_SPECTATORS))
	assert_eq(client.get_parent(), controller,
			"the panel's session moves to the controller instead of dying with the panel")
	var prompt := layer.get_node_or_null("JoinRolePrompt")
	assert_not_null(prompt, "the spectator host still asks for the join role")
	if prompt == null:
		return
	(prompt.find_child("CancelJoin", true, false) as Button).pressed.emit()
	await wait_frames(2)
	assert_false(is_instance_valid(client), "an abandoned join frees the held session")


# The post-mission route's NovaWorld half: only a match entered from the
# NovaWorld menu returns there with its session (the panel's adopt_client leg,
# novaworld_panel_test); a stray session after any other match is left.
func test_normal_exit_outside_novaworld_leaves_a_stray_session() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	var session := NovaWorldClient.new()
	controller.return_from_mission(session, false, "")
	assert_null(layer.get_node_or_null("NovaWorldPanel"),
			"no NovaWorld menu re-entry for a match not entered from it")
	await wait_frames(2)
	assert_false(is_instance_valid(session), "the stray session is stopped and freed")


func test_error_exit_shows_the_novaworld_error_dialog() -> void:
	var layer := Control.new()
	add_child_autofree(layer)
	var controller := _controller(layer)
	controller.return_from_mission(null, true, "The host closed the session.")
	var panel := layer.get_node_or_null("NovaWorldPanel") as NovaWorldPanel
	assert_not_null(panel, "an error exit shows the NovaWorld error dialog")
	if panel == null:
		return
	assert_eq(panel.current_screen(), NovaWorldPanel.Screen.MESSAGE)
	assert_eq(panel.message_text(), "The host closed the session.")
