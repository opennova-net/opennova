extends RefCounted

# NovaObjectModel's main-body skeletal-animation section (quality slice
# W4-6c, the merged W4-6a/6b verbatim-motion shape): clip play/variant/
# seeded selection, the retail remote body-state arbitration, the muzzle
# userpoint, playhead scrub, the PLAYPARTANIM part-anim channels, the
# aim-overlay/weapon-channel setters, and advance_body_animation pose
# evaluation. ALL state, constants and signals stay on the composing
# NovaObjectModel, reached through `_m`; every moved method keeps an owner
# delegate, so the external surface (and test-subclass overrides, e.g.
# _resolve_anim_channel_register) are unchanged. The owner is a Node3D --
# manually managed, not refcounted -- so this plain back-reference cannot
# cycle: the owner owns the helper; the helper points back at the Node.

var _m


func _init(model) -> void:
	_m = model


# --- Main-body skeletal animation (.bad/.adm) -----------------------------------
# A NovaSkeletalAnim carries the parsed/sampled skeleton + clips for this model. Setting
# it (then a skinned model) makes rebuild() build the Skeleton3D + Skin; play_body_clip
# selects the active clip the per-frame pass poses. Both the object-editor preview and the
# mission present pass drive these, so organic bodies animate from one path.
func set_skeletal_anim(skeletal) -> void:
	_m._skeletal = skeletal
	reset_remote_body_state()
	_m._anim_key = ""
	_m._anim_variant = 0
	_m._anim_time = 0.0
	_m._anim_playing = false
	_m._anim_external_phase = false
	_m._body_phase_stamp_valid = false
	_clear_body_blend()
	_m._last_slot_resolved = -1
	_m._last_slot_key = ""
	_m._body_pose_dirty = true
	_m.rebuild()


func get_skeletal_anim():
	return _m._skeletal


func get_skeleton() -> Skeleton3D:
	return _m._skeleton


func has_skeleton() -> bool:
	return _m._skeleton != null


## Whether this model resolved a gun-flash muzzle userpoint onto its skeleton
## (the D-AI-6 fire-origin seam; infantry body models author one — US01
## "GFlash01", SASBODY1 "MFlash01").
func has_muzzle() -> bool:
	return _m._muzzle_bone >= 0 and _m._skeleton != null


## The POSED muzzle world position: the authored model-space userpoint carried
## through its bone's live pose — the same rest-to-pose attachment transform the
## userpoint debug overlay and the action-particle attachments use.
## [orig: Entity_GetAttachmentWorldPosition @0x4b2670 — userpoint local position
## x the animated bone matrix]
func get_muzzle_world_position() -> Vector3:
	var model_to_world: Transform3D = (_m._skeleton.global_transform
			* _m._skeleton.get_bone_global_pose(_m._muzzle_bone)
			* _m._skeleton.get_bone_global_rest(_m._muzzle_bone).affine_inverse())
	return model_to_world * _m._muzzle_model_pos


# Resolve the muzzle userpoint against the built skeleton. Name preference:
# a "*flash*" userpoint (the gun-flash convention) over "bullet"/"*muzzle*";
# LOOK/CAMERA/etc never match. The rig is index-driven, so the userpoint's
# subobject row IS the skeleton bone index; rows past the bone count (padding
# rows exist in shipped models) disqualify the point.
func _resolve_muzzle_userpoint() -> void:
	_m._muzzle_bone = -1
	if _m._skeleton == null or _m.object_data == null:
		return
	var best := -1
	var best_rank := 99
	for i in range(int(_m.object_data.get_user_point_count())):
		var info: Dictionary = _m.object_data.get_user_point_info(i)
		var n := String(info.get("name", "")).to_lower()
		var rank := 99
		if n.contains("flash"):
			rank = 0
		elif n == "bullet" or n.contains("muzzle"):
			rank = 1
		if rank < best_rank:
			best_rank = rank
			best = i
	if best < 0:
		return
	var info2: Dictionary = _m.object_data.get_user_point_info(best)
	var bone := int(info2.get("subobject", -1))
	if bone < 0 or bone >= _m._skeleton.get_bone_count():
		return
	_m._muzzle_bone = bone
	_m._muzzle_model_pos = info2.get("position", Vector3.ZERO)


## Play a main-body clip by ADM key (e.g. "anim_walk"). Missing semantic keys
## use this ADM's RESET binding when present.
func play_body_clip(key: String) -> void:
	play_body_clip_variant(key, 0)


## play_body_clip selecting a same-key VARIANT (multi-clip .adm rows): the FSM owner's
## ring serves the index and playback follows that latch until the next play — a
## variant change re-poses even on the same key. [orig: AnimMap_PlayAnimBySlot
## @0x40bda0 latches the served ring entry at animState+68]
func play_body_clip_variant(key: String, variant: int) -> void:
	if variant == 0:
		key = _resolve_body_clip_key(key)
	if _m._skeletal == null or key.is_empty() or not _m._skeletal.has_clip(key):
		return
	_clear_body_blend()
	if key == _m._anim_key and variant == _m._anim_variant:
		_m._anim_external_phase = false
		_m._body_phase_stamp_valid = false
		_m._anim_playing = true
		return
	_m._anim_key = key
	_m._anim_variant = variant
	_m._anim_time = 0.0
	_m._anim_playing = true
	_m._anim_external_phase = false
	_m._body_phase_stamp_valid = false
	_m._body_pose_dirty = true


