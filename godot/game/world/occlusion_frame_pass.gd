class_name OcclusionFramePass
extends RefCounted


# The render-occlusion frame (docs/render/render-occlusion-re.md §3/§4/§5),
# extracted from GameWorld: the blink letter gates, the per-render-frame
# section-mask/portal apply, the probe A/B seam edges, and the unload reset.
#
# HOT PATH: GameWorld calls apply_blink_gates()/apply_frame() directly every
# frame. Every entry here is a plain method call on a stored direct reference
# — ZERO Callables/lambdas anywhere in the frame path (the dispatcher-perf
# rule) — and steady frames allocate nothing at this seam (GUT-pinned by
# test_occlusion_steady_frames_touch_no_nodes).
#
# Diff-applied: the sim emits verdict CHANGES (get_building_visibility_changes /
# get_render_culled_changes) and only transitions touch nodes, so a steady frame
# does no per-node work. Two ownership bits on each ObjectModel decide final
# visibility: the entity presenter's placed walk owns the sim's intent
# (PF_HIDDEN -> set_present_visible), this system owns the occlusion hide
# (set_occlusion_hidden), and the node's visible flag is their product — the
# placed walk never fights an occlusion hide, and an occlusion release lands on
# the sim's current intent so a sim-hidden entity never flashes (the bit
# contract is pinned by mission_present_pass_test).

# The GameWorld whose render nodes this pass drives — a direct reference,
# stored once in setup(); per-frame reads go through its public getters as
# direct calls (ADR 0043 rule 11: no test subclasses the world).
var _world: GameWorld

# The local player's applied blink letter gates (render-occlusion-re.md §4):
# accum bit 0x2 hides the terrain render (near detail + far foliage ride the
# terrain node) and the sky dome + celestials; bit 0x8 hides the water passes.
# blink_indoors is deliberately a public plain var: the world's frame
# clear-color pass reads it directly every frame (indoors clears BLACK).
var blink_indoors := false
var _blink_water_suppressed := false
# bms_id -> resolved ObjectModel, so steady frames skip registry lookups; every
# node this pass hid or masked is in it, so the release walk (A/B seam, unload)
# finds the bits it set. Entries revalidate with is_instance_valid on use
# (LIVENESS, not typing); reset on unload/A-B seams.
var _occlusion_node_cache: Dictionary = {}

# The shared F3 frame-stats board (null outside the game shell), re-handed by
# GameWorld wherever its own board changes: this pass lands the OCCL_* split
# spans itself from apply_frame.
var _frame_stats: FrameStats = null
# Mirrors GameWorld._perf_probe_enabled, written only at the probe toggle edge
# (set_perf_probe_enabled): the manual A/B probe shares apply_frame's clock
# reads with the F3 Stats capture, exactly as before the extraction.
var probe_timing := false
# The two apply_frame halves, valid while probe/stats timing runs:
# the native run_occlusion_frame call and the GDScript node application.
var _perf_occl_native_us := 0
var _perf_occl_apply_us := 0


## One-time wiring from the owning GameWorld (constructed in the world's
## _init, before any load).
func setup(world: GameWorld) -> void:
	_world = world


func set_frame_stats(board: FrameStats) -> void:
	_frame_stats = board


# --- Blink frame gates (docs/render/render-occlusion-re.md §4) -----------------
# The local player's accumulated blink letters gate whole render passes. The
# letters are authored PER BOX (init 0x3E; letters clear bits), so windowed
# buildings simply don't carry the indoors letter and keep the outside world
# rendering — no portal special-casing at the gate level. The per-section
# interior visibility masks (the portal traversal) are the next occlusion slice.

