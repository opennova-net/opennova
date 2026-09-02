class_name WorldDeviceFrame
extends RefCounted

# The Godot frame device legs (ADR 0035), extracted from GameWorld: the
# concrete renderer/audio/environment operations GameFramePipeline invokes in
# one visible order around inmatch::Session::advance(), plus the per-frame
# camera/timing latch begin_device_frame stamps for them and the exact-pose
# capture refresh (debug_refresh_render_pose) that re-runs the camera-driven
# legs at a frozen pose.
#
# HOT PATH: GameWorld's one-line leg delegates call straight in here every
# render frame — plain method calls on a stored direct reference, the
# OcclusionFramePass pattern. Shared world state (the engine nodes, the
# mission runtime, and every probe/perf counter a staying GameWorld method
# also reads) stays on GameWorld and is reached through `_world`; only the
# per-frame latch below lives here.

# The GameWorld whose device nodes these legs drive — a direct reference,
# stored once in setup(); reads go through its members as direct calls
# (harnesses subclass GameWorld, ADR 0034 — overridable names are always
# invoked as _world.<name>() so subclass overrides keep binding).
var _world: GameWorld

# Godot reserves this many vec4 values of the global shader buffer per
# geometry instance whose shader declares instance uniforms (see
# get_runtime_perf_counters' estimate).
const INSTANCE_UNIFORM_VALUES_PER_GEOMETRY := 16

var _frame_camera_pos := Vector3()
# Untyped on purpose: a Transform3D-typed member on this class crashes the
# engine's exit teardown when a test leaks a GameWorld instance (Godot 4.6
# quirk, bisected 2026-08-09); the Variant carries the camera transform.
# Several GUT files still construct GameWorld.new() without autofree, so the
# leak is not pinned to one test.
var _frame_camera_xform := Transform3D()
var _frame_delta := 0.0
var _frame_probe_enabled := false
var _frame_stats_on := false
var _frame_timing := false
var _frame_skip_occlusion := false
var _device_frame_start_us := 0
# Weakref edge latch for measured render time on the water reflection RTT.
var _stats_water_vp_ref: WeakRef = null
# Edge latch for the compositor passes' RD GPU timestamps (F3 capture only).
var _stats_aux_gpu_timing := false


## One-time wiring from the owning GameWorld (constructed in the world's
## _init, before any load).
func setup(world: GameWorld) -> void:
	_world = world


func begin_device_frame(camera_pos: Vector3, camera_xform: Transform3D,
		delta: float) -> void:
	_device_frame_start_us = Time.get_ticks_usec()
	_world._sample_panm_clock()
	_frame_camera_pos = camera_pos
	_frame_camera_xform = camera_xform
	_frame_delta = delta
	_frame_probe_enabled = _world._perf_probe_enabled
	_frame_stats_on = _world._frame_stats != null \
			and _world._frame_stats.is_capture_active()
	_frame_timing = _frame_probe_enabled or _frame_stats_on
	_frame_skip_occlusion = _frame_probe_enabled and _world._perf_probe_skip_occl
	if _frame_probe_enabled:
		_world._perf_probe_spans.clear()
	_world._last_tick_camera_pos = camera_pos
	_world._perf_foliage_us = 0
	_world._perf_runtime_us = 0
	_world._perf_audio_us = 0
	if _frame_probe_enabled and _world._world_ready:
		if _frame_skip_occlusion != _world._perf_probe_occlusion_skipped:
			if _frame_skip_occlusion:
				_world._occlusion.enter_probe_skip()
			else:
				_world._occlusion.leave_probe_skip()
		_world._perf_probe_occlusion_skipped = _frame_skip_occlusion
	elif _frame_probe_enabled:
		_world._perf_probe_occlusion_skipped = false


func finish_device_frame() -> void:
	_world._perf_tick_us = Time.get_ticks_usec() - _device_frame_start_us
	if _frame_stats_on:
		_world._frame_stats.add(FrameStats.WORLD_FOLIAGE, _world._perf_foliage_us)
		_world._frame_stats.add(FrameStats.WORLD_AUDIO, _world._perf_audio_us)
	_sample_water_render_stats(_frame_stats_on)
	_sample_auxiliary_render_stats(_frame_stats_on)


func render_terrain_frame() -> void:
	# The engine compiles the terrain patch draw list for this frame's camera and
	# Terrain applies it (ADR 0033 R2). Runs before the foliage leg, whose
	# dispatcher consumes the draw list's fresh detail-cell handoff — the old
	# self-driven _process walk left foliage reading a stale cell list. The frame
	# pipeline places the local view first, and Terrain samples that live viewport
	# camera inside render_frame (D-RORD-8).
	if _world._world_ready and _world._terrain != null:
		_world._terrain.render_frame()
		# MATCHTERRAIN consumes the same composed-page generation Terrain just
		# published. Refreshing here preserves retail's terrain-before-entity
		# order and prevents a moving crouched body from sampling a stale page.
		ObjectModel.refresh_match_terrain_frame(_world._terrain)