## Pose a selected clip variant at an authoritative time. Unlike
## set_animation_time(), this keeps render-frame _process(delta) from advancing
## the playhead; the fixed-tick weapon presenter supplies every later phase.
func play_body_clip_variant_at_time(key: String, variant: int, seconds: float) -> void:
	if _m._skeletal == null or not _m._skeletal.has_clip(key):
		return
	_clear_body_blend()
	var same_external: bool = (_m._anim_external_phase and key == _m._anim_key
			and variant == _m._anim_variant
			and is_equal_approx(_m._anim_time, seconds))
	_m._anim_key = key
	_m._anim_variant = variant
	_set_body_playhead(seconds)
	_m._anim_playing = false
	_m._anim_external_phase = true
	if same_external and not _m._body_pose_dirty:
		return
	_m._body_pose_dirty = true
	advance_body_animation(0.0)


## Pose a main-body clip at the authoritative infantry motor playhead. IDA's
## AnimMap phase advances in half-frame ticks, so seconds = ticks / (2 * clip_fps).
## The model does not free-run this clip between sim snapshots.
func play_body_clip_at(key: String, phase_ticks: int) -> void:
	key = _resolve_body_clip_key(key)
	if key.is_empty():
		return
	_clear_body_blend()
	# Repeat-call fast path: the stamp proves this exact (key, tick) pair is what
	# posed the skeleton last, nothing else touched the playhead since, and no
	# other input dirtied the pose — the full body below would be a no-op.
	if (_m._body_phase_stamp_valid and _m._anim_external_phase
			and not _m._body_pose_dirty
			and phase_ticks == _m._body_phase_ticks_applied
			and key == _m._anim_key):
		return
	var previous_key: String = _m._anim_key
	var previous_time: float = _m._anim_time
	var previous_external: bool = _m._anim_external_phase
	var fps: float = _m._skeletal.get_clip_fps(key)
	var seconds := 0.0
	if fps > 0.0:
		seconds = float(maxi(phase_ticks, 0)) / (2.0 * fps)
	var same_external := previous_external and key == previous_key and is_equal_approx(previous_time, seconds)
	_m._anim_key = key
	_m._anim_variant = 0  # stamp-driven body path: variant rings deferred to the 3P channel
	_set_body_playhead(seconds)
	_m._anim_playing = false
	_m._anim_external_phase = true
	_m._body_phase_stamp_valid = true
	_m._body_phase_ticks_applied = phase_ticks
	if same_external and not _m._body_pose_dirty:
		return
	_m._body_pose_dirty = true
	advance_body_animation(0.0)


## Pose the authoritative outgoing + incoming PRIMARY channels at one shared
## blend weight. The two clips are sampled first; the weapon-mask channel and
## aim overlay compose once on top in advance_body_animation(), matching
## AnimChannel_BlendTwoChannels -> Entity_BuildBoneTransformMatrices.
##
## A missing semantic source or target uses this ADM's RESET binding. If RESET
## itself is absent, a valid remaining channel is retained. Weight 1 uses
## play_body_clip_at's stamped single-channel fast path, so steady-state
## presentation pays no second pose evaluation.
func play_body_blend_at(
		source_key: String, source_phase_ticks: int,
		target_key: String, target_phase_ticks: int,
		weight: float) -> void:
	if _m._skeletal == null:
		return
	source_key = _resolve_body_clip_key(source_key)
	target_key = _resolve_body_clip_key(target_key)
	var source_valid := not source_key.is_empty()
	var target_valid := not target_key.is_empty()
	if not target_valid:
		if source_valid:
			play_body_clip_at(source_key, source_phase_ticks)
		else:
			_reset_body_pose()
		return
	if not source_valid or weight >= 1.0:
		play_body_clip_at(target_key, target_phase_ticks)
		return

	_pose_body_blend_at_times(
			source_key, _clip_phase_seconds(source_key, source_phase_ticks),
			target_key, _clip_phase_seconds(target_key, target_phase_ticks),
			weight)


func _pose_body_blend_at_times(
		source_key: String, source_time: float,
		target_key: String, target_time: float,
		weight: float) -> void:
	_m._anim_key = target_key
	_m._anim_variant = 0
	_set_body_playhead(target_time)
	_m._anim_playing = false
	_m._anim_external_phase = true
	_m._body_phase_stamp_valid = false
	_m._body_blend_source_key = source_key
	_m._body_blend_source_time = source_time
	_m._body_blend_weight = clampf(weight, 0.0, 1.0)
	_m._body_pose_dirty = true
	advance_body_animation(0.0)