## Re-apply the letter gates after a sim tick. `forces_indoors` is
## GameWorld._mission_forces_indoors, passed per call — the mission attribute
## is mission state and stays (test-pinned) on the world.
func apply_blink_gates(forces_indoors: bool) -> void:
	var runtime: MissionRoot = _world.get_runtime()
	if runtime == null:
		return
	var sim: Simulation = runtime.get_sim()
	if sim == null:
		return
	# The mission force-indoors attribute ORs the indoors letter into the frame
	# view for BOTH consumers, matching run_occlusion_frame's camera input
	# [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> accum |= 2].
	var flags := int(sim.local_player_blink_flags()) | (
			Simulation.BLINK_INDOORS if forces_indoors else 0)
	# Accum bit 0x2 (indoors): the terrain render is skipped entirely — the
	# near-detail and far-foliage tiers are terrain children here, matching
	# retail where the detail cells ride the skipped terrain traversal and the
	# far patches carry their own bit-2 gate — and the skybox pass (dome +
	# celestials) is skipped [orig: render_main_scene @ 0x5c1353 (PolyTrn
	# skip), terrain_scene_render @ 0x5d0570, Terrain_RenderSkyboxPass skip
	# @ 0x5ca84f, Foliage_RenderFarPatchesPass skips @ 0x5c95bf/0x5c9665].
	var indoors := (flags & Simulation.BLINK_INDOORS) != 0
	if indoors != blink_indoors:
		blink_indoors = indoors
		var terrain: Terrain = _world.get_terrain_node()
		if terrain != null:
			terrain.visible = not indoors
		var sky: SkyDome = _world.get_sky_dome_node()
		if sky != null:
			sky.visible = not indoors
		var celestial: Celestial = _world.get_celestial_node()
		if celestial != null:
			celestial.visible = not indoors
	# Accum bit 0x8 (the authored water letter): both water passes skipped.
	# Letter bits only accumulate while inside a box, so the outdoors leg of
	# retail's override is implicit; the remaining g_BlinkWaterVisible legs
	# (a camera building straddling the water plane, the window latch) ride
	# the section-mask slice [orig: Terrain_RenderSceneWithReflection
	# @ 0x5c93cb / @ 0x5c95d2-0x5c95ea].
	var water_off := (flags & Simulation.BLINK_WATER_OFF) != 0
	if water_off != _blink_water_suppressed:
		_blink_water_suppressed = water_off
		var water: Water = _world.get_water_node()
		if water != null:
			water.visible = not water_off