func render_foliage_frame() -> void:
	var foliage_start := Time.get_ticks_usec()
	_world._perf_foliage_us = 0
	if _world._world_ready and _world._dispatcher != null:
		# The silhouette tier is the hide-in-grass mechanic: retail's sector-entity
		# walk generates model foliage only around CROUCHED/PRONE infantry standing
		# on terrain — never around placed objects, whose MoveOrder stays 0
		# [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
		# (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7].
		var silhouette_anchors := PackedVector3Array()
		if _world._runtime != null:
			var anchor_sim := _world._runtime.get_sim()
			if anchor_sim != null:
				silhouette_anchors = anchor_sim.get_foliage_mask_anchor_positions()
		_world._dispatcher.silhouette_anchors = silhouette_anchors
		_world._dispatcher.render_frame(_render_camera_xform())
		_world._perf_foliage_us = Time.get_ticks_usec() - foliage_start


func drive_network_frame() -> bool:
	if not (_world._world_ready and _world._runtime != null
			and _world._runtime.is_playing()):
		return true
	if _world._join_wire_assets_pending and not _world.load_stages().apply_join_wire_til_if_ready():
		_world.report_join_wire_asset_failure(
				"join: host sent an incomplete or invalid S2C 0x45 terrain stream")
		return false
	# Net-session edges (admission/deploy/loss) + the gate's occupancy report.
	_world._net_drive.observe_tick(_world._runtime)
	return _world._runtime != null


## The precipitation presenter leg: the kernel re-floors the drop pool for
## this frame's camera and the renderer compiles the streaks
## (retail render_weather_trail_particles @ 0x5dee10 — after the camera-side
## particle pass, before the foliage billboards).
func render_precipitation_frame() -> void:
	var probe_phase_start := Time.get_ticks_usec() if _frame_timing else 0
	if _world._world_ready and _world._runtime != null \
			and _world._precipitation != null:
		if _world._env != null and _world._env.is_raining():
			var viewport := _world.get_viewport()
			_world._precipitation.render_frame(_world.get_sim(),
					viewport.get_camera_3d() if viewport != null else null)
		else:
			# Below the rain gate the drawer never runs (retail returns at
			# 0x5dee48 before touching the device).
			_world._precipitation.hide_frame()
	if _frame_timing:
		var precipitation_us := Time.get_ticks_usec() - probe_phase_start
		if _frame_probe_enabled:
			_world._perf_probe_spans["precipitation"] = precipitation_us
		if _frame_stats_on:
			_world._frame_stats.add(FrameStats.WORLD_WEATHER, precipitation_us)


func apply_blink_frame() -> void:
	# Blink flags only change on sim ticks; the driver invokes this leg only
	# after a batch that ran at least one.
	var probe_phase_start := Time.get_ticks_usec() if _frame_timing else 0
	if _world._world_ready:
		_world._occlusion.apply_blink_gates(_world._mission_forces_indoors)
	if _frame_timing:
		var blink_us := Time.get_ticks_usec() - probe_phase_start
		if _frame_probe_enabled:
			_world._perf_probe_spans["blink"] = blink_us
		if _frame_stats_on:
			_world._frame_stats.add(FrameStats.WORLD_BLINK, blink_us)


# The local-player VIEW placement, as a device leg: the camera/viewmodel move
# from the state THIS frame's session tick produced, BEFORE occlusion/iris/
# particles read the camera (D-RORD-8) [orig: the render frame builds its view
# from the current player state before collect+submit,
# Render_ProcessMainSceneFrame @ 0x5ca0f0]. A world without a presenter (tests,
# dedicated) skips it; main_game covers the frames that never reach this leg.
func present_local_view_frame() -> void:
	if _world._local_view_presenter != null:
		_world._local_view_presenter.after_world_tick()


## Compile the typed focused-Q3 snapshot after the camera and every live
## celestial/water/object producer has published this frame's final state. The
## immutable draw list is consumed by the terminal compositor against resolved
## beauty depth; there is no shared-world auxiliary camera or Q3 viewport.
func sync_framefx_frame() -> void:
	if _world._framefx != null:
		_world._framefx.advance_frame()