## Seed a main-body clip from retail half-frame ticks, pose it immediately, and
## leave it free-running. Distinct from play_body_clip_at(), whose callers own
## every later playhead sample and therefore intentionally pin external phase.
func play_body_clip_seeded(key: String, phase_ticks: int) -> void:
	if _select_body_clip_seeded(key, phase_ticks):
		advance_body_animation(0.0)


func _select_body_clip_seeded(key: String, phase_ticks: int) -> bool:
	key = _resolve_body_clip_key(key)
	if key.is_empty():
		return false
	_clear_body_blend()
	_m._anim_key = key
	_m._anim_variant = 0
	_set_body_playhead(_clip_phase_seconds(key, phase_ticks))
	_m._anim_playing = true
	_m._anim_external_phase = false
	_m._body_pose_dirty = true
	return true


## Apply one raw compact-organic body-state request with retail's remote
## transition arbitration. A phase belongs only to an immediately accepted
## player transition; queued states promote at tick zero when the current clip
## reaches its completion boundary.
func apply_remote_body_state(state_id: int, key: String, flags: int,
		phase_ticks: int = -1) -> bool:
	if state_id < 0 or _resolve_body_clip_key(key).is_empty():
		return remote_body_needs_fixed_tick()
	if _m._remote_state < 0:
		_accept_remote_body_state(state_id, key, flags, phase_ticks)
		return remote_body_needs_fixed_tick()
	if state_id == _m._remote_state:
		_clear_remote_body_pending()
		return remote_body_needs_fixed_tick()
	if ((_m._remote_flags & 0x4) != 0
			or ((_m._remote_flags & 0x20) != 0 and (flags & 0x1) == 0)):
		_queue_remote_body_state(state_id, key, flags)
		return remote_body_needs_fixed_tick()
	_accept_remote_body_state(state_id, key, flags, phase_ticks)
	return remote_body_needs_fixed_tick()


func reset_remote_body_state() -> void:
	_m._remote_state = -1
	_m._remote_flags = 0
	_clear_remote_body_pending()
	_clear_remote_body_blend()


func _accept_remote_body_state(state_id: int, key: String, flags: int,
		phase_ticks: int) -> void:
	_clear_remote_body_pending()
	var had_current: bool = (
			_m._remote_state >= 0 and not _m._anim_key.is_empty())
	_m._remote_state = state_id
	_m._remote_flags = flags
	var target_phase := phase_ticks if phase_ticks >= 0 else 0
	if had_current:
		_start_remote_body_blend(key, flags, target_phase)
	elif _select_body_clip_seeded(key, target_phase):
		advance_body_animation(0.0)


func _queue_remote_body_state(state_id: int, key: String, flags: int) -> void:
	_m._remote_pending_state = state_id
	_m._remote_pending_key = key
	_m._remote_pending_flags = flags
	var length: float = _m._skeletal.get_clip_length(_m._anim_key, _m._anim_variant)
	if length <= 0.0:
		# A hold clip whose length cannot resolve completes IMMEDIATELY — an INF
		# deadline here wedged the remote body-state machine forever (every later
		# stance/anim request queued behind it), freezing the remote player's pose
		# for the rest of the session. Retail's hold ends with the animation; a
		# zero-length animation is already over.
		_m._remote_pending_end_time = 0.0
	elif _m._skeletal.is_clip_looping(_m._anim_key, _m._anim_variant):
		_m._remote_pending_end_time = (floorf(_m._anim_time / length) + 1.0) * length
	else:
		_m._remote_pending_end_time = length
	# A request arriving after a one-shot already ended promotes immediately.
	if _promote_remote_body_pending_if_due():
		advance_body_animation(0.0)


func _clear_remote_body_pending() -> void:
	_m._remote_pending_state = -1
	_m._remote_pending_key = ""
	_m._remote_pending_flags = 0
	_m._remote_pending_end_time = INF


func _clear_remote_body_blend() -> void:
	_m._remote_blend_active = false
	_m._remote_blend_source_key = ""
	_m._remote_blend_source_phase_ticks = 0
	_m._remote_blend_source_time = 0.0
	_m._remote_blend_target_phase_ticks = 0
	_m._remote_blend_weight = 1.0
	_m._remote_blend_step = 0.0
	_clear_body_blend()


func _body_phase_ticks(key: String, seconds: float) -> int:
	if _m._skeletal == null or key.is_empty():
		return 0
	var fps: float = _m._skeletal.get_clip_fps(key)
	return maxi(int(floor(seconds * 2.0 * fps + 0.000001)), 0) \
			if fps > 0.0 else 0


func _f32(value: float) -> float:
	return PackedFloat32Array([value])[0]