# --- The render-occlusion frame (docs/render/render-occlusion-re.md §3/§5) -----
# Per render frame: run the sim's occlusion pipeline (camera blink query ->
# portal traversal -> section masks + TOC occluder culling + the entity render
# gates), then drive the de-batched building nodes' per-section masks and the
# gated entities' visibility. Runs after the present pass (inside
# session frame) so present's base visibility is re-asserted first each frame.
# (The marched iris-exposure weather feed that renders alongside stays on
# GameWorld — _stamp_iris_samples.)
func apply_frame(camera_xform: Transform3D, forces_indoors: bool) -> void:
	var runtime: MissionRoot = _world.get_runtime()
	if runtime == null:
		return
	var sim: Simulation = runtime.get_sim()
	if sim == null:
		return
	var registry: EntityIndex = runtime.get_entity_index()
	if registry == null:
		return
	var fov_y := 70.0
	var near := 0.05
	var aspect := 16.0 / 9.0
	if _world.is_inside_tree():
		var cam: Camera3D = _world.get_viewport().get_camera_3d()
		if cam != null:
			fov_y = cam.fov
			near = cam.near
		var vs: Vector2 = _world.get_viewport().get_visible_rect().size
		if vs.y > 0.0:
			aspect = vs.x / vs.y
	var fog := 1000.0
	var env: MissionEnvironment = _world.get_environment_node()
	if env != null:
		fog = env.get_fog_distance()
	var water: Water = _world.get_water_node()
	var water_z := -100000.0
	if water != null and _world.is_water_render_active():
		water_z = water.water_height
	var stats_on := _frame_stats != null and _frame_stats.is_capture_active()
	var timing := probe_timing or stats_on
	var native_start := Time.get_ticks_usec() if timing else 0
	sim.run_occlusion_frame(camera_xform, fov_y, aspect, near, fog, water_z,
			forces_indoors)
	var native_end := Time.get_ticks_usec() if timing else 0
	var building_query_us := 0
	var building_apply_us := 0
	var cull_query_us := 0
	var cull_apply_us := 0
	var light_query_us := 0
	var light_apply_us := 0
	var water_apply_us := 0

	# Building batch visibility + per-section masks (bit N = render part N,
	# forced-visible def bits already merged by the sim), applied as CHANGES:
	# the sim diffs against what this shell last applied, so a steady frame
	# walks nothing. Batch culls claim the occlusion-hidden bit; the same
	# verdicts as the full-walk form land on the nodes.
	# [orig: Terrain_RenderSectorModels @ 0x5c5d30]
	var building_query_start := Time.get_ticks_usec() if timing else 0
	var changes: PackedInt64Array = sim.get_building_visibility_changes()
	if timing:
		building_query_us = Time.get_ticks_usec() - building_query_start
	var building_apply_start := Time.get_ticks_usec() if timing else 0
	for i in range(0, changes.size(), 2):
		var bms_id := int(changes[i])
		var node := _occlusion_node(registry, bms_id)
		if node == null:
			continue
		var packed := int(changes[i + 1])
		node.set_section_visibility_mask(Simulation.building_visibility_mask(packed))
		node.set_occlusion_hidden(not Simulation.building_visibility_visible(packed))
	if timing:
		building_apply_us = Time.get_ticks_usec() - building_apply_start

	# Entity render gates (the blink-hits gate + the outdoors three-ray latch),
	# also applied as changes. [orig: the collector gates @ 0x5c7022-0x5c708a / §3.4]
	var cull_query_start := Time.get_ticks_usec() if timing else 0
	var culled_changes: PackedInt32Array = sim.get_render_culled_changes()
	if timing:
		cull_query_us = Time.get_ticks_usec() - cull_query_start
	var cull_apply_start := Time.get_ticks_usec() if timing else 0
	if culled_changes.size() >= 2:
		var added := int(culled_changes[0])
		for i in range(1, 1 + added):
			var node := _occlusion_node(registry, int(culled_changes[i]))
			if node != null:
				node.set_occlusion_hidden(true)
		for i in range(2 + added, culled_changes.size()):
			var node := _occlusion_node(registry, int(culled_changes[i]))
			if node != null:
				node.set_occlusion_hidden(false)
	# The same collector gate over the rows the entity presenter's wire walk
	# draws (remote organics and runtime spawns with no placed identity), keyed
	# by wire handle: retail's client walks its wire-built pools exactly like
	# the host walks its own (the native gate carries the witness).
	var wire_culled_changes: PackedInt32Array = sim.get_wire_render_culled_changes()
	if wire_culled_changes.size() >= 2:
		var presenter_for_cull := runtime.get_entity_presenter()
		if presenter_for_cull != null:
			var wire_added := int(wire_culled_changes[0])
			for i in range(1, 1 + wire_added):
				presenter_for_cull.set_render_culled(int(wire_culled_changes[i]), true)
			for i in range(2 + wire_added, wire_culled_changes.size()):
				presenter_for_cull.set_render_culled(int(wire_culled_changes[i]), false)
	if timing:
		cull_apply_us = Time.get_ticks_usec() - cull_apply_start

	# The per-drawn-entity sun-visibility factor (D-RLIT-3), also applied as
	# changes: quality 1..4 maps to effectScale quality*0.25, dimming only the
	# directional term (engine/runtime/renderer/light_runtime.h
	# sun_visibility_factor owns the witness). Placed and wire identities share
	# this triple feed; wire rays use the separately keyed 17-tick candidate
	# arena, and the entity presenter retains the value for cold bodies/weapons.
	# [orig: setup_terrain_effect_for_entity @ 0x5c74a0, pushed per sector
	# entity draw @ 0x5c7bff]
	if env != null:
		var light_query_start := Time.get_ticks_usec() if timing else 0
		var sun_changes: PackedInt64Array = sim.get_draw_lighting_changes(
				env.get_light_direction())
		if timing:
			light_query_us = Time.get_ticks_usec() - light_query_start
		var light_apply_start := Time.get_ticks_usec() if timing else 0
		var presenter := runtime.get_entity_presenter()
		for i in range(0, sun_changes.size(), 3):
			var wire_handle := int(sun_changes[i])
			var bms_id := int(sun_changes[i + 1])
			# quality -> effectScale maps engine-side (one owner:
			# renderer::sun_visibility_factor via sun_quality_factor).
			var effect_scale := sim.sun_quality_factor(int(sun_changes[i + 2]))
			if wire_handle >= 0:
				if presenter != null:
					presenter.set_entity_lighting_context(
							wire_handle, effect_scale, false, 0.0)
				continue
			var sun_node := _occlusion_node(registry, bms_id)
			if sun_node != null:
				sun_node.set_entity_lighting_context(effect_scale, false, 0.0)
		if timing:
			light_apply_us = Time.get_ticks_usec() - light_apply_start

	# The g_BlinkWaterVisible override legs the slice-1 gate deferred: with the
	# authored water letter suppressing (accum bit 0x8), the water still renders
	# when the frame latched the exterior or a camera building straddles the
	# water plane. [orig: @ 0x5c93cb / @ 0x5c95d2 + g_BlinkWaterVisible
	# @ 0x29ACE40]
	var water_apply_start := Time.get_ticks_usec() if timing else 0
	if water != null:
		water.visible = not _blink_water_suppressed or bool(sim.occlusion_water_visible())
	if timing:
		water_apply_us = Time.get_ticks_usec() - water_apply_start

	if timing:
		_perf_occl_native_us = native_end - native_start
		_perf_occl_apply_us = Time.get_ticks_usec() - native_end
	if stats_on:
		_frame_stats.add(FrameStats.OCCL_APPLY, _perf_occl_apply_us)
		# The native call's internal split; the remainder of the bound call
		# (marshalling + the handle collection) lands in the glue slot so the
		# pane's Occlusion group still sums to the whole frame cost.
		var build_us := int(sim.get_last_occlusion_build_us())
		var probe_us := int(sim.get_last_occlusion_probe_us())
		_frame_stats.add(FrameStats.OCCL_BUILD, build_us)
		_frame_stats.add(FrameStats.OCCL_PROBE, probe_us)
		_frame_stats.add(FrameStats.OCCL_GLUE,
				maxi(_perf_occl_native_us - build_us - probe_us, 0))
		_frame_stats.add(FrameStats.OCCL_BUILDING_QUERY, building_query_us)
		_frame_stats.add(FrameStats.OCCL_BUILDING_APPLY, building_apply_us)
		_frame_stats.add(FrameStats.OCCL_CULL_QUERY, cull_query_us)
		_frame_stats.add(FrameStats.OCCL_CULL_APPLY, cull_apply_us)
		_frame_stats.add(FrameStats.OCCL_LIGHT_QUERY, light_query_us)
		_frame_stats.add(FrameStats.OCCL_LIGHT_APPLY, light_apply_us)
		_frame_stats.add(FrameStats.OCCL_WATER_APPLY, water_apply_us)


