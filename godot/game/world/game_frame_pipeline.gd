class_name GameFramePipeline
extends RefCounted

## The first-class Godot frame owner. MissionSession decides lifecycle,
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

	var presentation = _world.get_runtime()
	var outcome: MissionFrameOutcome = null
	if presentation != null and presentation.is_playing():
		outcome = presentation.advance_session_frame(input)
		if outcome != null and outcome.is_terminal():
			_world.session_frame_failed(outcome.error)
			return outcome

	# Place the local-player camera/viewmodel from the state the session tick
	# just produced, BEFORE every camera-driven render leg reads it (D-RORD-8).
	_world.present_local_view_frame()
	# Retail re-applies fog/ambient per scene pass. Classify the adjusted render
	# eye after camera placement and publish that pass payload before terrain,
	# foliage, objects, viewmodel, and particles consume it.
	_world.apply_scene_environment_frame()
	# Terrain samples the viewport camera directly; foliage receives the same
	# live render transform. Their producer order stays terrain then foliage so
	# foliage consumes this frame's detail-cell handoff.
	_world.render_terrain_frame()
	_world.render_foliage_frame()
	if not _world.drive_network_frame():
		return outcome
	_world.advance_weather_frame()
	if outcome != null and outcome.did_tick():
		_world.apply_blink_frame()
	_world.apply_occlusion_frame()
	_world.sample_iris_frame()
	# The sun-veil stop-down feed for the weather ticks banked above (the
	# veil alpha itself rides the Celestial shader-global push).
	_world.render_sun_veil_frame()
	# The EffectWorld point-light select for this camera (after iris publishes
	# the frame's ambient scale, before the lit material draws consume the
	# pushed globals).
	_world.render_light_frame()
	# Per-model runtime advance (ex-self-clocked ObjectModel _process): after
	# occlusion resolves visibility, before the particle composite over it.
	_world.render_material_frame()
	# The render-slot ground shadows plan against the light select published
	# above and stamp the model subtrees the material frame just rebuilt
	# (ex-self-clocked SlotShadow _process, which ran after the whole frame).
	_world.render_slot_shadow_frame()
	_world.render_particle_frame()
	_world.mix_audio_frame(outcome.get_ticks_run() if outcome != null else 0)
	_world.update_clear_frame()
	_world.finish_device_frame()
	return outcome