func _start_remote_body_blend(
		target_key: String, target_flags: int, target_phase_ticks: int) -> void:
	target_key = _resolve_body_clip_key(target_key)
	if target_key.is_empty():
		return
	# A retarget during A->B retains A and replaces only B with C.
	var source_key: String = (
			_m._remote_blend_source_key
			if _m._remote_blend_active else _m._anim_key)
	var source_phase: int = (
			_m._remote_blend_source_phase_ticks
			if _m._remote_blend_active
			else _body_phase_ticks(_m._anim_key, _m._anim_time))
	var source_time: float = (
			_m._remote_blend_source_time
			if _m._remote_blend_active else _m._anim_time)
	if (source_key.is_empty() or _m._skeletal == null
			or not _m._skeletal.has_clip(source_key)):
		_clear_remote_body_blend()
		if _select_body_clip_seeded(target_key, target_phase_ticks):
			advance_body_animation(0.0)
		return
	_m._remote_blend_active = true
	_m._remote_blend_source_key = source_key
	_m._remote_blend_source_phase_ticks = source_phase
	_m._remote_blend_source_time = source_time
	_m._remote_blend_target_phase_ticks = target_phase_ticks
	_m._remote_blend_weight = 0.0
	_m._remote_blend_step = _f32(
			1.0 / 15.0 if (target_flags & 0x400) != 0 else 0.1)
	_pose_body_blend_at_times(
			_m._remote_blend_source_key, _m._remote_blend_source_time,
			target_key,
			_clip_phase_seconds(
					target_key, _m._remote_blend_target_phase_ticks),
			0.0)


## Advance one receive-side simulation tick for an already accepted target.
## Both channels advance before the float32 target weight increments. The state
## guard lets an arriving retarget replace B with C at weight zero instead of
## accidentally consuming a tick from the old transition first.
func advance_remote_body_blend_tick(state_id: int) -> bool:
	if not _m._remote_blend_active:
		if _m._remote_pending_state >= 0:
			_promote_remote_body_pending_if_due()
			return remote_body_needs_fixed_tick()
		return false
	if state_id != _m._remote_state and state_id != _m._remote_pending_state:
		return false
	_m._remote_blend_source_phase_ticks += 1
	_m._remote_blend_source_time += _clip_half_tick_seconds(
			_m._remote_blend_source_key)
	_m._remote_blend_target_phase_ticks += 1
	# PackedFloat32Array performs the same IEEE-754 single-precision rounding as
	# retail's accumulated channel weight. This allocation exists only for the
	# ten/fifteen transition ticks; steady-state never enters this method.
	_m._remote_blend_weight = minf(_f32(
			_m._remote_blend_weight + _m._remote_blend_step), 1.0)
	var target_key: String = _m._anim_key
	if _m._remote_blend_weight >= 1.0:
		var target_phase: int = _m._remote_blend_target_phase_ticks
		_clear_remote_body_blend()
		if _select_body_clip_seeded(target_key, target_phase):
			advance_body_animation(0.0)
		return remote_body_needs_fixed_tick()
	_pose_body_blend_at_times(
			_m._remote_blend_source_key, _m._remote_blend_source_time,
			target_key,
			_clip_phase_seconds(
					target_key, _m._remote_blend_target_phase_ticks),
			_m._remote_blend_weight)
	return remote_body_needs_fixed_tick()


func remote_body_needs_fixed_tick() -> bool:
	return _m._remote_blend_active or _m._remote_pending_state >= 0


func _promote_remote_body_pending_if_due() -> bool:
	if _m._remote_pending_state < 0 or is_inf(_m._remote_pending_end_time):
		return false
	if _m._anim_time + 0.000001 < _m._remote_pending_end_time:
		return false
	var state_id: int = _m._remote_pending_state
	var key: String = _m._remote_pending_key
	var flags: int = _m._remote_pending_flags
	_clear_remote_body_pending()
	_m._remote_state = state_id
	_m._remote_flags = flags
	# The queued packet's phase described the old current channel. Retail starts
	# the promoted request at the first frame and discards any overshoot.
	_start_remote_body_blend(key, flags, 0)
	return true


func stop_body_clip() -> void:
	_m._anim_playing = false
	_m._anim_external_phase = false
	_m._body_phase_stamp_valid = false
	_clear_body_blend()
	reset_remote_body_state()


func get_active_body_clip() -> String:
	return _m._anim_key


## Play a main-body animation by canonical AI slot (opennova::world::BodyAnim). Resolves the
## slot to a clip key via the loaded NovaSkeletalAnim (with idle/reset fallback) and plays it.
## Idempotent: a repeated same-slot call (every present tick) does not restart a playing loop.
## No-op without a skeletal set or for slot < 0. This is the present pass's body-anim entry point.
func play_body_anim(slot: int) -> void:
	if _m._skeletal == null or slot < 0:
		return
	var key: String = _m._skeletal.slot_to_key(slot)
	if key.is_empty():
		return
	if key == _m._anim_key and not _m._anim_external_phase:
		return
	play_body_clip(key)


