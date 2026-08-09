extends RefCounted

# THE viewing-client fire-presentation pass: presents the sim's authoritative
# host rounds or decoded visual-only joiner rounds — AI/remote-player fire sound,
# muzzle effect, and in-flight tracers. The local player's own predicted fire keeps
# its action-slot presentation (PlayerWeaponEffects._fire_action_effects) and is
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
# (62 * dist / 330) >> 2 ticks. The gate and the pending-sound slot pool
# [orig: Sound_TickPendingSlots @ 0x529310] live in the SIM on the logic clock
# (world/fire_sound.h, S12a): the runtime stamps the camera listener each frame,
# the sim gates at fire time and counts down per logic tick, and this pass just
# plays whatever drain_fire_sounds() returns — including the adm-arm action-row
# sounds, which retail plays immediately at the shooter with no delay. The set's
# max range still culls at PLAY time in our bank vs fire time in retail (the
# tracked D-AI-8 delta). The MF_Light muzzle glow leg (+36/+40 ->
# Entity_UpdateMuzzleGlowEffect @ 0x56C960, a light-pool glow) is a tracked
# deferral — no light-pool port yet.
#
# Tracers [orig: RoundData_SpawnRound @ 0x4EC0D0]: every tracer_rate-th round per
# shooter (forcetracer 0x8000 = every round) is visible in flight — a channel in the
# tracer/trail emitter pool [orig: g_TracerEmitterPool @ 0x2BF5270], styled by the
# ammo 'tracer_type <friendly> [enemy]' id selected against the LOCAL player's team
# at spawn (@ 0x4ec740; shooter == local player counts friendly — our sim runs the
# same select, world/tracer_trails.h). The sim owns the point rings (one pre-move
# point per 62 Hz tick, drain after death); the witnessed style tables and the
# camera-facing ribbon build [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0] live in
# engine/runtime/renderer/tracer_frame.{h,cpp} (the Simulation
# compile_tracer_ribbons static, pinned by the renderer_tracer_frame ctest); this
# pass uploads the compiled per-family strips. Additive styles (std/rapid/
# sniper/df1/NVG) draw fog-to-black additive [orig: SetFogAndBlendMode mode 2 —
# ONE:ONE, alpha unused; our disable_fog stand-in = D-AI-12c]; smoke styles
# (rocket/at4/grenade) draw alpha-blended with scene fog [orig: mode 0]. The 4-wide
# wave-animated smoke/sniper cross-section and the distortion pass (style +0x828)
# are tracked in docs/world/world-wac-ai-re.md §24.6 (D-AI-12a/b); the round's item
# graphic (frndlyTrcrID/foeTrcrID + TRACER_SCALE/TRACER_WIDTH nodes) and the
# light_move glow (round+0x1B4) are D-AI-12d/e. The SP host shows tracers
# unconditionally (the MP NoTracers rules bit, dword_24D1E34 & 1, is a net seam
# wired via RoundSim.no_tracers_rule).

var _sim: Simulation                  # drain + trail source (null in data-driven tests)
var _audio_provider := Callable()     # -> MissionAudio (or null)
var _fx_provider := Callable()        # -> EffectWorld (or null)
var _listener_provider := Callable()  # -> Vector3 listener position (camera)
# (wire_handle: int, userpoint: String) -> Vector3 world muzzle point, or Vector3.INF.
# Retail's adm arm spawns at the shooter's own WEAPON, not at the wire position, so
# presentation needs a way back to that body's held-weapon node.
var _muzzle_provider := Callable()
var _mesh: ImmediateMesh
var _mesh_instance: MeshInstance3D
var _mat_additive: StandardMaterial3D  # std/rapid/sniper/df1/NVG [orig: fog-black additive]
var _mat_alpha: StandardMaterial3D     # rocket/at4/grenade smoke [orig: alpha + scene fog]
# Probe/diagnostic counters (ai_threat_probe asserts the presentation actually ran).
var _stats := {"fires": 0, "sounds": 0, "effects": 0, "tracer_peak": 0}

func get_stats() -> Dictionary:
	return _stats.duplicate()


## The ribbon geometry surface — the ADR 0018 read seam for tests asserting the
## rebuilt tracer strips (surface count, vertex layout) without private reach-ins.
func ribbon_mesh() -> ImmediateMesh:
	return _mesh


## Load-time pipeline warm: emit one invisible (alpha-0) strip on each ribbon
## material at `position` (must be in frustum so the strips actually draw) so
## their pipelines compile behind the loading screen instead of as a ~40 ms
## draw stall on the first tracer. The next present's clear_surfaces drops the
## warm strips.
func warm_pipelines(position: Vector3) -> void:
	if _mesh == null:
		return
	_mesh.clear_surfaces()
	for mat in [_mat_additive, _mat_alpha]:
		if mat == null:
			continue
		_mesh.surface_begin(Mesh.PRIMITIVE_TRIANGLE_STRIP, mat)
		for i in range(4):
			_mesh.surface_set_color(Color(1, 1, 1, 0.0))
			_mesh.surface_add_vertex(position + Vector3(0.001 * i, 0, 0))
		_mesh.surface_end()