## The environment presenters' per-frame advance (ex-self-clocked _process
## bodies): weather smoothing toward the fixed-tick targets, the sun's
## direction law, the sky dome and the celestial bodies following the render
## eye. Ordered after the scene-environment classify so the lit consumers
## below (terrain, foliage, objects) read this frame's pushed globals.
func render_environment_nodes_frame() -> void:
	if _world._weather != null:
		_world._weather.advance_frame(_frame_delta)
	if _world._sun_shadow != null:
		_world._sun_shadow.advance_frame(_frame_delta)
	if _world._sky_dome != null:
		_world._sky_dome.advance_frame(_frame_delta)
	if _world._celestial != null:
		_world._celestial.advance_frame(_frame_delta)


## The water strip march + mirror camera for this frame's render eye
## (ex-self-clocked Water._process), under retail's per-frame water-active
## test: the terrain leg just tracked the visible terrain bounds, and the
## occlusion frame's Blink water verdict is last frame's, as retail reads it
## [orig: terrain_setup_view_and_lighting @ 0x60fe40].
func render_water_frame() -> void:
	if _world._water == null:
		return
	if _world._terrain != null:
		_world._water.set_visible_terrain_bounds(
				_world._terrain.has_visible_terrain_bounds(),
				_world._terrain.get_visible_terrain_min_height(),
				_world._terrain.get_visible_terrain_max_height())
	var sim := _world.get_sim()
	_world._water.set_blink_water_visible(
			sim != null and bool(sim.occlusion_water_visible()))
	_world._water.advance_frame(_frame_delta)


# Select the main scene's per-pass fog after the local player has placed the
# camera, and before every rendered consumer submits. Camera offsets are part
# of the render transform, so a v_offset-only waterline crossing must flip the
# same state as Water/Terrain. [orig: Environment_ApplyFogAndAmbient @ 0x57e440]
func apply_scene_environment_frame() -> void:
	if _world._env == null:
		return
	# The device leg only samples: the strict-vs-inclusive waterline
	# comparison semantics live in the engine behind apply_render_eye.
	var water_active := _world.is_water_render_active() and _world.is_inside_tree()
	var eye_y := 0.0
	if water_active:
		var cam := _world.get_viewport().get_camera_3d()
		if cam == null:
			water_active = false
		else:
			eye_y = cam.get_camera_transform().origin.y
	_world._env.apply_render_eye(eye_y,
			float(_world._water.water_height) if water_active else 0.0, water_active)
	# Publish the same adjusted render eye to the per-strip Q1/Q2 classifier.
	# This frame leg runs after camera placement and before ObjectModel's
	# retained material walk, so water crossings flip the ladder immediately.
	var shader_cache := ObjectShaderCache.get_singleton()
	if water_active:
		var water_height := float(_world._water.water_height)
		shader_cache.set_water_plane(water_height, eye_y >= water_height)
	else:
		shader_cache.clear_water_plane()


# The view the imminent render uses: the live camera AFTER the local-view leg
# placed it; the frame-entry stash only when no camera exists (headless
# worlds/tests) (D-RORD-8).
func _render_camera_xform() -> Transform3D:
	if _world.is_inside_tree():
		var cam := _world.get_viewport().get_camera_3d()
		if cam != null:
			return cam.global_transform
	return _frame_camera_xform


func apply_occlusion_frame() -> void:
	# The render-occlusion frame is camera-driven: it runs every render frame
	# (retail collects visible entities per scene render, not per sim tick),
	# and consumes the RENDER camera the local-view leg just placed (D-RORD-8).
	# [orig: Terrain_CollectVisibleEntities @ 0x5c9160 from
	# Terrain_RenderSceneWithReflection @ 0x5c94f0]
	if _world._world_ready:
		var probe_phase_start := Time.get_ticks_usec() if _frame_timing else 0
		if not _frame_skip_occlusion:
			_world._occlusion.apply_frame(_render_camera_xform(),
					_world._mission_forces_indoors)
		if _frame_probe_enabled:
			_world._perf_probe_spans["occl_frame"] = (0 if _frame_skip_occlusion
					else Time.get_ticks_usec() - probe_phase_start)
	elif _frame_probe_enabled:
		_world._perf_probe_spans["occl_frame"] = 0


func sample_iris_frame() -> void:
	if _world._world_ready:
		var probe_phase_start := Time.get_ticks_usec() if _frame_timing else 0
		# The iris re-target reads the render view too (D-RORD-8) [orig: retail
		# re-targets from the local player's view every render pass].
		_stamp_iris_samples(_render_camera_xform())
		if _frame_timing:
			var iris_us := Time.get_ticks_usec() - probe_phase_start
			if _frame_probe_enabled:
				_world._perf_probe_spans["iris"] = iris_us
			if _frame_stats_on:
				_world._frame_stats.add(FrameStats.WORLD_IRIS, iris_us)
	elif _frame_probe_enabled:
		_world._perf_probe_spans["iris"] = 0