## Pose a main-body animation slot at the authoritative infantry motor playhead.
func play_body_anim_at(slot: int, phase_ticks: int) -> void:
	if _m._skeletal == null or slot < 0:
		return
	if slot != _m._last_slot_resolved:
		_m._last_slot_key = _m._skeletal.slot_to_key(slot)
		_m._last_slot_resolved = slot
	if _m._last_slot_key.is_empty():
		return
	play_body_clip_at(_m._last_slot_key, phase_ticks)


## Scrub the active body clip's playhead to `seconds` and pose IMMEDIATELY,
## even while paused (paused scrubbing is the point; while playing the
## zero-delta advance adds nothing). Mirrors eval_pose's own time handling so
## the stored playhead and the rendered pose can never disagree: looping clips
## wrap over the clip length, one-shots clamp to it. No-op without a skeletal
## set / active clip. Distinct from the _anim_time_ms material/PANM clock.
func set_animation_time(seconds: float) -> void:
	if _m._skeletal == null or _m._anim_key.is_empty():
		return
	_clear_body_blend()
	_m._anim_external_phase = false
	_set_body_playhead(seconds)
	_m._body_pose_dirty = true
	advance_body_animation(0.0)


func _set_body_playhead(seconds: float) -> void:
	# Any playhead write outside play_body_clip_at's own apply (which re-stamps
	# right after) invalidates the applied-phase stamp.
	_m._body_phase_stamp_valid = false
	var length: float = _m._skeletal.get_clip_length(_m._anim_key, _m._anim_variant)
	if length <= 0.0:
		_m._anim_time = 0.0
	elif _m._skeletal.is_clip_looping(_m._anim_key, _m._anim_variant):
		_m._anim_time = fposmod(seconds, length)
	else:
		_m._anim_time = clampf(seconds, 0.0, length)


func _resolve_body_clip_key(key: String) -> String:
	if _m._skeletal == null:
		return ""
	if not key.is_empty() and _m._skeletal.has_clip(key):
		return key
	# AnimMap registration binds absent semantic state keys to state-0 RESET.
	# Keep that same fallback for presentation; if this ADM has no RESET either,
	# callers retain their valid outgoing channel or fail without inventing one.
	return "anim_reset" if _m._skeletal.has_clip("anim_reset") else ""


func _clip_phase_seconds(key: String, phase_ticks: int) -> float:
	var fps: float = _m._skeletal.get_clip_fps(key)
	return (float(maxi(phase_ticks, 0)) / (2.0 * fps)
			if fps > 0.0 else 0.0)


func _clip_half_tick_seconds(key: String) -> float:
	var fps: float = _m._skeletal.get_clip_fps(key)
	return 1.0 / (2.0 * fps) if fps > 0.0 else 0.0


func _clear_body_blend() -> void:
	if not _m._body_blend_source_key.is_empty() or _m._body_blend_weight < 1.0:
		_m._body_pose_dirty = true
	_m._body_blend_source_key = ""
	_m._body_blend_source_time = 0.0
	_m._body_blend_weight = 1.0


func _reset_body_pose() -> void:
	_m._anim_key = ""
	_m._anim_variant = 0
	_m._anim_time = 0.0
	_m._anim_playing = false
	_m._anim_external_phase = false
	_m._body_phase_stamp_valid = false
	_clear_body_blend()
	if _m._skeleton != null:
		for bone in range(_m._skeleton.get_bone_count()):
			_m._skeleton.reset_bone_pose(bone)
	_m._body_pose_dirty = false


## The active body clip's playhead in seconds, loop-wrapped (one-shots clamp),
## so a scrub slider binds to it directly.
func get_animation_time() -> float:
	if _m._skeletal == null or _m._anim_key.is_empty():
		return 0.0
	var length: float = _m._skeletal.get_clip_length(_m._anim_key, _m._anim_variant)
	if length <= 0.0:
		return 0.0
	if _m._skeletal.is_clip_looping(_m._anim_key, _m._anim_variant):
		return fposmod(_m._anim_time, length)
	return clampf(_m._anim_time, 0.0, length)


# --- Part-animation channels (PLAYPARTANIM mission action) ---
# [orig: Jointops Entity_ApplyCommand @0x43ab60 case 0x22] PLAYPARTANIM(channel, play_type, time):
# ANIMNUM (channel) in {1,2} selects one of two part-anim channels (slot = channel-1); ANIMPLAYTYPE
# +1/0/-1 = forward/stop/reverse; ANIMTIME seconds = how long the part takes to cross its full range.
# The original stores a per-channel direction + truncated rate on the AI struct
# and a fixed-16-ms consumer applies wrapping signed ADD/SUB. It clamps only
# after strict upper/negative overshoot (so exact endpoints remain active for
# one more tick); it is velocity-from-current and does not reset the phase.
# Before drawing, retail publishes channel 1/2 to the fixed global CTRL names
# VEHICLE_SPECIAL1/VEHICLE_SPECIAL2. It never selects a model-local CTRL entry
# by ordinal.
# [orig: Jointops HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38]

