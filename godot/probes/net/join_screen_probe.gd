extends GameProbe

## join_screen: this process's join screen (pre.mnu PRE_GAME_MENU, the shell's
## PreGameMenuPresenter), driven as a player drives it. Each run makes one `op`:
## - `state` reads only.
## - `join` dials `address`:`port` from the menus through the shell's join seam.
##   This is the dial the `--lan-join <ip>:<port>` launch hook makes. No role is
##   answered, so a discovered session whose flag word offers a choice opens the
##   choice panels.
## - `set_text` types `text` into the GAME_PASSWORD or TEAM_PASSWORD edit.
## - `press` activates ACCEPT, BACK or ABORT_CANCEL through the screen's menu
##   driver, the path the join-screen GUT tests take.
## The run then waits up to `wait_ms` for `until`: the screen open or closed, or
## one panel showing. It returns the screen's read: open, the panel, the pre.mnu
## windows shown, the MESSAGES line, the two edits' text, the SPECTATE radio and
## the shell state.
## The panels, their walk and the status lines are the engine's
## (inmatch/pre_game_menu.h). The probe changes nothing a player could not.

const EDITS := ["GAME_PASSWORD", "TEAM_PASSWORD"]
const BUTTONS := ["ACCEPT", "BACK", "ABORT_CANCEL"]


func run(ctx: ProbeContext) -> ProbeVerdict:
	var op := String(ctx.args["op"])
	var refused := _act(ctx, op)
	if not refused.is_empty():
		return ProbeVerdict.failed(refused, _read_state(ctx))
	var until := String(ctx.args["until"])
	if not until.is_empty():
		var deadline := Time.get_ticks_msec() + int(ctx.args["wait_ms"])
		while not _holds(_read_state(ctx), until):
			if ctx.cancelled:
				return ProbeVerdict.failed("cancelled", _read_state(ctx))
			if Time.get_ticks_msec() >= deadline:
				return ProbeVerdict.failed("the join screen did not reach '%s' in %d ms"
						% [until, int(ctx.args["wait_ms"])], _read_state(ctx))
			await ctx.wait_frames(1)
	var state := _read_state(ctx)
	ctx.progress(state)
	return ProbeVerdict.passed("%s: open=%s panel=%s" % [op, str(state["open"]), state["panel"]],
			state)


# The join screen's read (the verdict's data).
static func _read_state(ctx: ProbeContext) -> Dictionary:
	var shell := ctx.shell
	var screen := ctx.join_screen()
	var open := screen != null and screen.is_open()
	var windows := []
	var driver: MenuDriver = screen.get_menu_driver() if open else null
	if open:
		for index in JoinScreenStatus.window_count():
			var window := JoinScreenStatus.window_name(index)
			if screen.is_window_shown(window):
				windows.append(window)
	return {
		"open": open,
		"panel": panel_name(screen.panel()) if open else "",
		"windows": windows,
		"message": screen.message_text() if open else "",
		"game_password": _text(driver, "GAME_PASSWORD"),
		"team_password": _text(driver, "TEAM_PASSWORD"),
		"spectate": _checked(driver, "SPECTATE"),
		"shell_state": shell.shell_state_name() if shell != null else "",
	}


## The panel state's name in the probe's `until` vocabulary.
static func panel_name(panel: int) -> String:
	if panel == JoinScreenStatus.PANEL_ERROR:
		return "error"
	if panel == JoinScreenStatus.PANEL_PROGRESS:
		return "progress"
	if panel == JoinScreenStatus.PANEL_GAME_PASSWORD:
		return "game_password"
	if panel == JoinScreenStatus.PANEL_SPECTATE:
		return "spectate"
	if panel == JoinScreenStatus.PANEL_SPECTATOR_PASSWORD:
		return "spectator_password"
	if panel == JoinScreenStatus.PANEL_TEAM_PASSWORD:
		return "team_password"
	return "panel_%d" % panel


# The op's action: "" once made, else why it was refused.
func _act(ctx: ProbeContext, op: String) -> String:
	match op:
		"state":
			return ""
		"join":
			return _join(ctx)
		"set_text", "press":
			return _drive(ctx, op)
	return "unknown op '%s'" % op


func _join(ctx: ProbeContext) -> String:
	var shell := ctx.shell
	if shell == null:
		return "no game shell"
	if shell.shell_state_name() != "menu":
		return "a join starts from the menus (the shell is in '%s')" % shell.shell_state_name()
	var address := String(ctx.args["address"])
	if address.is_empty():
		return "join needs an address"
	var target := JoinTarget.new()
	target.host_ip = address
	var port := int(ctx.args["port"])
	if port > 0:
		target.port = port
	ctx.log("join %s:%d" % [target.host_ip, target.port])
	shell.join_lan_server(target)
	return ""


func _drive(ctx: ProbeContext, op: String) -> String:
	var control := String(ctx.args["control"])
	var allowed: Array = EDITS if op == "set_text" else BUTTONS
	if not (control in allowed):
		return "%s takes a control in %s, not '%s'" % [op, str(allowed), control]
	var screen := ctx.join_screen()
	if screen == null or not screen.is_open():
		return "the join screen is not open"
	var wrapper := _wrapper_of(control)
	if not screen.is_window_shown(wrapper):
		return "%s is not on the screen (%s hidden, panel %s)" % [control, wrapper,
				panel_name(screen.panel())]
	var driver := screen.get_menu_driver()
	var id := driver.widget_id(control) if driver != null else -1
	if id < 0:
		return "pre.mnu has no control named '%s'" % control
	if op == "set_text":
		driver.set_widget_text(id, String(ctx.args["text"]))
		ctx.log("set_text %s (%d characters)" % [control, String(ctx.args["text"]).length()])
	else:
		ctx.log("press %s on panel %s" % [control, panel_name(screen.panel())])
		driver.activate(id)
	return ""


# The pre.mnu window each control sits in. A player reaches a control only while
# its window shows, so the probe refuses the others.
static func _wrapper_of(control: String) -> String:
	match control:
		"GAME_PASSWORD":
			return "GAME_PASSWORD_WRAPPER"
		"TEAM_PASSWORD":
			return "TEAM_PASSWORD_WRAPPER"
		"ACCEPT", "BACK":
			return "ABORTRETRY_WRAPPER"
		"ABORT_CANCEL":
			return "ABORT_WRAPPER"
	return ""


static func _holds(state: Dictionary, until: String) -> bool:
	match until:
		"open":
			return bool(state["open"])
		"closed":
			return not bool(state["open"])
	return bool(state["open"]) and String(state["panel"]) == until


static func _text(driver: MenuDriver, control: String) -> String:
	if driver == null:
		return ""
	var id := driver.widget_id(control)
	return String(driver.get_widget_text(id)) if id >= 0 else ""


static func _checked(driver: MenuDriver, control: String) -> bool:
	if driver == null:
		return false
	var id := driver.widget_id(control)
	return id >= 0 and bool(driver.is_widget_checked(id))