## The probe A/B seam, entering the occlusion skip: restore the water to the
## authored blink state, then release every occlusion override. Mission
## blink/indoors semantics remain authoritative (release keeps them; iris
## keeps sampling on the world).
func enter_probe_skip() -> void:
	var water: Water = _world.get_water_node()
	if water != null:
		water.visible = not _blink_water_suppressed
	release_overrides(false)


## Leaving the skip: re-arm a full re-emit from the sim's delta baseline.
func leave_probe_skip() -> void:
	_reset_apply_baseline()


# Resolve (and cache) the ObjectModel a bms_id drives. Cache entries revalidate
# with is_instance_valid (LIVENESS); a freed node re-resolves through the
# registry (reloads recreate nodes under the same ids).
func _occlusion_node(registry: EntityIndex, bms_id: int) -> ObjectModel:
	var cached: Variant = _occlusion_node_cache.get(bms_id)
	if cached != null and is_instance_valid(cached):
		return cached
	var node: ObjectModel = registry.resolve_single(bms_id)
	if node == null:
		_occlusion_node_cache.erase(bms_id)
		return null
	_occlusion_node_cache[bms_id] = node
	return node


# The live sim for the occlusion apply paths (null before a mission runtime
# exists — a real state on the unload/A-B seams).
func _occlusion_sim() -> Simulation:
	var runtime: MissionRoot = _world.get_runtime()
	return runtime.get_sim() if runtime != null else null