const _PART_ANIM_CTRL_NAMES: Array[String] = [
	"VEHICLE_SPECIAL1",
	"VEHICLE_SPECIAL2",
]
const _PART_ANIM_CTRL_OWNERS: Array[String] = [
	"present:part_anim:1",
	"present:part_anim:2",
]
const _PART_ANIM_TICK_S := 0.016


func _part_anim_rate_for_seconds(time_s: float) -> int:
	# x87 FISTP returns the integer-indefinite INT_MIN for infinity, NaN,
	# or an out-of-range result. A zero finite result is then promoted to 1.
	# [orig: Entity_ApplyCommand @0x43B1A9..0x43B1F9]
	if time_s == 0.0:
		return -2147483648
	var rate_f := (_PART_ANIM_TICK_S / time_s) * 65536.0
	if is_nan(rate_f) or rate_f >= 2147483648.0 or rate_f < -2147483648.0:
		return -2147483648
	var rate := int(rate_f)
	return 1 if rate == 0 else _m._ctrl_dword(rate)


## Play a model part animation for the editor/legacy mission-controller path.
## The authoritative runtime integrates the same fields in AiSystem and presents
## them through set_part_phase(). channel: 1 or 2; play_type: -1, 0, or 1.
func play_part_anim(channel: int, play_type: int, time_s: float) -> void:
	var slot := channel - 1
	if slot < 0 or slot > 1:
		return  # the original validates channel in {1,2}; anything else is ignored
	if play_type < -1 or play_type > 1:
		return
	var register: String = _m._resolve_anim_channel_register(slot)
	if register.is_empty():
		return
	if play_type == 0:
		_m._part_anims.erase(register)  # Stop: freeze the part at its current value
		return
	if _m._part_anims.is_empty():
		_m._part_anim_tick_accum_s = 0.0
	_m._part_anims[register] = {
		"register": register,
		"dir": play_type,
		"rate": _part_anim_rate_for_seconds(time_s),
		"value": int(_m._ctrl_values.get(register, 0)),  # velocity from current
	}


## Editor-preview convenience: seed the channel at its rest start (0 forward / max reverse) then play,
## so a preview always shows the full motion from rest. Production presentation
## receives the authoritative phase through set_part_phase().
func restart_part_anim(channel: int, play_type: int, time_s: float) -> void:
	var slot := channel - 1
	if slot < 0 or slot > 1 or play_type < -1 or play_type > 1:
		return
	var register: String = _m._resolve_anim_channel_register(slot)
	# Stop (play_type == 0) must freeze the part where it is, so do NOT reseed the register: reseeding
	# to 0 would jump the part to its 0 pose before play_part_anim's stop erases the channel. Only the
	# forward/reverse previews seed a rest start (0 forward / max reverse).
	if not register.is_empty() and play_type != 0:
		_m._ctrl_values[register] = 0 if play_type >= 0 else 65536
		_m._ctrl_value_owners.erase(register)
		_m._finish_ctrl_change(register, false)
	play_part_anim(channel, play_type, time_s)


## Pose a part channel directly to the engine-computed signed dword. Ordinary
## sweeps live in 0..0x10000, but zero-time retail arithmetic can wrap outside
## that range and the publisher copies it without another clamp.
## The faithful runtime path: NovaSimulation/the AI brain integrates the PLAYPARTANIM phase in-engine
## (Entity_ApplyCommand @0x43ab60 + the per-frame consumer), and the owner just writes it to the PANM
## control register here. Distinct from play_part_anim (the editor/object-preview owner-side integrator).
func set_part_phase(channel: int, phase: int) -> void:
	var slot := channel - 1
	var register: String = _m._resolve_anim_channel_register(slot)
	var owner := _resolve_anim_channel_owner(slot)
	if register.is_empty() or owner.is_empty():
		return
	var next_phase: int = int(_m._ctrl_dword(phase))
	_m._part_anims.erase(register)  # the engine owns this channel's phase; no owner integrator on it
	# Keep the source tag so stale teardown cannot clear a newer writer. Retail
	# has one current slot value, not a rollback stack: clearing this current
	# publication removes it instead of resurrecting an older authored value.
	_m.set_ctrl_override(owner, register, next_phase)


## Release PLAYPARTANIM's ownership of one semantic register. This is distinct
## from publishing zero: retail suppresses VEHICLE_SPECIAL1 altogether for
## ItemDefAttrib FastRope (0x1000), while it still publishes SPECIAL2.
func clear_part_phase(channel: int) -> void:
	var slot := channel - 1
	var register: String = _m._resolve_anim_channel_register(slot)
	var owner := _resolve_anim_channel_owner(slot)
	if register.is_empty() or owner.is_empty():
		return
	_m._part_anims.erase(register)
	_m.clear_ctrl_override(owner, register)


func clear_part_anims() -> void:
	_m._part_anims.clear()
	_m._part_anim_tick_accum_s = 0.0


func get_active_part_anims() -> Dictionary:
	return _m._part_anims.duplicate(true)