## The sun-veil weather feed [orig: Environment_ApplySunVeilAndExposureStopdown
## @ 0x5ad8b0 runs once per main scene frame]: Celestial computed this frame's
## veil pair in its own advance (and pushed the white-quad alpha global);
## forward the exposure stop-down half to modulator-2's witnessed writer so
## the next weather ticks chase it. Zero-safe with either node absent.
func render_sun_veil_frame() -> void:
	if not _world._world_ready:
		return
	var veil_weather := _world.get_weather_node()
	if veil_weather == null or _world._celestial == null:
		return
	veil_weather.set_sun_veil_stopdown(_world._celestial.get_sun_veil_stopdown())


## The render-slot ground-shadow plan for this camera (GameFramePipeline,
## after the material frame: render_light_frame pushed this frame's
## LightScene and light context into the device, the material frame may
## have rebuilt the model subtrees the capture channels are stamped on, and
## slot priority plus the capture poses are camera-relative)
## [orig: render_shadow_pass @ 0x5d7b70 once per main scene frame].
func render_slot_shadow_frame() -> void:
	if _world._slot_shadow != null:
		_world._slot_shadow.advance_frame()


## Retail refreshes TexCubeEnvironment during the offscreen preparation leg:
## all six 256-square faces together initially/when forced and every 128 render
## frames, centered on the simulation player and clamped above terrain.
## [orig: update_environment_cubemap @ 0x6106a0].
func render_environment_cube_frame() -> void:
	if not _world._world_ready or _world._environment_cube == null:
		return
	var capture_position := _render_camera_xform().origin
	var capture_sim := _world.get_sim()
	if capture_sim != null:
		capture_position = capture_sim.get_local_player_position()
	_world._environment_cube.advance_frame(capture_position)


func mix_audio_frame(ticks_run: int) -> void:
	var audio_start := Time.get_ticks_usec()
	if _world._world_ready and _world._mission_audio != null:
		# Ambient soundloop regions read that same clock [orig:
		# Entity_CalcTimeOfDayRegion @ 0x408110].
		if _world._env != null:
			_world._mission_audio.set_time_of_day_hhmm(_world._env.time_of_day)
		# Marker eval/registration rides the sim's logic-tick clock — the witnessed
		# pool-2 stagger [orig: Entity_UpdateAllEntities @ 0x4c225a]; the per-frame
		# call below is only the live-slot mix + voice binds [orig:
		# SoundEmitter_UpdateAndMixTop8 @ 0x521341]. A world with no ticking runtime
		# (editor idle) free-runs the eval clock off render delta instead.
		if ticks_run > 0 and _world._runtime != null:
			var audio_sim := _world._runtime.get_sim()
			if audio_sim != null:
				_world._mission_audio.advance_ticks(int(audio_sim.get_logic_tick()))
				# The weather tick's thunder one-shots, placed around the
				# listener (mission_audio.gd carries the cites).
				_world._mission_audio.play_weather_sounds(
						audio_sim.drain_weather_sounds(), _frame_camera_xform)
		_world._mission_audio.tick(_frame_camera_pos, _frame_delta)
		_world._music_var_pump()
		_world._perf_audio_us = Time.get_ticks_usec() - audio_start


func render_material_frame() -> void:
	# The per-model runtime advance (PANM registers, dynamic materials, part/body
	# anim, staggered env restamp) — the ex-self-clocked ObjectModel _process,
	# now one static driver over the shared awake set at a defined ladder slot
	# (after occlusion resolves visibility, before the particle composite)
	# [orig: Terrain_RenderSectorModels @ 0x5c5d30 computes model runtime
	# constants during the render sector walk].
	var viewport := _world.get_viewport() if _world.is_inside_tree() else null
	var camera := viewport.get_camera_3d() if viewport != null else null
	if camera != null:
		var viewport_size := viewport.get_visible_rect().size
		ObjectModel.update_authored_lods(camera.global_transform, camera.fov,
				viewport_size.x, viewport_size.y)
		# The retained static instances select their RLOD per entity from the
		# same camera frame (the placer rewrites only the slots that crossed).
		if _world._placer != null:
			_world._placer.update_static_lods(camera.global_transform, camera.fov,
					viewport_size.x, viewport_size.y)
	if not _frame_stats_on:
		ObjectModel.advance_awake_frame(_frame_delta)
		return
	var profile := ObjectModel.profile_awake_frame(_frame_delta)
	if profile.size() < ObjectModel.AWAKE_PROFILE_SLOT_COUNT:
		return
	_world._frame_stats.add(FrameStats.MODEL_CLOCK_ANIMATION,
			profile[ObjectModel.AWAKE_PROFILE_CLOCK_ANIMATION_US])
	_world._frame_stats.add(FrameStats.MODEL_PANM,
			profile[ObjectModel.AWAKE_PROFILE_PANM_US])
	_world._frame_stats.add(FrameStats.MODEL_MATERIAL,
			profile[ObjectModel.AWAKE_PROFILE_MATERIAL_US])
	_world._frame_stats.add(FrameStats.MODEL_ORDER_BOUNDS,
			profile[ObjectModel.AWAKE_PROFILE_ORDER_BOUNDS_US])
	_world._frame_stats.add(FrameStats.MODEL_AWAKE_MODELS,
			profile[ObjectModel.AWAKE_PROFILE_AWAKE_MODELS])
	_world._frame_stats.add(FrameStats.MODEL_RENDERABLE_MODELS,
			profile[ObjectModel.AWAKE_PROFILE_RENDERABLE_MODELS])


