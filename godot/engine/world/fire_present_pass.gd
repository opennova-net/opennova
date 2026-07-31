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
# shooter (forcetracer 0x8000 = every round) is visible in flight — a channel in the
# tracer/trail emitter pool [orig: g_TracerEmitterPool @ 0x2BF5270], styled by the
# ammo 'tracer_type <friendly> [enemy]' id selected against the LOCAL player's team
# at spawn (@ 0x4ec740; shooter == local player counts friendly — our sim runs the
# same select, world/tracer_trails.h). The sim owns the point rings (one pre-move
# point per 62 Hz tick, drain after death); this pass builds the witnessed
# camera-facing ribbons [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0]: per point a
# +/- right pair, right = normalize(cross(segment dir, camera - point)), half-width
# = max(style size curve x point jitter, distance x min-screen-width), vertex color
# from the style's ARGB ramp indexed by (count - i) + age - 1 (the same ramp is the
# along-trail gradient AND the post-death fade), the oldest pair takes the style
# base color, triangle strip, untextured vertex color (the B=0 path writes no UVs —
# the stock trail shader cannot be sampling a texture). Additive styles (std/rapid/
# sniper/df1/NVG) draw fog-to-black additive [orig: SetFogAndBlendMode mode 2 —
# ONE:ONE, alpha unused; our disable_fog stand-in = D-AI-12c]; smoke styles
# (rocket/at4/grenade) draw alpha-blended with scene fog [orig: mode 0]. The 4-wide
# wave-animated smoke/sniper cross-section and the distortion pass (style +0x828)
# are tracked in docs/world/world-wac-ai-re.md §24.6 (D-AI-12a/b); the round's item
# graphic (frndlyTrcrID/foeTrcrID + TRACER_SCALE/TRACER_WIDTH nodes) and the
# light_move glow (round+0x1B4) are D-AI-12d/e. The SP host shows tracers
# unconditionally (the MP NoTracers rules bit, dword_24D1E34 & 1, is a net seam
# wired via RoundSim.no_tracers_rule).

const SOUND_DELAY_MIN_DIST := 30.0  # units [orig: dist >= 0x1E gate @ 0x528ed4]
const SOUND_SPEED := 330.0          # units/s [orig: g_SoundSpeedFixed = 330.0 16.16]

# Min half-width per unit of camera distance — the witnessed screen-size clamp
# [orig: |camera - point| (16.16) x 1.83e-8 / proj scale @ CEffectChannel_RenderRibbon;
# ~0.0012/u at the shipped projection — the proj divisor is D-AI-12g].
const TRACER_MIN_WIDTH_PER_DIST := 0.0012

var _sim                        # NovaSimulation (drain + trail source)
var _audio_provider := Callable()     # -> NovaMissionAudio (or null)
var _fx_provider := Callable()        # -> NovaEffectWorld (or null)
var _listener_provider := Callable()  # -> Vector3 listener position (camera)
# (wire_handle: int, userpoint: String) -> Vector3 world muzzle point, or Vector3.INF.
# Retail's adm arm spawns at the shooter's own WEAPON, not at the wire position, so
# presentation needs a way back to that body's held-weapon node.
var _muzzle_provider := Callable()
var _mesh: ImmediateMesh
var _mesh_instance: MeshInstance3D
var _mat_additive: StandardMaterial3D  # std/rapid/sniper/df1/NVG [orig: fog-black additive]
var _mat_alpha: StandardMaterial3D     # rocket/at4/grenade smoke [orig: alpha + scene fog]
var _pending: Array = []        # [{ticks, set, pos, source_bms_id}] pending sounds
# Probe/diagnostic counters (ai_threat_probe asserts the presentation actually ran).
var _stats := {"fires": 0, "sounds": 0, "delayed_sounds": 0, "effects": 0, "tracer_peak": 0}