# [orig: Jointops HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38:
#  comp[113]/comp[114] -> global CTRL VEHICLE_SPECIAL1/VEHICLE_SPECIAL2.]
func _resolve_anim_channel_register(slot: int) -> String:
	if slot < 0 or slot >= _PART_ANIM_CTRL_NAMES.size():
		return ""
	return _PART_ANIM_CTRL_NAMES[slot]


func _resolve_anim_channel_owner(slot: int) -> String:
	if slot < 0 or slot >= _PART_ANIM_CTRL_OWNERS.size():
		return ""
	return _PART_ANIM_CTRL_OWNERS[slot]


# Advance at retail's fixed 16 ms cadence with wrapping signed-dword ADD/SUB.
# Only strict overshoot clamps and stops; landing exactly on an endpoint keeps
# the direction live for one more tick.
# [orig: Entity_UpdateSuspensionBounce @0x456740..0x4567A9]
# Writes straight into _ctrl_values (NOT set_ctrl_value, which would eagerly re-evaluate per channel);
# the enclosing _apply_runtime_state applies the result once, in the same frame, to materials + PANM.
func _advance_part_anims(delta: float) -> bool:
	if _m._part_anims.is_empty() or delta <= 0.0:
		return false
	_m._part_anim_tick_accum_s += delta
	var tick_count := int(floor(
			(_m._part_anim_tick_accum_s + 0.000000001) / _PART_ANIM_TICK_S))
	if tick_count <= 0:
		return false
	_m._part_anim_tick_accum_s -= float(tick_count) * _PART_ANIM_TICK_S
	var changed := false
	for _tick in range(tick_count):
		if _m._part_anims.is_empty():
			break
		for register in _m._part_anims.keys():
			var anim: Dictionary = _m._part_anims[register]
			var previous := int(anim["value"])
			var direction := int(anim["dir"])
			var rate := int(anim["rate"])
			var next_value: int
			var finished := false
			if direction == 1:
				next_value = _m._ctrl_dword(previous + rate)
				if next_value > 65536:
					next_value = 65536
					finished = true
			else:
				next_value = _m._ctrl_dword(previous - rate)
				if next_value < 0:
					next_value = 0
					finished = true
			anim["value"] = next_value
			_m._ctrl_values[register] = next_value
			_m._ctrl_value_owners.erase(register)
			changed = changed or next_value != previous
			if finished:
				_m._part_anims.erase(register)
	_m._bounds_dirty = _m._bounds_dirty or changed
	return changed


func set_weapon_channel(key: String, phase_ticks: int) -> void:
	if key == _m._wpn_key and phase_ticks == _m._wpn_phase_ticks:
		return
	_m._wpn_key = key
	_m._wpn_phase_ticks = phase_ticks
	_m._body_pose_dirty = true


func set_aim_overlay(deltas: Array) -> void:
	var overlay_changed: bool = deltas != _m._aim_overlay_deltas
	var classes_changed := false
	if not deltas.is_empty() and _m._aim_overlay_classes.is_empty() and _m._skeletal != null \
			and _m._skeletal.has_method("get_overlay_classes"):
		var next_classes: PackedInt32Array = _m._skeletal.get_overlay_classes()
		classes_changed = next_classes != _m._aim_overlay_classes
		_m._aim_overlay_classes = next_classes
	if not overlay_changed and not classes_changed:
		return
	_m._aim_overlay_deltas = deltas
	_m._body_pose_dirty = true


func set_right_hand_collapsed(collapsed: bool) -> void:
	if collapsed == _m._collapse_right_hand:
		return
	_m._collapse_right_hand = collapsed
	_m._body_pose_dirty = true


