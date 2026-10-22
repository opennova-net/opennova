extends GutTest

# join_screen over the REAL shell (WorldFixture.boot_shell, the front end in its
# menus). The shell's own join screen is opened over the authored pre.mnu
# (JoinScreenFixture): the shell fixture's archives carry no pre.mnu, and an open
# screen stays open when the shell opens it again. The cases:
# - the catalog contract;
# - a closed screen's read and refusals, and a join refused without an address
#   and outside the menus;
# - D-NET-167's RO drive: the shell's join choice for protected sides
#   (MainGame.begin_join_choice), the side password typed, then ACCEPT's commit
#   handing the three controls to the join;
# - the admin pass's A7 drive on the choice: ABORT_CANCEL refused there (its
#   window is hidden), BACK running the join's abort;
# - `join`: the real dial of a silent endpoint enumerates on the progress panel
#   (PRE_ENUMSESSION), and ABORT_CANCEL there abandons it back to the menus.

const TMP_DIR := "res://.godot/join_screen_probe_test"
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _config: TestFs.Snapshot
var _temp_dir := ""
var _shell: MainGame = null
# The screen registers the text tables it finds missing: each test puts back what
# was there.
var _menutxt: RtxtStringFile = null
var _gameerr: RtxtStringFile = null
var _committed: Array = []
var _aborts := 0


func before_each() -> void:
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	LaunchFlags.set_args_override(PackedStringArray([]))
	_menutxt = Strings.get_table(Strings.TABLE_MENUTXT)
	_gameerr = Strings.get_table(Strings.TABLE_GAMEERR)
	JoinScreenFixture.stage(self, TMP_DIR)
	_committed = []
	_aborts = 0


func after_each() -> void:
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	_config.restore()
	GameCfgStore.reset()
	Strings.register_table(Strings.TABLE_MENUTXT, _menutxt)
	Strings.register_table(Strings.TABLE_GAMEERR, _gameerr)
	_menutxt = null
	_gameerr = null


func after_all() -> void:
	JoinScreenFixture.unstage(TMP_DIR)


# The real shell in its menus.
func _boot() -> bool:
	_shell = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	if _shell == null:
		return false
	assert_eq(_shell.shell_state_name(), "menu")
	return true


# The shell's join screen, open over the authored pre.mnu.
func _open_screen() -> PreGameMenuPresenter:
	var screen := _shell.get_join_screen()
	assert_true(screen.open(PresenterFixture.root_over(self, TMP_DIR), _shell),
			"the shell's join screen opens over the authored pre.mnu")
	return screen


func _run(args: Dictionary) -> ProbeVerdict:
	var def := ProbeDef.definition("join_screen")
	var validated := def.validate_args(args)
	assert_true(validated.ok, str(validated.errors))
	var ctx := ProbeContext.new()
	ctx.name = def.name
	ctx.args = validated.values
	ctx.tree = get_tree()
	ctx.shell = _shell
	var verdict: ProbeVerdict = await def.load_probe().run(ctx)
	ctx.finish()
	return verdict


func _on_committed(game_password: String, spectate: bool, team_password: String) -> void:
	_committed.append([game_password, spectate, team_password])


func _on_abort() -> void:
	_aborts += 1


func test_the_catalog_registers_join_screen() -> void:
	var def := ProbeDef.definition("join_screen")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/net/"), "the net family")
	assert_false(def.needs_window, "a headless joiner can drive it")
	assert_false(def.needs_mission, "the join screen is up before any mission")
	var defaults := def.validate_args({})
	assert_true(defaults.ok, "every argument has a default")
	assert_eq(String(defaults.values["op"]), "state", "a bare run only reads")
	assert_false(def.validate_args({"op": "type"}).ok, "the ops are named")
	assert_false(def.validate_args({"op": "press", "control": "LAN_JOINGAME"}).ok,
			"only the join screen's controls")
	assert_false(def.validate_args({"until": "deploy"}).ok, "only the screen's states")
	assert_false(def.validate_args({"port": 65536}).ok)
	assert_false(def.validate_args({"wait_ms": 1.5}).ok, "wait_ms is whole milliseconds")
	assert_true(def.validate_args({"op": "set_text", "control": "TEAM_PASSWORD",
			"text": "side"}).ok)
	assert_true(def.validate_args({"op": "join", "address": "127.0.0.1", "port": 32768,
			"until": "team_password", "wait_ms": 60000}).ok)


# A closed screen reads closed and refuses every control; an `until` it never
# reaches fails at `wait_ms`; an op takes only its own controls; a join needs an
# address and the menus.
func test_a_closed_screen_refuses_and_a_join_needs_the_menus() -> void:
	if not await _boot():
		return
	var read := await _run({})
	assert_true(read.ok, read.summary)
	assert_false(bool(read.data["open"]))
	assert_eq(String(read.data["panel"]), "")
	assert_eq_deep(read.data["windows"], [])
	assert_eq(String(read.data["shell_state"]), "menu")
	var press := await _run({"op": "press", "control": "BACK"})
	assert_false(press.ok, "no screen to press on")
	var wait := await _run({"until": "open", "wait_ms": 50})
	assert_false(wait.ok, "the screen never opened")
	var mixed := await _run({"op": "set_text", "control": "ACCEPT"})
	assert_false(mixed.ok, "set_text types into an edit, never a button")
	var no_address := await _run({"op": "join"})
	assert_false(no_address.ok, "a join needs an address")
	assert_false(_shell.is_join_screen_open(), "the refused join started nothing")
	var loaded: bool = await WorldFixture.start_shell_mission(self, _shell)
	assert_true(loaded, "the minimal mission loads through the real front end")
	if not loaded:
		return
	var in_world := await _run({"op": "join", "address": "127.0.0.1", "port": 9})
	assert_false(in_world.ok, "a join starts from the menus")
	assert_eq(String(in_world.data["shell_state"]), "world")
	assert_false(_shell.is_join_screen_open(), "the refused join started nothing")


