class_name WorldEffectRouter
extends RefCounted

# The mission-effect and fixed-tick presentation router, extracted from
# GameWorld: the WAC/BMS effect routing (dialog audio, fx2ssn emitters), the
# per-source-tick impact/scorch drains, the runtime signal handlers, and the
# weather resync after a sim restore. Plain RefCounted on the internal-member
# pattern (OcclusionFramePass / WorldDeviceFrame / WorldPlayerVisuals).
#
# GameWorld keeps a route_mission_effects delegate (tests drive it) plus thin
# _on_runtime_effects/_on_runtime_fixed_tick/_on_runtime_simulation_restarted
# delegates so _start_runtime's signal connects keep binding GameWorld methods
# (a future harness can override them there). The mission_effects signal stays
# declared on GameWorld; this router emits it through _world, and reaches the
# shared presentation sinks (_mission_audio/_effect_world/_item_fx/
# _light_director) and the probe switches the same way.

# The GameWorld whose presentation sinks these effects route onto.
var _world: GameWorld


## One-time wiring from the owning GameWorld (constructed in the world's
## _init, before any load).
func setup(world: GameWorld) -> void:
	_world = world


# Fire mission audio + particle effects for presentation. PlayWavList actions surface as "dialog"
# effects carrying the dialog/wav id in `a`; route them to the mission audio (which resolves the id
# through the co-named .DBF and plays the LWF set). WAC fx commands surface with the effect name in
# `str`; route them to the effect world. Other kinds are still emitted via mission_effects for downstream
# consumers (HUD, etc.).
func route_mission_effects(effects: Array) -> void:
	for e in effects:
		var eff: MissionEffect = e
		var kind := eff.kind
		if kind == "dialog":
			# BMS PlayWavList: dialog id resolved through the co-named .DBF (queued).
			if _world._mission_audio != null:
				_world._mission_audio.play_dialog(eff.a)
		elif kind == "dialog_wav":
			# WAC wave/pwave: a scripted voice .wav by filename on its own channel.
			if _world._mission_audio != null:
				_world._mission_audio.play_wac_wave(eff.text)
		elif kind == "fx2ssn":
			# WAC fx2ssn: spawn the named effect at the SSN entity's position with
			# the emitter handle owned per entity — a scripted re-trigger detaches
			# the previous group (spawn_effect_owned), so loops/respawns never stack
			# emitters and FOREVEREMIT effects never accumulate
			# [orig: WacScript_SpawnEffectAtSsnEntity @ 0x4f23a0 — renamed from the
			# kong "sound" misnomer, it spawns a particle emitter]. The original
			# orients the emitter to the terrain surface normal at the entity's
			# grid cell, using the same recovered normal-map kernel as terrain.
			# [orig: WacScript_SpawnEffectAtSsnEntity @0x4f23a0 reads
			# outMillis/off_849934 after resolving the entity grid cell.]
			if _world._effect_world != null and _world._runtime != null:
				var ssn := eff.b
				var pos: Variant = _world._runtime.entity_position_for_ssn(ssn)
				if pos != null:
					var orientation := Vector3.UP
					if _world._terrain_data != null:
						orientation = _world._terrain_data.get_surface_normal_world(pos)
					_world._effect_world.spawn_effect_owned(
							ssn, eff.text, pos, orientation)
		# fx2tgt (spawn at a placed type-6088 target marker
		# [orig: WacScript_SpawnEffectAtTargetMarker @ 0x4f7fd0 — same misnomer
		# family]) stays unrouted: which .bms record field carries the 1..99
		# target number is unwitnessed — ptl-format-re.md §8.


# Drain the flight sim's resolved round impacts and present both descriptor legs.
# Impact particles are generic Always transients in the world domain; their
# production tick/order and catch-up age survive a multi-tick render frame.
# [orig: Projectile_UpdatePhysics @ 0x4e9d70 -> the type-specific impact
#  handler -> Projectile_SpawnImpactEffect @ 0x4e9b80]
func _route_round_impacts() -> void:
	var sim := _world.get_sim()
	if sim == null:
		return
	for row_v in sim.drain_round_impacts():
		var row: RoundImpactRow = row_v
		var pos := row.position
		if _world._effect_world != null and not row.effect.is_empty():
			_world._effect_world.spawn_effect_transient(row.effect, pos,
					row.direction, maxi(row.age_ticks, 0),
					EffectScene.RENDER_DOMAIN_WORLD,
					row.source_tick, row.source_order)
		if _world._mission_audio != null and not row.sound.is_empty():
			_world._mission_audio.fire_soundset(row.sound, pos)
		# The light_impact flash rides the effect leg's own gate (the row only
		# carries light fields when the ammo authors it and the effect presents)
		# [orig: AmmoDef_ProcessImpactEffect @ 0x40a2b3].
		if _world._light_director != null and row.has_light:
			_world._light_director.on_impact_light(pos,
					row.light_radius, row.light_color, row.light_ticks)