func render_particle_frame() -> void:
	if _world._effect_world != null:
		_world._effect_world.render_frame()


## The EffectWorld point-light device leg: per visible model, select the
## witnessed <= 4 pool lights for that draw context and write them as
## per-instance shader parameters (effect_light_director.gd carries the seam
## notes). The viewmodel parts ride along with the local player as owner so
## first-person self-lights gate correctly.
func render_light_frame() -> void:
	if _world._light_director == null or not _world.is_inside_tree():
		return
	var viewport := _world.get_viewport()
	var viewmodel_parts: Array[ObjectModel] = []
	if _world._local_view_presenter != null:
		viewmodel_parts = _world._local_view_presenter.vm_parts()
	var viewmodel_owner := -1
	var sim: Simulation = _world.get_sim()
	if sim != null and sim.has_local_player():
		viewmodel_owner = sim.get_local_player_wire_handle()
	_world._light_director.render_frame(
			viewport.get_camera_3d() if viewport != null else null,
			viewmodel_parts, viewmodel_owner, _frame_stats_on)
	# The terrain leg of the same pool: the next terrain frame re-draws its
	# patches with the pool lights they overlap (terrain_light_leg.gd).
	TerrainLightLeg.render_frame(_world._terrain, _world._light_director)
	# Feed the render-slot shadow device the same point-light context (its
	# per-slot dominant-light pick reads the shared pool) plus the local
	# player state for the retail priority/drape gates.
	if _world._slot_shadow != null:
		_world._slot_shadow.set_light_scene(_world._light_director.scene())
		_world._slot_shadow.set_light_context(_world._light_director.light_gain(),
				Time.get_ticks_msec(), _world._weather)
		if _world._resource_root != null:
			_world._slot_shadow.set_resource_root(_world._resource_root)
		if _world._local_view_presenter != null:
			_world._slot_shadow.set_local_player_model(
					_world._local_view_presenter.avatar())
			_world._slot_shadow.set_local_player_first_person(
					not _world._local_view_presenter.is_third_person())
		if sim != null:
			_world._slot_shadow.set_local_player_prone(
					sim.get_local_player_stance_latch() == 2)


func update_clear_frame() -> void:
	if not _world._world_ready or not _world.is_visible_in_tree():
		_restore_idle_frame_clear_color()
	else:
		_update_frame_clear_color()