# D-NET-167's RO half: a host with protected sides opens the side password's
# panel on the shell's join choice; the probe reads it, types the password and
# presses ACCEPT, whose commit closes the screen and hands the typed text to the
# join (the TEAM_PASSWORD text is the side password: JoinTarget.apply_pre_game_choice).
func test_the_side_password_is_typed_and_accepted_on_the_shells_choice() -> void:
	if not await _boot():
		return
	_open_screen()
	var side_flags: int = (JoinTarget.FLAG_TEAM_CHOICE | JoinTarget.FLAG_BLUE_PASSWORD
			| JoinTarget.FLAG_RED_PASSWORD)
	assert_true(_shell.begin_join_choice(side_flags, _on_committed, _on_abort),
			"protected sides open the shell's join choice")
	var read := await _run({"until": "team_password", "wait_ms": 1000})
	assert_true(read.ok, read.summary)
	assert_true(bool(read.data["open"]))
	assert_eq_deep(read.data["windows"],
			["ABORTRETRY_WRAPPER", "TEAM_PASSWORD_WRAPPER", "TEAM_STATIC_WRAPPER"])
	assert_eq(String(read.data["team_password"]), "")
	assert_false(bool(read.data["spectate"]))

	var refused := await _run({"op": "set_text", "control": "GAME_PASSWORD",
			"text": "server"})
	assert_false(refused.ok, "GAME_PASSWORD's window is hidden on this panel")
	assert_eq(String(refused.data["game_password"]), "", "nothing was typed")

	var typed := await _run({"op": "set_text", "control": "TEAM_PASSWORD",
			"text": "sidepw"})
	assert_true(typed.ok, typed.summary)
	assert_eq(String(typed.data["team_password"]), "sidepw", "the edit holds the typed text")
	assert_eq(String(typed.data["panel"]), "team_password", "typing walks nothing")

	var accepted := await _run({"op": "press", "control": "ACCEPT", "until": "closed",
			"wait_ms": 1000})
	assert_true(accepted.ok, accepted.summary)
	assert_eq_deep(_committed, [["", false, "sidepw"]],
			"the commit hands the typed side password to the join")
	assert_eq(_aborts, 0)
	assert_false(bool(accepted.data["open"]), "the commit closes the join screen")


# The admin pass's A7 on the join choice: ABORT_CANCEL's window is hidden there,
# so the probe refuses it; BACK runs the join's abort and closes the screen.
func test_back_on_the_shells_choice_runs_the_joins_abort() -> void:
	if not await _boot():
		return
	_open_screen()
	assert_true(_shell.begin_join_choice(JoinTarget.FLAG_SERVER_PASSWORD, _on_committed,
			_on_abort))
	var read := await _run({"until": "game_password", "wait_ms": 1000})
	assert_true(read.ok, read.summary)
	assert_eq_deep(read.data["windows"], ["ABORTRETRY_WRAPPER", "GAME_PASSWORD_WRAPPER"])
	var hidden := await _run({"op": "press", "control": "ABORT_CANCEL"})
	assert_false(hidden.ok, "ABORT_CANCEL's window is hidden on the choice")
	assert_eq(_aborts, 0)
	assert_true(_shell.is_join_screen_open())
	var back := await _run({"op": "press", "control": "BACK", "until": "closed",
			"wait_ms": 1000})
	assert_true(back.ok, back.summary)
	assert_eq(_aborts, 1, "BACK runs the join's abort")
	assert_eq_deep(_committed, [], "BACK commits nothing")


# `join` is the real dial with no role answered: a silent endpoint enumerates on
# the progress panel (ERROR_WRAPPER's MESSAGES with ABORT_WRAPPER's Cancel), and
# ABORT_CANCEL there abandons the join, stops the enumerate and leaves the shell
# in its menus (the admin pass's A7 hold (c)).
func test_a_join_enumerates_and_abort_cancel_leaves_it_for_the_menus() -> void:
	if not await _boot():
		return
	_open_screen()
	var joined := await _run({"op": "join", "address": "127.0.0.1", "port": 9,
			"until": "progress", "wait_ms": 1000})
	assert_true(joined.ok, joined.summary)
	assert_eq_deep(joined.data["windows"], ["ERROR_WRAPPER", "ABORT_WRAPPER"])
	var net := _shell.get_node("NetSessionController")
	assert_not_null(net.get_node_or_null("SpectatorPreflight"), "the join enumerates the endpoint")
	var abort := await _run({"op": "press", "control": "ABORT_CANCEL", "until": "closed",
			"wait_ms": 1000})
	assert_true(abort.ok, abort.summary)
	assert_eq(String(abort.data["shell_state"]), "menu", "the abandoned join leaves to the menus")
	await get_tree().process_frame
	assert_null(net.get_node_or_null("SpectatorPreflight"), "ABORT_CANCEL stops the enumerate")
