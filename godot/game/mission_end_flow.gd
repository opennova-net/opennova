class_name MissionEndFlow
extends RefCounted
## The SP end-of-round flow's shell half. The world's round end starts the
## engine's end-of-round cine (engine world/epilog_cine.h: the win epilog, the
## lose screen, their stage machines and timeouts); the shell mounts the
## MissionEndScreen, which draws the cine's live events every frame. The world
## keeps ticking underneath while gameplay input idles [orig: the post-round
## input gate — the client input uplinks stop against g_SpawnSuccessGate
## @0x42c410]. The round-over keys
## run the engine's special-key leg (Simulation.round_over_key) and every exit
## rides the session's mission exit through the main frame's router
## (GameWorld.main_frame_exit). An MP round rides EndRoundPresenter instead.

var _round_ended := false
var _screen: MissionEndScreen = null


func is_round_ended() -> bool:
	return _round_ended


func has_screen() -> bool:
	return _screen != null


## The sim's round_end effect: out of a session only (the engine started the
## cine on the same round end).
func begin(sim: Simulation) -> void:
	if _round_ended or sim == null or sim.is_mp_session():
		return
	_round_ended = true


## Mount the cine's screen under `mount` (the HUD layer, or the shell when there
## is none): it reads the sim's cine every frame; `banner` returns the stored
## end-of-round banner (the WAC Lose cause) the lose screen's banner line draws.
## One screen per round: a second call is a no-op.
func show_screen(sim: Simulation, banner: Callable, root: ResourceRoot, mount: Node) -> void:
	if _screen != null or not _round_ended:
		return
	_screen = MissionEndScreen.new()
	_screen.name = "MissionEndScreen"
	_screen.setup(sim, banner, root)
	mount.add_child(_screen)


## The world teardown: every flag back to rest and the screen freed.
func reset() -> void:
	_round_ended = false
	if _screen != null:
		_screen.queue_free()
		_screen = null