# The two compositor passes the root-viewport rows cannot split out: the
# focused Q3 draw list and the slot-shadow captures. Both report typed
# per-frame counts (the compile of this frame, the draw of the previous one).
# Focused Q3 and the slot-shadow captures both draw inside the root
# compositor (POST_TRANSPARENT and PRE_OPAQUE); their per-pass counts come off
# the effects' typed reports. Their GPU spans are carved back out of the root
# rows by RD timestamps the effects capture only while the Stats tab does —
# capture_timestamp barriers the RD graph, so the toggle rides stats_on
# (re-applied every captured frame so a late-built effect still hears it,
# switched off on the capture's falling edge).
func _sample_auxiliary_render_stats(stats_on: bool) -> void:
	if not stats_on:
		if _stats_aux_gpu_timing:
			_stats_aux_gpu_timing = false
			if _world._framefx != null:
				_world._framefx.set_gpu_timing_enabled(false)
			if _world._slot_shadow != null:
				_world._slot_shadow.set_gpu_timing_enabled(false)
		return
	_stats_aux_gpu_timing = true
	if _world._framefx != null:
		_world._framefx.set_gpu_timing_enabled(true)
		var q3_report := _world._framefx.get_backend_report()
		_world._frame_stats.add(FrameStats.RENDER_Q3_OBJECTS,
				int(q3_report.get("q3_drawn_commands", 0)))
		_world._frame_stats.add(FrameStats.RENDER_Q3_DRAWS,
				int(q3_report.get("q3_gpu_draw_calls", 0)))
		if bool(q3_report.get("q3_gpu_valid", false)):
			_world._frame_stats.add(FrameStats.RENDER_Q3_GPU,
					int(q3_report.get("q3_gpu_us", 0)))
	if _world._slot_shadow != null:
		_world._slot_shadow.set_gpu_timing_enabled(true)
		var slot_report := _world._slot_shadow.get_report()
		_world._frame_stats.add(FrameStats.RENDER_SLOT_OBJECTS,
				int(slot_report.get("slot_surfaces_compiled", 0)))
		_world._frame_stats.add(FrameStats.RENDER_SLOT_DRAWS,
				int(slot_report.get("slot_draw_calls", 0)))
		_world._frame_stats.add(FrameStats.RENDER_SLOT_CAPTURES,
				int(slot_report.get("slot_captures_drawn", 0)))
		_world._frame_stats.add(FrameStats.RENDER_SLOT_PACKED_VERTICES,
				int(slot_report.get("slot_packed_vertices", 0)))
		_world._frame_stats.add(FrameStats.RENDER_SLOT_SKINNED,
				int(slot_report.get("slot_skinned_commands", 0)))
		if bool(slot_report.get("slot_gpu_valid", false)):
			_world._frame_stats.add(FrameStats.RENDER_SLOT_GPU,
					int(slot_report.get("slot_gpu_us", 0)))


func is_water_render_stats_measured() -> bool:
	return _stats_water_vp_ref != null \
			and is_instance_valid(_stats_water_vp_ref.get_ref())


# Water-reflection RTT sampling for the Stats tab: flip measured render time on
# the reflection SubViewport only while the tab captures, then land the
# previous frame's CPU/GPU times on the board. Weakref-latched so a freed
# viewport never sees a stale-RID RenderingServer call.
func _sample_water_render_stats(stats_on: bool) -> void:
	var viewport: SubViewport = null
	if stats_on and _world._water != null:
		var viewport_v: Variant = _world._water.get_reflection_viewport()
		if viewport_v is SubViewport and is_instance_valid(viewport_v):
			viewport = viewport_v
	var previous: Object = _stats_water_vp_ref.get_ref() if _stats_water_vp_ref != null else null
	if previous != viewport:
		if previous is SubViewport:
			RenderingServer.viewport_set_measure_render_time(
					(previous as SubViewport).get_viewport_rid(), false)
		_stats_water_vp_ref = weakref(viewport) if viewport != null else null
		if viewport != null:
			RenderingServer.viewport_set_measure_render_time(
					viewport.get_viewport_rid(), true)
	if viewport == null:
		return
	var rid := viewport.get_viewport_rid()
	_world._frame_stats.add(FrameStats.RENDER_WATER_CPU,
			int(RenderingServer.viewport_get_measured_render_time_cpu(rid) * 1000.0))
	_world._frame_stats.add(FrameStats.RENDER_WATER_GPU,
			int(RenderingServer.viewport_get_measured_render_time_gpu(rid) * 1000.0))
	# What the mirror pass actually re-rendered (previous frame): the witnessed
	# reflection re-renders the world scene [orig: Water_ReflectionPrerender
	# @ 0x5c2780], so its submission count is a first-class stats row.
	_world._frame_stats.add(FrameStats.RENDER_WATER_OBJECTS,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer.VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME))
	_world._frame_stats.add(FrameStats.RENDER_WATER_DRAWS,
			RenderingServer.viewport_get_render_info(rid,
					RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE,
					RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME))


func _stop_water_render_stats() -> void:
	var previous: Object = (
			_stats_water_vp_ref.get_ref() if _stats_water_vp_ref != null else null)
	if previous is SubViewport:
		RenderingServer.viewport_set_measure_render_time(
				(previous as SubViewport).get_viewport_rid(), false)
	_stats_water_vp_ref = null


