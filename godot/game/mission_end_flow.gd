class_name MissionEndFlow
extends RefCounted
## The SP end-of-mission flow, split out of the shell: armed by the sim's
## "round_end" effect [orig: Server_ProcessRoundEnd @0x5164f0 SP tail]. The
## world keeps ticking underneath (the SP world runs through the epilog —
## humans >= 1 keeps the run gate open); player input idles once the round is
## over [orig: the post-round input gate — the client input uplinks stop
## against g_spawn_success_gate @0x42c410]. The shell reads is_round_ended() /
## has_screen() for its input and mouse policy and drives tick() and
## show_screen() from its frame.

## The short beat between the round end and the score/failed screen stands in
## for the cine lead-in (the lose letterbox+fade, the win flyaway — D-AI-10).
## [orig: Cine_StartPlayback @0x577840 / Cine_InitPlayback @0x578390]
const LEAD_IN_SECONDS := 3.0

var _round_ended := false
var _winner := 0
var _screen_delay := 0.0
var _screen: MissionEndScreen = null


func is_round_ended() -> bool:
	return _round_ended


func has_screen() -> bool:
	return _screen != null


## The sim's round_end effect. An MP round rides EndRoundPresenter (5.68)
## instead, so a net session never enters this flow.
func begin(winner: int, sim: Simulation) -> void:
	if _round_ended:
		return
	if sim != null and bool(sim.get_round_outcome_debug().get("mp_session", false)):
		return
	_round_ended = true
	_winner = winner
	_screen_delay = LEAD_IN_SECONDS


## The lead-in beat, run once per shell frame while the world is loaded. True on
## the frame the beat expires: the caller mounts the screen (show_screen).
func tick(delta: float) -> bool:
	if not _round_ended or _screen != null:
		return false
	_screen_delay -= delta
	return _screen_delay <= 0.0


## Mount the score/failed screen under `mount` (the HUD layer, or the shell when
## there is none) over the sim's outcome record; `on_exit` receives the screen's
## exit request. One screen per round: a second call is a no-op.
func show_screen(sim: Simulation, banner: String, root: ResourceRoot, mount: Node,
		on_exit: Callable) -> void:
	if _screen != null:
		return
	var outcome: Dictionary = {}
	if sim != null:
		outcome = sim.get_round_outcome_debug()
	if outcome.is_empty():
		outcome = {"ended": true, "winner_team": _winner}
	_screen = MissionEndScreen.new()
	_screen.name = "MissionEndScreen"
	_screen.setup(outcome, banner, root)
	mount.add_child(_screen)
	_screen.exit_requested.connect(on_exit)
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


## ESC during the epilog leaves the mission: routed through the screen while it
## is up; false when no screen is mounted yet, so the caller exits directly
## [orig: ESC (0x1B) sets g_mission_exit_reason = 1 during the epilog,
## Input_HandleSpecialKeys @0x49c8e2].
func request_screen_exit() -> bool:
	if _screen == null:
		return false
	_screen.request_exit()
	return true


## The world teardown: every flag back to rest and the screen freed.
func reset() -> void:
	_round_ended = false
	_winner = 0
	_screen_delay = 0.0
	if _screen != null:
		_screen.queue_free()
		_screen = null