# The witnessed per-style ribbon tables, id 1..12 = the ammo.def tracer_type ids
# (stdred/stdgreen/rocket/at4/grenade/rapidred/rapidgreen/8=NVG laser/sniperred/
# snipergreen/df1red/df1green; any other id takes the stdred block) [orig: the
# 0x830-B style blocks — static .data g_TracerStyle_Std*/Rapid*/Sniper* @ 0x8437F0..
# 0x8460E0, runtime-built DF1/rocket/at4/grenade/NVG in
# CEffectEmitterPool_ResetAndBuildStyles @ 0x5DB3A0]. Per style: the ARGB color ramp
# (+0x18, head index 0 = the ramp start; table length +0x10 = the ring cap), the
# float half-width curve (+0x41C, count +0x418), the base color (+0x14, the oldest
# vertex pair), and the additive flag (+0 — fog-black additive vs alpha smoke).
static var _styles := _build_styles()


static func _build_styles() -> Array:
	# index 0 = the fallback (stdred — the CEffectChannel_Init default case @ 0x5db1c8).
	var styles: Array = []
	styles.resize(13)
	var stdred := {
		"colors": PackedInt32Array([0x000000, 0xE08080, 0xC08080, 0xA04040, 0x802020,
			0x601010, 0x200000, 0x100000, 0x100000, 0x100000, 0x080000, 0x000000]),
		"sizes": PackedFloat32Array([0.02]), "base": 0, "additive": true,
	}
	var stdgreen := {
		"colors": PackedInt32Array([0x000000, 0x80A080, 0x808880, 0x407040, 0x205820,
			0x104010, 0x001400, 0x000800, 0x000800, 0x000800, 0x000400, 0x000000]),
		"sizes": PackedFloat32Array([0.02]), "base": 0, "additive": true,
	}
	styles[0] = stdred
	styles[1] = stdred
	styles[2] = stdgreen
	# Rocket/AT4: 112 gray 0xC0C0C0 entries, quadratic alpha fade a=((255-2i)^2)>>8;
	# linear width growth [orig: @ 0x5db4f8-0x5db5f6].
	var smoke_colors := PackedInt32Array()
	smoke_colors.resize(112)
	for i in 112:
		var v := 255 - 2 * i
		smoke_colors[i] = ((v * v) >> 8 << 24) | 0xC0C0C0
	var rocket_sizes := PackedFloat32Array()
	rocket_sizes.resize(32)
	var at4_sizes := PackedFloat32Array()
	at4_sizes.resize(32)
	for i in 32:
		rocket_sizes[i] = i * 0.0625
		at4_sizes[i] = i * 0.03125
	styles[3] = {"colors": smoke_colors, "sizes": rocket_sizes,
			"base": 0xC0C0C0, "additive": false}
	styles[4] = {"colors": smoke_colors, "sizes": at4_sizes,
			"base": 0xC0C0C0, "additive": false}
	# Grenade: 64 grays, cubic alpha fade a=(192*(255-3i)^3)>>24 [orig: @ 0x5db648].
	var gren_colors := PackedInt32Array()
	gren_colors.resize(64)
	for i in 64:
		var s := 255 - 3 * i
		gren_colors[i] = ((192 * s * s * s) >> 24 << 24) | 0xC0C0C0
	var gren_sizes := PackedFloat32Array()
	gren_sizes.resize(32)
	for i in 32:
		gren_sizes[i] = i * 0.0078125
	styles[5] = {"colors": gren_colors, "sizes": gren_sizes,
			"base": 0xC0C0C0, "additive": false}
	styles[6] = {
		"colors": PackedInt32Array([0x000000, 0xE08080, 0xA04040, 0x200000, 0x100000,
			0x000000]),
		"sizes": PackedFloat32Array([0.02]), "base": 0, "additive": true,
	}
	styles[7] = {
		"colors": PackedInt32Array([0x000000, 0x80A080, 0x407040, 0x001400, 0x000800,
			0x000000]),
		"sizes": PackedFloat32Array([0.02]), "base": 0, "additive": true,
	}
	# The NVG laser style (numeric id 8): 0xFF2020 red, alpha ramps UP 6/entry
	# [orig: @ 0x5db6f7-0x5db711].
	var nvg_colors := PackedInt32Array()
	nvg_colors.resize(32)
	for i in 32:
		nvg_colors[i] = (6 * i << 24) | 0xFF2020
	styles[8] = {"colors": nvg_colors, "sizes": PackedFloat32Array([0.01]),
			"base": 0xC04040, "additive": true}
	# Sniper: dim high-alpha ramps, width 0.04 -> 0.1 over 10 [orig: .data @ 0x8458C8].
	var sniper_red := PackedInt32Array([0xFF180000, 0xFF180000, 0xFF180000, 0xFF180000,
		0xF0070000, 0xE0070000, 0xD0060000, 0xC0060000, 0xB0050000, 0xA0050000,
		0x90040000, 0x80040000, 0x70030000, 0x60030000, 0x50020000, 0x40020000,
		0x30010000, 0x20010000, 0x10000000, 0x00000000])
	var sniper_green := PackedInt32Array()
	sniper_green.resize(20)
	for i in 20:
		var c := sniper_red[i]
		sniper_green[i] = (c & 0xFF000000) | ((c >> 16 & 0xFF) << 8)
	var sniper_sizes := PackedFloat32Array([0.04, 0.05, 0.06, 0.07, 0.08, 0.09,
		0.1, 0.1, 0.1, 0.1])
	styles[9] = {"colors": sniper_red, "sizes": sniper_sizes, "base": 0, "additive": true}
	styles[10] = {"colors": sniper_green, "sizes": sniper_sizes, "base": 0, "additive": true}
	# DF1: 32-entry red/green fades, thin constant width [orig: @ 0x5db3d9-0x5db4d6].
	var df1_red := PackedInt32Array()
	var df1_green := PackedInt32Array()
	df1_red.resize(32)
	df1_green.resize(32)
	df1_red[0] = 0
	df1_green[0] = 0
	for i in range(1, 32):
		var hi := (32 - i) * 8 >> 1
		var lo := (32 - i) * 8 >> 2
		df1_red[i] = (hi << 16) | (lo << 8) | lo
		df1_green[i] = (lo << 16) | (hi << 8) | lo
	styles[11] = {"colors": df1_red, "sizes": PackedFloat32Array([0.006]),
			"base": 0, "additive": true}
	styles[12] = {"colors": df1_green, "sizes": PackedFloat32Array([0.006]),
			"base": 0, "additive": true}
	return styles


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


