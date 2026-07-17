extends RefCounted

# THE host fire-presentation pass: presents the sim's authoritative fired rounds on
# the viewing host — AI (and remote-player) fire sound, muzzle effect, and in-flight
# tracers, none of which SP had before this pass. The local player's own fire keeps
# its action-slot presentation (local_player_host._fire_action_effects) and is
# self-filtered here, exactly like wire_present_pass filters the local avatar.
#
# [orig: WeaponSlot_FireAndSpawnEffects @ 0x53F440 — on the firing host every AI
# anim-event fire plays the ammo-def 'ai_launch' sound set (+64) through
# Sound_PlayWithDistanceAttenuation @ 0x528E40 and spawns the 'ai_launcheffect'
# muzzle effect (+68) through CEffectWorld_SpawnEmitterAtPosition @ 0x5F6DF0 at the
# fire origin along the fire direction; remote clients re-fire the same ring records
# (net-re §5.60). Our sim records each spawn (RoundSim.fired) and this pass drains it
# once per logic tick — the same records, one presentation per shot.]
#
# Sound distance model [orig: Sound_PlayWithDistanceAttenuation @ 0x528E40]: a shot
# heard from >= 30 u arrives LATE by the witnessed propagation delay
# (62 * dist / 330) >> 2 ticks (g_SoundSpeedFixed @ 0x24D6660 = 330.0 u/s, the
# witnessed quarter-compression) — the original queues those in the pending-sound
# slots drained per tick by Sound_TickPendingSlots @ 0x529310; `_pending` here is
# that queue. The set's max range gates the play (soundDef+72 in the original; our
# bank applies the set's cull range at play time — range-checking at fire time vs
# play time is the tracked delta, audible only if the listener moves during the
# delay). The MF_Light muzzle glow leg (+36/+40 -> Entity_UpdateMuzzleGlowEffect
# @ 0x56C960, a light-pool glow) is a tracked deferral — no light-pool port yet.
#
# Tracers [orig: RoundData_SpawnRound @ 0x4EC0D0]: every tracer_rate-th round per
# shooter (forcetracer 0x8000 = every round) is visible in flight — retail renders
# the round's tracer item graphic plus a tracer-pool emitter styled by the ammo
# 'tracer_type <friendly> [enemy]' pair, selected against the LOCAL player's team on
# each presenting client (@ 0x4ec740; shooter == local player counts friendly). Ours
# draws an additive vertex-colored streak per tracer round (geometry stand-in — the
# tracer-pool emitter internals and round graphic models are tracked D-AI-8) in the
# witnessed style families: red 1/6/9/11 (stdred/rapidred/sniperred/df1red), green
# 2/7/10/12, ordnance 3/4/5 (rocket/at4/grenade). The SP host shows tracers
# unconditionally (the MP NoTracers rules bit, dword_24D1E34 & 1, is a net seam).

const TRACER_LEN := 12.0        # streak metres (stand-in; net_event_view precedent)
const SOUND_DELAY_MIN_DIST := 30.0  # units [orig: dist >= 0x1E gate @ 0x528ed4]
const SOUND_SPEED := 330.0          # units/s [orig: g_SoundSpeedFixed = 330.0 16.16]

const COL_TRACER_RED := Color(1.0, 0.32, 0.18)
const COL_TRACER_GREEN := Color(0.35, 1.0, 0.4)
const COL_TRACER_ORDNANCE := Color(1.0, 0.9, 0.55)

var _sim                        # NovaSimulation (drain + tracer source)
var _audio_provider := Callable()     # -> NovaMissionAudio (or null)
var _fx_provider := Callable()        # -> NovaEffectWorld (or null)
var _listener_provider := Callable()  # -> Vector3 listener position (camera)
var _mesh: ImmediateMesh
var _mesh_instance: MeshInstance3D
var _pending: Array = []        # [{ticks, set, pos}] — the pending-sound queue
# Probe/diagnostic counters (ai_threat_probe asserts the presentation actually ran).
var _stats := {"fires": 0, "sounds": 0, "delayed_sounds": 0, "effects": 0, "tracer_peak": 0}


func get_stats() -> Dictionary:
	return _stats.duplicate()


func setup(sim, container: Node3D, audio_provider: Callable, fx_provider: Callable,
		listener_provider: Callable) -> void:
	_sim = sim
	_audio_provider = audio_provider
	_fx_provider = fx_provider
	_listener_provider = listener_provider
	if container != null and is_instance_valid(container):
		_mesh = ImmediateMesh.new()
		_mesh_instance = MeshInstance3D.new()
		_mesh_instance.name = "FireTracers"
		_mesh_instance.mesh = _mesh
		# Additive, unshaded, vertex-colored, depth-tested (a world object, not an
		# overlay — walls occlude tracers). The original tracer material family is
		# additive ONE,ONE unlit (Tracer.fx VS_TRACER, render-material-re).
		var mat := StandardMaterial3D.new()
		mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		mat.vertex_color_use_as_albedo = true
		mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		mat.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
		_mesh_instance.material_override = mat
		container.add_child(_mesh_instance)


func teardown() -> void:
	if _mesh_instance != null and is_instance_valid(_mesh_instance):
		_mesh_instance.queue_free()
	_mesh_instance = null
	_mesh = null
	_pending.clear()


## Once per present (beside the other passes), after the sim advanced. `ticks` is
## the number of logic ticks in the batch (tick_realtime catch-up runs several per
## present) — the pending-sound countdown consumes logic ticks, not presents.
func present(ticks: int = 1) -> void:
	if _sim == null:
		return
	_drain_fires()
	_tick_pending_sounds(ticks)
	_draw_tracers()