## Re-evaluates only camera-dependent production render state for an exact-pose
## visual capture. The caller must first make camera current and stop its normal
## presenter. This deliberately does not drive the mission session, weather
## clock, material animation, particles, or audio. Accumulator state that the
## live pipeline needs many frames to reach (the iris exposure chase, the
## glare occlusion window) is instead SETTLED at the capture pose through the
## witnessed per-tick math, so a frozen fixture measures the steady state a
## resting retail camera shows rather than a starved accumulator (D-RLIT-2
## fixture starvation).
func debug_refresh_render_pose(camera: Camera3D) -> Error:
	if not _world._world_ready or not _world.is_inside_tree():
		return ERR_UNAVAILABLE
	if camera == null or not is_instance_valid(camera) \
			or not camera.is_inside_tree() or not camera.is_current() \
			or camera.get_viewport() != _world.get_viewport():
		return ERR_INVALID_PARAMETER

	_frame_camera_pos = camera.get_camera_transform().origin
	_frame_camera_xform = camera.global_transform
	# The frozen path never runs the live iris/exposure legs, so a fixture
	# used to publish the modulator's mission-reset identity gain — the flat
	# exposure half of the D-RLIT-2 fixture starvation. Stamp the marched
	# samples for the capture pose and chase the modulator to its settled
	# state through the witnessed math only (Weather.settle_exposure holds
	# the freeze contract: no weather time, no mission clock).
	# The celestial device normally self-refreshes during a live frame; drive
	# it first at the frozen pose so the settled glare brightness, the veil
	# alpha global, and this frame's stop-down all exist before the exposure
	# settle chases them.
	if _world._celestial != null:
		# The glare occlusion brightness accumulates over ~a dozen live
		# frames; a frozen fixture gets exactly one zero-delta advance, which
		# left the sun glow invisible at any pose (the D-RLIT-2 fixture
		# starvation's other half). Settle the witnessed ray/window/step leg
		# at this pose first, then publish it through the normal frame.
		_world._celestial.settle_glare_occlusion()
		_world._celestial.advance_frame(0.0)
	render_sun_veil_frame()
	_stamp_iris_samples(_frame_camera_xform)
	var settle_weather := _world.get_weather_node()
	if settle_weather != null:
		settle_weather.settle_exposure()
	# Keep the same camera-producer order as GameFramePipeline, omitting every
	# time-owning leg. Terrain publishes the detail-cell handoff consumed by
	# foliage; occlusion then resolves the world visibility for this exact view.
	apply_scene_environment_frame()
	render_terrain_frame()
	render_foliage_frame()
	apply_occlusion_frame()

	# These native devices advance as GameFramePipeline legs (sky/celestial
	# before terrain, water between terrain and foliage). A fixture freezes
	# their parent before moving the capture camera, so drive their public
	# zero-delta frame seams explicitly after that move.
	if _world._sky_dome != null:
		_world._sky_dome.advance_frame(0.0)
	if _world._water != null:
		# Water's public frame seam retargets the mirror/strip and advances its
		# render-noise counter exactly once. The fixture freezes immediately after
		# this call and records that non-canonical phase in its manifest.
		_world._water.advance_frame(0.0)
	# Rebuild the particle draw lists for the moved capture camera. The effect
	# SIM stays frozen (only fixed ticks advance it, and the runtime is paused);
	# render_frame re-orients billboards and re-attaches the compositor to the
	# now-current view. Without this the last pre-freeze draw list — built for
	# the old camera pose — is all that renders, and captures lose every live
	# emitter (the 00TRa fire-barrel flame was the exposing case).
	render_particle_frame()
	# Reselect the point lights for the fixture camera the same way (the
	# flicker phase freezes with the weather ring, matching the phase
	# contract).
	render_light_frame()
	# Re-plan the render-slot ground shadows for the moved capture camera
	# (slot priority and the capture poses are camera-relative).
	render_slot_shadow_frame()
	update_clear_frame()
	return OK


# The marched iris-exposure feed (D-RLIT-2): three camera-ray samples from the
# sim each render frame, consumed by Weather's exposure re-target on its
# next tick [orig: Environment_ApplyFogAndAmbient @ 0x57e512 ->
# compute_ambient_light_along_direction @ 0x5c7a00 — retail re-targets from the
# local player's view every render pass]. The render-occlusion frame it used
# to share a section with (blink letter gates + the section-mask/portal apply)
# lives in occlusion_frame_pass.gd; the iris march stays here as the weather
# feed.
func _stamp_iris_samples(camera_xform: Transform3D) -> void:
	var weather: Weather = _world._weather
	var sim := _world.get_sim()
	if weather == null or sim == null:
		return
	var light_dir := Vector3.UP
	if _world._env != null:
		light_dir = _world._env.get_light_direction()
	weather.iris_samples = sim.compute_iris_samples(
			camera_xform.origin, -camera_xform.basis.z, light_dir)


# --- Frame clear color (env divergence #21, closed) ----------------------------


func _restore_idle_frame_clear_color() -> void:
	_world._clear_env_generation = -1
	if _world._clear_color == null or _world._clear_color.environment == null:
		return
	_world._clear_color.environment.background_color = _world._idle_frame_clear_color