func setup(sim, container: Node3D, audio_provider: Callable, fx_provider: Callable,
		listener_provider: Callable, muzzle_provider := Callable()) -> void:
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
	_pending.clear()


## Once per present (beside the other passes), after the sim advanced. `ticks` is
## the number of logic ticks in the batch (tick_realtime catch-up runs several per
## present) — the pending-sound countdown consumes logic ticks, not presents.
func present(ticks: int = 1) -> void:
	if _sim == null:
		return
	_drain_fires()
	_drain_slot_sounds()
	_drain_sound_emitters()
	_tick_pending_sounds(ticks)
	_draw_tracers()


# The body slot-sound drain (footsteps/foley/landing thumps/death screams): the
# sim resolves each entity's SndProf.def profile slot to its authored set name
# and emits the (foot-level) position; this plays them full-volume positional
# with NO propagation-delay leg — footsteps play immediately, unlike fire
# [orig: the odd/even-tick consumers call Entity_PlaySound3D_FullVolume
# @ 0x528e20 directly]. The local player's own body sounds DO play (retail
# plays your own steps; only fire has an action-slot presentation to defer to).
# Slots 43/44 (chute flap / freefall) refire every body tick by design; the
# exclusive key folds the refires into one continuous voice (D-SND-10).
func _drain_slot_sounds() -> void:
	if not _sim.has_method("drain_slot_sounds"):
		return  # stale native DLL — presentation degrades silently, sim unaffected
	var events: Array = _sim.drain_slot_sounds()
	if events.is_empty():
		return
	var audio = _audio_provider.call() if _audio_provider.is_valid() else null
	if audio == null or not audio.has_method("slot_soundset"):
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
func _drain_sound_emitters() -> void:
	var events: Array = _sim.drain_sound_emitters()
	if events.is_empty():
		return
	var audio = _audio_provider.call() if _audio_provider.is_valid() else null
	if audio == null:
		return
	audio.apply_sound_emitters(events)


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
		var effect := String(ev.get("effect", ""))
		# THE ARM SPLIT. Retail's round-event receive path has two mutually exclusive
		# arms and only one of them is the ammo-def pair. The adm-indexed arm plays no
		# ammo-def sound and spawns no ammo-def effect: it executes the ADDRESSED def's
		# FIRE action row instead, at that weapon's own userpoint on the gfx3 model.
		# This matters because the wire position is the shooter's EYE — retail sends
		# Position + CameraOffset [orig: Entity_CalcWeaponFirePosition @0x4dc750] — so
		# running the ammo-def leg on this arm draws every remote muzzle flash out of
		# the shooter's face, roughly a metre behind the barrel.
		# [orig: arms @0x42f521 / @0x42f6ce; ammo legs @0x42f5dc / @0x42f6c2;
		#  the fire row @0x42f777 / @0x42f98f]
		if bool(ev.get("adm_arm", false)):
			sound_set = String(ev.get("action_sound_set", ""))
			effect = String(ev.get("action_effect", ""))
			# The anchor: this shooter's held weapon, not the wire point. Falling back
			# to the wire eye position would reintroduce the very bug this fixes, so an
			# unresolvable anchor takes the provider's own body-origin fallback —
			# retail's deepest fallback is the entity origin [orig: @0x401867..0x401887].
			if _muzzle_provider.is_valid():
				var anchored: Vector3 = _muzzle_provider.call(
						int(ev.get("shooter_handle", -1)),
						String(ev.get("action_userpoint", "")))
				if anchored.is_finite():
					origin = anchored
		var source_bms_id := int(ev.get("source_bms_id", 0))
		if audio != null and not sound_set.is_empty():
			# Propagation delay for far shots [orig: @ 0x528ed4-0x528ef2:
			# >= 30 u -> pending slot, (62*dist/330)>>2 ticks; else immediate].
			var dist := origin.distance_to(listener) if listener.is_finite() else 0.0
			if dist >= SOUND_DELAY_MIN_DIST:
				var delay_ticks := int(62.0 * dist / SOUND_SPEED) >> 2
				if delay_ticks > 0:
					_pending.append({
						"ticks": delay_ticks,
						"set": sound_set,
						"pos": origin,
						"source_bms_id": source_bms_id,
					})
					_stats.delayed_sounds += 1
				else:
					audio.fire_soundset(sound_set, origin, source_bms_id)
					_stats.sounds += 1
			else:
				audio.fire_soundset(sound_set, origin, source_bms_id)
				_stats.sounds += 1
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
			audio.fire_soundset(
				String(p["set"]), p["pos"], int(p.get("source_bms_id", 0)))
	_pending = still


