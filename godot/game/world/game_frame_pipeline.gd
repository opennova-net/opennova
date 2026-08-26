class_name GameFramePipeline
extends RefCounted

## The first-class Godot frame owner. inmatch::Session decides lifecycle,
## cadence, input consumption, catch-up, and cancellation; this pipeline orders the
## actual Godot devices around that one typed call. There is deliberately no
## generic renderer interface: every leg is a direct GameWorld/presentation
## method over the one renderer we ship.

var _world: Node


func setup(world: Node) -> void:
	_world = world


func advance(camera_pos: Vector3, camera_xform: Transform3D, delta: float,
		input: MissionFrameInput) -> MissionFrameOutcome:
	if _world == null:
		return null
	if input == null:
		input = MissionFrameInput.new()
	input.delta_seconds = delta
	input.set_camera_sample(camera_pos, -camera_xform.basis.z, true)
	_world.begin_device_frame(camera_pos, camera_xform, delta)
	var frame_stats: FrameStatsBoard = _world.get_active_frame_stats_board()
	var device_timing: bool = _world.is_device_frame_timing_enabled()
	var leg_start := 0

	var presentation = _world.get_runtime()
	var outcome: MissionFrameOutcome = null
	if presentation != null and presentation.is_playing():
		var runtime_start := Time.get_ticks_usec() if device_timing else 0
		outcome = presentation.advance_session_frame(input)
		if device_timing:
			_world.record_runtime_frame(Time.get_ticks_usec() - runtime_start)
		if outcome != null and outcome.is_terminal():
			_world.session_frame_failed(outcome.error)
			return outcome

	# Place the local-player camera/viewmodel from the state the session tick
	# just produced, BEFORE every camera-driven render leg reads it (D-RORD-8).
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.present_local_view_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_LOCAL_VIEW,
				Time.get_ticks_usec() - leg_start)
	# The renderer's auxiliary views share this world and restore their color
	# into the beauty target, so they must take THIS frame's pose. Ordered
	# here, never self-clocked: a process callback races the placement above
	# and shears foliage/scars/coronas against the ground on every turn.
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.sync_framefx_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_FRAMEFX,
				Time.get_ticks_usec() - leg_start)
	# Retail re-applies fog/ambient per scene pass. Classify the adjusted render
	# eye after camera placement and publish that pass payload before terrain,
	# foliage, objects, viewmodel, and particles consume it.
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.apply_scene_environment_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_SCENE_ENV,
				Time.get_ticks_usec() - leg_start)
	# Terrain samples the viewport camera directly; foliage receives the same
	# live render transform. Their producer order stays terrain then foliage so
	# foliage consumes this frame's detail-cell handoff.
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.render_terrain_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_TERRAIN,
				Time.get_ticks_usec() - leg_start)
	_world.render_foliage_frame()
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	var network_frame_ok: bool = _world.drive_network_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_NETWORK_FRAME,
				Time.get_ticks_usec() - leg_start)
	if not network_frame_ok:
		return outcome
	_world.advance_weather_frame()
	if outcome != null and outcome.did_tick():
		_world.apply_blink_frame()
	_world.apply_occlusion_frame()
	_world.sample_iris_frame()
	# The sun-veil stop-down feed for the weather ticks banked above (the
	# veil alpha itself rides the Celestial shader-global push).
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.render_sun_veil_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_SUN_VEIL,
				Time.get_ticks_usec() - leg_start)
	# The EffectWorld point-light select for this camera (after iris publishes
	# the frame's ambient scale, before the lit material draws consume the
	# pushed globals).
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.render_light_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_LIGHT,
				Time.get_ticks_usec() - leg_start)
	# Per-model runtime advance (ex-self-clocked ObjectModel _process): after
	# occlusion resolves visibility, before the particle composite over it.
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.render_material_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_MATERIAL,
				Time.get_ticks_usec() - leg_start)
	# The render-slot ground shadows plan against the light select published
	# above and stamp the model subtrees the material frame just rebuilt
	# (ex-self-clocked SlotShadow _process, which ran after the whole frame).
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.render_slot_shadow_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_SLOT_SHADOW,
				Time.get_ticks_usec() - leg_start)
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.render_particle_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_PARTICLES,
				Time.get_ticks_usec() - leg_start)
	_world.mix_audio_frame(outcome.get_ticks_run() if outcome != null else 0)
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.update_clear_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_CLEAR,
				Time.get_ticks_usec() - leg_start)
	leg_start = Time.get_ticks_usec() if frame_stats != null else 0
	_world.render_environment_cube_frame()
	if frame_stats != null:
		frame_stats.add(FrameStatsBoard.WORLD_ENV_CUBE,
				Time.get_ticks_usec() - leg_start)
	_world.finish_device_frame()
	return outcome