func _drain_fires() -> void:
	if not _sim.has_method("drain_fire_presentation_events"):
		return  # stale native DLL — presentation degrades silently, sim unaffected
	var events: Array = _sim.drain_fire_presentation_events()
	if events.is_empty():
		return
	var audio = _audio_provider.call() if _audio_provider.is_valid() else null
	var fx = _fx_provider.call() if _fx_provider.is_valid() else null
	var listener := _listener_pos()
	for ev_v in events:
		var ev: Dictionary = ev_v
		# The local player's own fire is presented by the action-slot legs
		# [orig: ActionSlot_ExecuteActionTick @ 0x541A70 routing]; everyone
		# else's rides the ammo-def legs below.
		if bool(ev.get("is_local_player", false)):
			continue
		_stats.fires += 1
		var origin: Vector3 = ev.get("origin", Vector3.ZERO)
		var sound_set := String(ev.get("sound_set", ""))
		if audio != null and not sound_set.is_empty():
			# Propagation delay for far shots [orig: @ 0x528ed4-0x528ef2:
			# >= 30 u -> pending slot, (62*dist/330)>>2 ticks; else immediate].
			var dist := origin.distance_to(listener) if listener.is_finite() else 0.0
			if dist >= SOUND_DELAY_MIN_DIST:
				var delay_ticks := int(62.0 * dist / SOUND_SPEED) >> 2
				if delay_ticks > 0:
					_pending.append({"ticks": delay_ticks, "set": sound_set, "pos": origin})
					_stats.delayed_sounds += 1
				else:
					audio.fire_soundset(sound_set, origin)
					_stats.sounds += 1
			else:
				audio.fire_soundset(sound_set, origin)
				_stats.sounds += 1
		var effect := String(ev.get("effect", ""))
		if fx != null and not effect.is_empty():
			# The muzzle effect at the fire origin along the fire direction
			# [orig: the 56-B spawn descriptor -> CEffectWorld_SpawnEmitterAtPosition
			# @ 0x5F6DF0; every fire spawns one — no per-shooter guard on this leg].
			fx.spawn_effect(effect, origin, ev.get("forward", Vector3.FORWARD))
			_stats.effects += 1
		# ev["mf_light"]: the muzzle glow light — tracked deferral (no light pool).


# The pending-sound queue [orig: Sound_TickPendingSlots @ 0x529310 — countdown per
# logic tick, play 3D-positional at the recorded position on zero].
func _tick_pending_sounds(ticks: int) -> void:
	if _pending.is_empty():
		return
	var audio = _audio_provider.call() if _audio_provider.is_valid() else null
	var still: Array = []
	for p_v in _pending:
		var p: Dictionary = p_v
		p["ticks"] = int(p["ticks"]) - maxi(1, ticks)
		if int(p["ticks"]) > 0:
			still.append(p)
		elif audio != null:
			audio.fire_soundset(String(p["set"]), p["pos"])
	_pending = still


func _draw_tracers() -> void:
	if _mesh == null or not _sim.has_method("get_tracer_rounds"):
		return
	_mesh.clear_surfaces()
	var rows: PackedFloat32Array = _sim.get_tracer_rounds()
	if rows.is_empty():
		return
	_stats.tracer_peak = maxi(int(_stats.tracer_peak), rows.size() / 9)
	var local_team := int(_sim.get_local_player_team()) if _sim.has_method("get_local_player_team") else 0
	_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	var i := 0
	while i + 8 < rows.size():
		var pos := Vector3(rows[i], rows[i + 1], rows[i + 2])
		var vel := Vector3(rows[i + 3], rows[i + 4], rows[i + 5])
		var team := int(rows[i + 6])
		# Friendly/enemy style select vs the local player's team at present time —
		# each retail client resolves its own [orig: @ 0x4ec740; own/friendly ->
		# +0xE8, else +0xEC].
		var type_id := int(rows[i + 7]) if (local_team != 0 and team == local_team) else int(rows[i + 8])
		var dir := vel.normalized()
		if dir.length_squared() > 0.0001:
			var head := pos
			var tail := pos - dir * TRACER_LEN
			var col := _tracer_color(type_id)
			var tail_col := col
			tail_col.a = 0.0  # taper to nothing toward the muzzle
			_mesh.surface_set_color(tail_col)
			_mesh.surface_add_vertex(tail)
			_mesh.surface_set_color(col)
			_mesh.surface_add_vertex(head)
		i += 9
	_mesh.surface_end()


# The witnessed tracer_type id families [orig: AmmoDef_ParseTypeName — stdred 1,
# stdgreen 2, rocket 3, at4 4, grenade 5, rapidred 6, rapidgreen 7, sniperred 9,
# snipergreen 10, df1red 11, df1green 12]. Exact retail streak colors ride the
# tracer-pool emitter styles (tracked D-AI-8); these are the family stand-ins.
func _tracer_color(type_id: int) -> Color:
	match type_id:
		1, 6, 9, 11:
			return COL_TRACER_RED
		2, 7, 10, 12:
			return COL_TRACER_GREEN
		3, 4, 5:
			return COL_TRACER_ORDNANCE
		_:
			return COL_TRACER_ORDNANCE


func _listener_pos() -> Vector3:
	if _listener_provider.is_valid():
		var v = _listener_provider.call()
		if v is Vector3:
			return v
	return Vector3.INF