# The witnessed ribbon build [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0, the
# normal pass]: per channel, one +/- right vertex pair per point over points
# [0 .. count-2] (the newest point steers direction only — the visible head is the
# second-newest point, one tick behind the pre-move append), right =
# normalize(cross(dir_to_next, camera - point)), half-width =
# max(size[idx] * point.w, dist * min-width), color = ramp[(count - i) + age - 1]
# clamped (the oldest pair takes the style base color), triangle strip. Channels
# join into one strip surface per blend family via degenerate pairs.
func _draw_tracers() -> void:
	if _mesh == null or not _sim.has_method("get_tracer_trails"):
		return
	_mesh.clear_surfaces()
	var rows: PackedFloat32Array = _sim.get_tracer_trails()
	if rows.is_empty():
		return
	var cam := _listener_pos()
	if not cam.is_finite():
		cam = Vector3.ZERO
	var additive_verts: Array = []  # interleaved [Vector3, Color, ...]
	var alpha_verts: Array = []
	var channels := 0
	var i := 0
	while i + 2 < rows.size():
		var style_id := int(rows[i])
		var age := int(rows[i + 1])
		var count := int(rows[i + 2])
		i += 3
		if count <= 0 or i + count * 4 > rows.size():
			break
		channels += 1
		var style: Dictionary = _styles[style_id] if style_id >= 1 and style_id <= 12 \
				else _styles[0]
		var sink: Array = additive_verts if bool(style["additive"]) else alpha_verts
		_append_channel_ribbon(sink, rows, i, count, age, style, cam)
		i += count * 4
	_stats.tracer_peak = maxi(int(_stats.tracer_peak), channels)
	_emit_strip(additive_verts, _mat_additive, true)
	_emit_strip(alpha_verts, _mat_alpha, false)