# Install simulation-resolved permanent scorch records into the terrain page
# compiler before this source tick's ordinary impact presentation. Bounds are
# exact 16.16 terrain x/z; Terrain owns selective page invalidation.
func _route_terrain_scorches() -> void:
	var sim := _world.get_sim()
	if sim == null or _world._terrain == null:
		return
	for row_v in sim.drain_terrain_scorches():
		var row: TerrainScorchRow = row_v
		_world._terrain.append_terrain_scorch(row.texture_index,
				row.minimum_x_q16, row.minimum_z_q16,
				row.maximum_x_q16, row.maximum_z_q16)


# Consume render-internal lifecycle effects first, route "dialog" actions to
# mission audio (resolved through the co-named .DBF + LWF set), then expose only
# the remaining downstream effects to HUD consumers.
func _on_runtime_effects(effects: Array) -> void:
	var routed: Array = []
	for effect_v in effects:
		var effect := effect_v as MissionEffect
		if effect != null and _world._item_fx.consume_control_effect(effect):
			continue
		routed.append(effect_v)
	if routed.is_empty():
		return
	_world.route_mission_effects(routed)
	_world.mission_effects.emit(routed)


func _on_runtime_fixed_tick(_logic_tick: int) -> void:
	var probe_enabled := _world._perf_probe_enabled
	var skip_fixed_handlers := probe_enabled and _world._perf_probe_skip_fixed_handlers
	if skip_fixed_handlers:
		return
	# Retail executes local weapon actions and physical impacts before the same
	# frame's global particle update. Consume each source tick synchronously so
	# admission slots, first emission, and catch-up chronology are exact; only
	# mission render Nodes remain batched until the session frame returns.
	if _world._local_player_weapon_tick_consumer.is_valid():
		_world._local_player_weapon_tick_consumer.call(
				_world.drain_local_player_weapon_events())
	_route_terrain_scorches()
	_route_round_impacts()
	# The light-pool lifecycle decay + the light_move round-glow follow, on
	# the witnessed 62 Hz cadence [orig: EffectWorld_TickInstancesAndLightScale
	# @ 0x5aa170 from Game_ProcessMainFrame; the round follow @ 0x4eaa9f].
	if _world._light_director != null:
		_world._light_director.advance_fixed_tick()
		var glow_sim := _world.get_sim()
		if glow_sim != null:
			_world._light_director.sync_round_glows(glow_sim.get_round_glow_rows())
	var skip_effect_tick := probe_enabled and _world._perf_probe_skip_effect_tick
	if _world._effect_world != null and not skip_effect_tick:
		if _world._frame_stats != null and _world._frame_stats.is_capture_active():
			var fx_start := Time.get_ticks_usec()
			_world._effect_world.advance_fixed_tick(Simulation.tick_dt())
			_world._frame_stats.add(FrameStats.EFFECTS_TICK,
					Time.get_ticks_usec() - fx_start)
		else:
			_world._effect_world.advance_fixed_tick(Simulation.tick_dt())


func _on_runtime_simulation_restarted() -> void:
	# A Stop/restart can restore the saved personal slot while the presenter still
	# owns an emplaced model. Consume that control event synchronously; no fixed
	# tick runs while stopped.
	if _world._local_player_weapon_tick_consumer.is_valid():
		_world._local_player_weapon_tick_consumer.call(
				_world.drain_local_player_weapon_events())
	if _world._terrain != null:
		_world._terrain.clear_terrain_scorches()
	if _world._effect_world == null:
		_resync_weather_after_restore()
		return
	_world._effect_world.reset_runtime_state()
	# Persistent item effects belong to the restored entity set, not the scene
	# that was just discarded. Re-register their admission and owner identities;
	# restore emits fresh controller-start lifecycle events for occupied baselines.
	_world._item_fx.reattach()
	if _world._light_director != null:
		_world._light_director.reattach()
	_resync_weather_after_restore()


# The restored baseline rewound the World's weather home; the render owner
# snaps its color blocks back onto the restored targets.
func _resync_weather_after_restore() -> void:
	var weather: Weather = _world._weather
	if weather != null:
		weather.resync_colors_now()