## Pose the Skeleton3D from the active main-body clip. Advances the playhead while playing,
## evaluates the parent-local pose per bone (NovaSkeletalAnim, Godot space) and writes it as
## the bone pose. Public so deterministic owners can advance the render-time channel without
## reaching through Godot's private _process callback.
## write_pose=false advances the clip clock and latches _body_pose_dirty
## without writing bones — the hidden-model leg: the pose re-derives from
## _anim_time on the next visible frame, so an unseen clip never freezes or
## drifts.
func advance_body_animation(delta: float, write_pose := true) -> void:
	if _m._skeleton == null or _m._skeletal == null or _m._anim_key.is_empty():
		return
	if _m._is_playing and _m._anim_playing and not _m._anim_external_phase and delta != 0.0:
		_m._anim_time += delta
		_m._body_pose_dirty = true
	if _promote_remote_body_pending_if_due():
		_m._body_pose_dirty = true
	if not write_pose:
		return
	if not _m._body_pose_dirty:
		return
	var use_overlay: bool = (not _m._aim_overlay_deltas.is_empty()
			and not _m._aim_overlay_classes.is_empty())
	var use_primary_blend: bool = (
			not _m._body_blend_source_key.is_empty()
			and _m._body_blend_weight < 1.0)
	if _m._skeletal.has_method("pose_skeleton"):
		# The whole evaluate-and-write-bones loop in one native call: this runs per
		# animated model per render frame, and the per-bone Variant boxing + three
		# cross-boundary Skeleton3D calls from script dominated the present pass.
		var wpn_time := 0.0
		if use_overlay and not _m._wpn_key.is_empty():
			# Weapon-channel playhead: half-frame ticks -> seconds, the
			# play_body_clip_at convention (seconds = ticks / (2 * clip_fps)).
			var wfps: float = _m._skeletal.get_clip_fps(_m._wpn_key)
			if wfps > 0.0:
				wpn_time = float(maxi(_m._wpn_phase_ticks, 0)) / (2.0 * wfps)
		if use_primary_blend and _m._skeletal.has_method("pose_skeleton_blended"):
			_m._skeletal.pose_skeleton_blended(
					_m._skeleton,
					_m._body_blend_source_key, _m._body_blend_source_time,
					_m._anim_key, _m._anim_time, _m._body_blend_weight,
					_m._aim_overlay_classes if use_overlay else PackedInt32Array(),
					_m._aim_overlay_deltas if use_overlay else [],
					_m._wpn_key if use_overlay else "", wpn_time,
					_m._collapse_right_hand)
		else:
			_m._skeletal.pose_skeleton(
					_m._skeleton, _m._anim_key, _m._anim_time, _m._anim_variant,
					_m._aim_overlay_classes if use_overlay else PackedInt32Array(),
					_m._aim_overlay_deltas if use_overlay else [],
					_m._wpn_key if use_overlay else "", wpn_time,
					_m._collapse_right_hand)
		_m._body_pose_dirty = false
		return
	# Script fallback for duck-typed skeletal doubles (tests) without the native
	# batch entry.
	var pose: Array
	if use_primary_blend and use_overlay \
			and _m._skeletal.has_method("eval_pose_blended_overlay"):
		var wpn_time := 0.0
		if not _m._wpn_key.is_empty():
			var wfps: float = _m._skeletal.get_clip_fps(_m._wpn_key)
			if wfps > 0.0:
				wpn_time = float(maxi(_m._wpn_phase_ticks, 0)) / (2.0 * wfps)
		pose = _m._skeletal.eval_pose_blended_overlay(
				_m._body_blend_source_key, _m._body_blend_source_time,
				_m._anim_key, _m._anim_time, _m._body_blend_weight,
				_m._aim_overlay_classes, _m._aim_overlay_deltas,
				_m._wpn_key, wpn_time, _m._collapse_right_hand)
	elif use_primary_blend and _m._skeletal.has_method("eval_pose_blended"):
		pose = _m._skeletal.eval_pose_blended(
				_m._body_blend_source_key, _m._body_blend_source_time,
				_m._anim_key, _m._anim_time, _m._body_blend_weight)
	elif use_overlay and _m._skeletal.has_method("eval_pose_overlay"):
		# Weapon-channel playhead: half-frame ticks -> seconds, the play_body_clip_at
		# convention (seconds = ticks / (2 * clip_fps)).
		var wpn_time := 0.0
		if not _m._wpn_key.is_empty():
			var wfps: float = _m._skeletal.get_clip_fps(_m._wpn_key)
			if wfps > 0.0:
				wpn_time = float(maxi(_m._wpn_phase_ticks, 0)) / (2.0 * wfps)
		pose = _m._skeletal.eval_pose_overlay(
			_m._anim_key, _m._anim_time, _m._aim_overlay_classes, _m._aim_overlay_deltas,
			_m._wpn_key, wpn_time, _m._collapse_right_hand)
	else:
		# The weapon channel only renders through the overlay path — its export gate
		# (primary-state flag 0x40) implies the aim overlay is active [orig: @0x4b14a7].
		pose = _m._skeletal.eval_pose(_m._anim_key, _m._anim_time, _m._anim_variant)
	var count: int = mini(pose.size(), _m._skeleton.get_bone_count())
	for i in range(count):
		var t: Transform3D = pose[i]
		if _m._collapse_right_hand and i == 16:
			# eval_pose_overlay preserves BN17's sampled parent-local joint origin
			# while clearing its basis. Apply that origin directly and collapse scale
			# there. Sending the joint to world zero makes mixed-weight triangles span
			# from the actor to the origin instead of clipping the baked weapon.
			# This branch also covers the no-overlay eval_pose fallback above.
			_m._skeleton.set_bone_pose_position(i, t.origin)
			_m._skeleton.set_bone_pose_rotation(i, Quaternion.IDENTITY)
			_m._skeleton.set_bone_pose_scale(i, Vector3.ZERO)
		else:
			_m._skeleton.set_bone_pose_position(i, t.origin)
			_m._skeleton.set_bone_pose_rotation(i, t.basis.get_rotation_quaternion())
			_m._skeleton.set_bone_pose_scale(i, t.basis.get_scale())
	_m._body_pose_dirty = false