func _append_channel_ribbon(sink: Array, rows: PackedFloat32Array, base: int,
		count: int, age: int, style: Dictionary, cam: Vector3) -> void:
	if count < 2:
		return
	var colors: PackedInt32Array = style["colors"]
	var sizes: PackedFloat32Array = style["sizes"]
	var base_argb := int(style["base"])
	var last_right := Vector3.ZERO
	var first := true
	for pi in range(count - 1):
		var o := base + pi * 4
		var p := Vector3(rows[o], rows[o + 1], rows[o + 2])
		var jitter := rows[o + 3]
		var nxt := Vector3(rows[o + 4], rows[o + 5], rows[o + 6])
		# Ramp index [orig: (count - i) + age - 1, clamped per table @ 0x5db8a0].
		var idx := count - pi + age - 1
		var color_idx := clampi(idx, 0, colors.size() - 1)
		var size_idx := clampi(idx, 0, sizes.size() - 1)
		# The oldest pair takes the style base color [orig: i == 0 -> desc+0x14].
		var argb := base_argb if pi == 0 else colors[color_idx]
		var dir := nxt - p
		var right: Vector3
		if dir.length_squared() > 0.000001:
			right = dir.cross(cam - p)
			right = right.normalized() if right.length_squared() > 0.000001 else last_right
		else:
			right = last_right  # duplicate points (the death double-append) keep facing
		last_right = right
		var half_w := maxf(sizes[size_idx] * jitter,
				p.distance_to(cam) * TRACER_MIN_WIDTH_PER_DIST)
		var col := _argb_to_color(argb, bool(style["additive"]))
		var a := p + right * half_w
		var b := p - right * half_w
		if first and not sink.is_empty():
			# Degenerate join from the previous channel's last vertex.
			sink.append(sink[sink.size() - 2])
			sink.append(sink[sink.size() - 2])
			sink.append(a)
			sink.append(col)
		first = false
		sink.append(a)
		sink.append(col)
		sink.append(b)
		sink.append(col)


func _emit_strip(verts: Array, mat: StandardMaterial3D, _additive: bool) -> void:
	if verts.size() < 8:  # fewer than two pairs draws nothing
		return
	_mesh.surface_begin(Mesh.PRIMITIVE_TRIANGLE_STRIP, mat)
	var i := 0
	while i + 1 < verts.size():
		_mesh.surface_set_color(verts[i + 1])
		_mesh.surface_add_vertex(verts[i])
		i += 2
	_mesh.surface_end()


# Style-table ARGB -> Godot color. Additive styles render ONE:ONE in retail (alpha
# byte unused — the std ramps author alpha 0), so alpha is forced to 1.0 under
# Godot's SRCALPHA:ONE add; smoke styles carry their authored alpha fade.
func _argb_to_color(argb: int, additive: bool) -> Color:
	var r := float((argb >> 16) & 0xFF) / 255.0
	var g := float((argb >> 8) & 0xFF) / 255.0
	var b := float(argb & 0xFF) / 255.0
	var a := 1.0 if additive else float((argb >> 24) & 0xFF) / 255.0
	return Color(r, g, b, a)


func _listener_pos() -> Vector3:
	if _listener_provider.is_valid():
		var v = _listener_provider.call()
		if v is Vector3:
			return v
	return Vector3.INF