# Release every occlusion override: clear the occlusion-hidden bit on every
# node this pass touched (the node lands on the sim's CURRENT present intent —
# the placed walk's own bit — so a WAC/sim-hidden entity never flashes for a
# frame), clear section masks to fully-visible, drop the caches, and forget the
# sim's delta baseline so a later re-enable re-emits full state. The A/B seam
# keeps mission blink/indoors semantics (reset_semantics=false); reset_semantics
# = true marks the unload-time release — the mission force-indoors attribute
# itself stays on GameWorld (test-pinned by name) and unload clears it THERE,
# so the flag carries no extra work here.
func release_overrides(_reset_semantics: bool) -> void:
	for bms_id in _occlusion_node_cache:
		var node := _occlusion_hidden_release_node(int(bms_id))
		if node != null:
			node.set_occlusion_hidden(false)
			node.set_section_visibility_mask(-1)
	_occlusion_node_cache.clear()
	_reset_apply_baseline()


## Placed nodes arrived after the occlusion frames started (a joiner's
## streamed statics, placed once the host's world stream settles): forget the
## node lookups that missed and the sim's applied-state baseline, so the next
## frame re-emits every building/cull/sun verdict onto the new nodes instead
## of only the transitions since the mission began.
func rebind_placed_nodes() -> void:
	_occlusion_node_cache.clear()
	_reset_apply_baseline()


# The wire walk's applied render-gate verdicts pair with the sim's baseline:
# both forget together, so the next frame's full re-emit lands on a clean set.
func _clear_wire_render_culled() -> void:
	var runtime: MissionRoot = _world.get_runtime()
	var presenter: EntityPresenter = runtime.get_entity_presenter() \
			if runtime != null else null
	if presenter != null:
		presenter.clear_render_culled()


# The cache holds only ObjectModels; entries revalidate for LIVENESS on use.
func _occlusion_hidden_release_node(bms_id: int) -> ObjectModel:
	var cached: Variant = _occlusion_node_cache.get(bms_id)
	return cached if cached != null and is_instance_valid(cached) else null


# Forget the sim's applied-state baseline so the next occlusion frame re-emits
# everything (the shell caches were dropped or the A/B skip ended).
func _reset_apply_baseline() -> void:
	var sim := _occlusion_sim()
	if sim != null:
		sim.reset_occlusion_apply_baseline()
	_clear_wire_render_culled()


## Unload teardown. The ordering replicates the pre-extraction unload EXACTLY:
## 1. the blink letter gates restore [orig: the letter-bit clear @ 0x525c45 at
##    mission start] — an unload while indoors must not leave the next
##    mission's terrain/sky/water hidden;
## 2. every occlusion override releases (the hidden bits this pass set clear
##    onto each node's own present intent) and the sim delta baseline resets.
func reset() -> void:
	_reset_blink_frame_gates()
	release_overrides(true)


func _reset_blink_frame_gates() -> void:
	if blink_indoors:
		var terrain: Terrain = _world.get_terrain_node()
		if terrain != null:
			terrain.visible = true
		var sky: SkyDome = _world.get_sky_dome_node()
		if sky != null:
			sky.visible = true
		var celestial: Celestial = _world.get_celestial_node()
		if celestial != null:
			celestial.visible = true
	if _blink_water_suppressed:
		var water: Water = _world.get_water_node()
		if water != null:
			water.visible = true
	blink_indoors = false
	_blink_water_suppressed = false