func setup(sim: Simulation, container: Node3D, audio_provider: Callable,
		fx_provider: Callable, listener_provider: Callable,
		muzzle_provider := Callable()) -> void:
	_sim = sim
	_audio_provider = audio_provider
	_fx_provider = fx_provider
	_listener_provider = listener_provider
	_muzzle_provider = muzzle_provider
	if container != null and is_instance_valid(container):
		_mesh = ImmediateMesh.new()
		_mesh_instance = MeshInstance3D.new()
		_mesh_instance.name = "FireTracers"
		_mesh_instance.mesh = _mesh
		_mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		# Both families are unshaded vertex-colored, depth-tested (a world object,
		# not an overlay — walls occlude tracers), untextured (the witnessed B=0
		# ribbon writes no UVs). Additive family [orig: ONE:ONE, alpha unused,
		# fog-to-BLACK via SetFogAndBlendMode mode 2 @ CEffectChannel_RenderRibbon]:
		# vertex alpha rides at 1.0 so Godot's SRCALPHA:ONE add equals ONE:ONE;
		# fog off stands in for the fog-to-black leg (tracked in the RE record).
		_mat_additive = StandardMaterial3D.new()
		_mat_additive.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_mat_additive.vertex_color_use_as_albedo = true
		_mat_additive.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		_mat_additive.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
		_mat_additive.cull_mode = BaseMaterial3D.CULL_DISABLED  # [orig: pass cull-off]
		_mat_additive.disable_fog = true
		# Smoke family (rocket/at4/grenade) [orig: alpha blend + scene fog, mode 0].
		_mat_alpha = StandardMaterial3D.new()
		_mat_alpha.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_mat_alpha.vertex_color_use_as_albedo = true
		_mat_alpha.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		_mat_alpha.blend_mode = BaseMaterial3D.BLEND_MODE_MIX
		_mat_alpha.cull_mode = BaseMaterial3D.CULL_DISABLED
		container.add_child(_mesh_instance)


func teardown() -> void:
	if _mesh_instance != null and is_instance_valid(_mesh_instance):
		_mesh_instance.queue_free()
	_mesh_instance = null
	_mesh = null


## Once per present (beside the other passes), after the sim advanced. The
## pending-sound countdown consumes logic ticks inside the sim now
## (world/fire_sound.h) — this pass only drains and plays. Each drain forwards
## into a public data leg (the present_snapshot precedent): tests feed the same
## rows without a live sim.
func present() -> void:
	if _sim == null:
		return
	present_fires(_sim.drain_fire_presentation_events())
	present_fire_sounds(_sim.drain_fire_sounds())
	present_slot_sounds(_sim.drain_slot_sounds())
	present_sound_emitters(_sim.drain_sound_emitters())
	draw_tracer_rows(_sim.get_tracer_trails())


# The body slot-sound drain (footsteps/foley/landing thumps/death screams): the
# sim resolves each entity's SndProf.def profile slot to its authored set name
# and emits the (foot-level) position; this plays them full-volume positional
# with NO propagation-delay leg — footsteps play immediately, unlike fire
# [orig: the odd/even-tick consumers call Entity_PlaySound3D_FullVolume
# @ 0x528e20 directly]. The local player's own body sounds DO play (retail
# plays your own steps; only fire has an action-slot presentation to defer to).
# Slots 43/44 (chute flap / freefall) refire every body tick by design; the
# exclusive key folds the refires into one continuous voice (D-SND-10).
func present_slot_sounds(events: Array) -> void:
	if events.is_empty():
		return
	var audio: MissionAudio = _audio_provider.call() \
			if _audio_provider.is_valid() else null
	if audio == null:
		return
	for ev_v in events:
		var ev: Dictionary = ev_v
		var set_name := String(ev.get("set", ""))
		if set_name.is_empty():
			continue
		var slot := int(ev.get("slot", 0))
		var key := ""
		if slot == 43 or slot == 44:
			key = "%d:%d" % [int(ev.get("handle", 0)), slot]
		if audio.slot_soundset(set_name, ev.get("pos", Vector3.ZERO), key):
			_stats.sounds += 1


# Entity-attached loop registrations (vehicle idle/drive/reverse today) share
# the native ambient emitter table and its loudest-eight physical pool. The
# audio layer replays the bounded latest intents at their producer ticks before
# advancing to the end of a catch-up frame, preserving the 30-tick keep-alive.
# [orig: SoundEmitter_RegisterSetLayers @0x528340;
# SoundEmitter_UpdateAndMixTop8 @0x5284a0]
func present_sound_emitters(events: Array) -> void:
	if events.is_empty():
		return
	var audio: MissionAudio = _audio_provider.call() \
			if _audio_provider.is_valid() else null
	if audio == null:
		return
	audio.apply_sound_emitters(events)