# The witnessed frame clear: the horizon-blended skyfog above water, the lit
# water color underwater [orig: Render_ProcessMainSceneFrame @ 0x5ca776..
# 0x5ca792 - clear color = alternate_fog ? 0x808080 : cam above water ?
# skyfog[0] : Env_WaterColorLit; the vehicle alternate-fog view is not modeled
# yet]. Both branches serve RENDER-SPACE (x2-gained) colors, consumed VERBATIM
# by the modulate2x-path Clear this renderer reproduces (D-RMAT-7): above water the
# post-blend DOUBLED skyfog, underwater Env_WaterColorLit = water x light >> 7;
# the halving branch [orig: @ 0x67715d] is the non-modulate2x fallback with no
# Godot analog. The ClearColor Environment must stay BG_COLOR with ambient
# disabled - BG_SKY with no sky renders black and swallows these writes
# (GUT-pinned).
func _update_frame_clear_color() -> void:
	if _world._clear_color == null or _world._clear_color.environment == null \
			or _world._env == null:
		return
	# The clear SELECTION (black indoors / skyfog above water / lit water
	# underwater) is the engine's (environment_state.h carries the witness);
	# this device classifies the eye and writes the color. The sentinel
	# generation (-2) forces a recompute on indoors exit.
	if _world._occlusion.blink_indoors:
		if _world._clear_env_generation != -2:
			_world._clear_env_generation = -2
			_world._clear_color.environment.background_color = (
					_world._env.frame_clear_color_for(true, true))
		return
	var above := not _world._env.is_underwater_view()
	var gen := int(_world._env.get_light_state().get_generation())
	if gen == _world._clear_env_generation and above == _world._clear_above_water:
		return
	_world._clear_env_generation = gen
	_world._clear_above_water = above
	_world._clear_color.environment.background_color = (
			_world._env.frame_clear_color_for(false, above))


func get_runtime_perf_counters() -> Dictionary:
	var foliage_backend: Dictionary = (
			_world._dispatcher.get_backend_report() if _world._dispatcher != null else {})
	return {
		"tick_us": _world._perf_tick_us,
		"foliage_us": _world._perf_foliage_us,
		"runtime_us": _world._perf_runtime_us,
		"audio_us": _world._perf_audio_us,
		"runtime": _world._runtime.get_perf_counters() if _world._runtime != null else {},
		"foliage": (_world._dispatcher.get_frame_stats().to_json_value()
				if _world._dispatcher != null else {}),
		"foliage_backend": foliage_backend,
		"framefx": _world._framefx.get_backend_report() if _world._framefx != null else {},
		"mission_placement": (_world._mission_stats.to_json_value()
				if _world._mission_stats != null else {}),
		"static_live_populations": _world.get_static_live_population_count(),
		"audio": _world._mission_audio.get_perf_counters() if _world._mission_audio != null else {},
		"instance_uniform_geometry_estimate":
				_instance_uniform_geometry_estimate(foliage_backend),
	}


# Godot reserves INSTANCE_UNIFORM_VALUES_PER_GEOMETRY vec4 values of the global
# shader buffer for every geometry instance whose shader declares instance
# uniforms, visible or not, and prints "Too many instances using shader
# instance variables. Increase buffer size in Project Settings." once the
# buffer_size budget is exhausted (16384 instances with the project's setting;
# shader_resource_validation_test.gd pins it). Godot does not expose the live
# allocation, so this sums the retained instance-uniform geometry the shell
# itself owns: the foliage draw pools (FoliageDispatcher), the placer's static
# populations (visible batches plus their shadow twins), and every surface
# instance of every live ObjectModel scene. Terrain patches, water, and the
# per-model shadow twins the placer parents under animated models are not
# counted: read the total as a floor on the allocation, not the exact figure.
func _instance_uniform_geometry_estimate(foliage_backend: Dictionary) -> Dictionary:
	var foliage_pool := int(foliage_backend.get("pool_size", 0))
	var static_populations := 0
	if _world._mission_stats != null:
		static_populations = (_world._mission_stats.batches
				+ _world._mission_stats.static_shadow_batches)
	var object_geometry := int(ObjectModel.get_live_geometry_instance_count())
	var buffer_size := int(ProjectSettings.get_setting(
			"rendering/limits/global_shader_variables/buffer_size", 0))
	return {
		"total": foliage_pool + static_populations + object_geometry,
		"budget": buffer_size / INSTANCE_UNIFORM_VALUES_PER_GEOMETRY,
		"foliage_pool": foliage_pool,
		"static_populations": static_populations,
		"object_geometry": object_geometry,
	}