func present_fires(events: Array) -> void:
	if events.is_empty():
		return
	var fx: EffectWorld = _fx_provider.call() if _fx_provider.is_valid() else null
	for ev_v in events:
		var ev: Dictionary = ev_v
		# The local player's own fire is presented by the action-slot legs
		# [orig: ActionSlot_ExecuteActionTick @ 0x541A70 routing]; everyone
		# else's rides the ammo-def legs below. (The SOUND legs of every arm
		# run in the sim now — world/fire_sound.h — and arrive through
		# _drain_fire_sounds; this drain owns the EFFECT legs.)
		if bool(ev.get("is_local_player", false)):
			continue
		_stats.fires += 1
		var origin: Vector3 = ev.get("origin", Vector3.ZERO)
		var effect := String(ev.get("effect", ""))
		# THE ARM SPLIT. Retail's round-event receive path has two mutually exclusive
		# arms and only one of them is the ammo-def pair. The adm-indexed arm spawns
		# no ammo-def effect: it executes the ADDRESSED def's FIRE action row
		# instead, at that weapon's own userpoint on the gfx3 model.
		# This matters because the wire position is the shooter's EYE — retail sends
		# Position + CameraOffset [orig: Entity_CalcWeaponFirePosition @0x4dc750] — so
		# running the ammo-def leg on this arm draws every remote muzzle flash out of
		# the shooter's face, roughly a metre behind the barrel.
		# [orig: arms @0x42f521 / @0x42f6ce; ammo effect @0x42f6c2;
		#  the fire row @0x42f777 / @0x42f98f]
		if bool(ev.get("adm_arm", false)):
			effect = String(ev.get("action_effect", ""))
			# The anchor: this shooter's held weapon, not the wire point — the
			# rendered gun's own userpoint, which is what retail spawns at (the
			# authority DECISION closing the S12a shadow seam: the rendered-node
			# anchor is permanent, the sim-posed re-derivation is gone). Falling
			# back to the wire eye position would reintroduce the very bug this
			# fixes, so an unresolvable anchor takes the provider's own
			# body-origin fallback — retail's deepest fallback is the entity
			# origin [orig: @0x401867..0x401887].
			var anchored := Vector3.INF
			if _muzzle_provider.is_valid():
				anchored = _muzzle_provider.call(
						int(ev.get("shooter_handle", -1)),
						String(ev.get("action_userpoint", "")))
			if anchored.is_finite():
				origin = anchored
		if fx != null and not effect.is_empty():
			# The muzzle effect at the fire origin along the fire direction
			# [orig: the 56-B spawn descriptor -> CEffectWorld_SpawnEmitterAtPosition
			# @ 0x5F6DF0; every fire spawns one — no per-shooter guard on this leg].
			fx.spawn_effect(effect, origin, ev.get("forward", Vector3.FORWARD))
			_stats.effects += 1
		# ev["mf_light"]: the muzzle glow light — tracked deferral (no light pool).


# The sim's ready fire sounds: immediate near shots, the adm-arm action-row
# sets, and expired propagation-delayed slots, gated and counted down on the
# logic clock (world/fire_sound.h). Each row plays positionally; the set's
# max-range cull runs in the audio bank (D-AI-8).
# [orig: Entity_PlaySound3D_FullVolume @ 0x528e20 / Sound_TickPendingSlots
#  @ 0x529310]
func present_fire_sounds(sounds: Array) -> void:
	if sounds.is_empty():
		return
	var audio: MissionAudio = _audio_provider.call() \
			if _audio_provider.is_valid() else null
	if audio == null:
		return
	for row_v in sounds:
		var row: Dictionary = row_v
		audio.fire_soundset(String(row.get("set", "")), row.get("pos", Vector3.ZERO),
				int(row.get("source_bms_id", 0)))
		_stats.sounds += 1


# The compiled per-family ribbon strips [orig: CEffectChannel_RenderRibbon
# @ 0x5DB8A0 — the math and the style tables live natively in
# engine/runtime/renderer/tracer_frame.cpp]. This pass drains the sim's trail
# rows, hands them with the camera to the native compile, and uploads each
# family's vertex run verbatim.
func draw_tracer_rows(rows: PackedFloat32Array) -> void:
	if _mesh == null:
		return
	_mesh.clear_surfaces()
	if rows.is_empty():
		return
	var cam := _listener_pos()
	if not cam.is_finite():
		cam = Vector3.ZERO
	var frame: Dictionary = Simulation.compile_tracer_ribbons(rows, cam)
	_stats.tracer_peak = maxi(int(_stats.tracer_peak), int(frame.get("channels", 0)))
	_emit_strip(frame.get("additive", {}), _mat_additive)
	_emit_strip(frame.get("alpha", {}), _mat_alpha)


func _emit_strip(family: Dictionary, mat: StandardMaterial3D) -> void:
	var positions: PackedVector3Array = family.get("positions", PackedVector3Array())
	var colors: PackedColorArray = family.get("colors", PackedColorArray())
	if positions.size() < 4:  # fewer than two pairs draws nothing
		return
	_mesh.surface_begin(Mesh.PRIMITIVE_TRIANGLE_STRIP, mat)
	for i in positions.size():
		_mesh.surface_set_color(colors[i])
		_mesh.surface_add_vertex(positions[i])
	_mesh.surface_end()


func _listener_pos() -> Vector3:
	if _listener_provider.is_valid():
		var v: Vector3 = _listener_provider.call()
		return v
	return Vector3.INF
