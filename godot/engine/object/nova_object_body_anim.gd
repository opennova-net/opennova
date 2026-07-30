extends RefCounted

# NovaObjectModel's main-body skeletal-animation section (quality slice
# W4-6c, the merged W4-6a/6b verbatim-motion shape): clip play/variant/
# seeded selection, the retail remote body-state arbitration, the muzzle
# userpoint, playhead scrub, the PLAYPARTANIM part-anim channels, the
# aim-overlay/weapon-channel setters, and advance_body_animation pose
# evaluation. ALL state, constants and signals stay on the composing
# NovaObjectModel, reached through `_m`; every moved method keeps a host
# delegate, so the external surface (and test-subclass overrides, e.g.
# _resolve_anim_channel_register) are unchanged. The host is a Node3D --
# manually managed, not refcounted -- so this plain back-reference cannot
# cycle: the host owns the helper; the helper points back at the Node.

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


## Play a main-body clip by ADM key (e.g. "anim_walk"). No-op if no skeletal set / unknown.
func play_body_clip(key: String) -> void:
	play_body_clip_variant(key, 0)


## play_body_clip selecting a same-key VARIANT (multi-clip .adm rows): the FSM owner's
## ring serves the index and playback follows that latch until the next play — a
## variant change re-poses even on the same key. [orig: AnimMap_PlayAnimBySlot
## @0x40bda0 latches the served ring entry at animState+68]
func play_body_clip_variant(key: String, variant: int) -> void:
	if _m._skeletal == null or not _m._skeletal.has_clip(key):
		return
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
	# Repeat-call fast path: the stamp proves this exact (key, tick) pair is what
	# posed the skeleton last, nothing else touched the playhead since, and no
	# other input dirtied the pose — the full body below would be a no-op.
	if (_m._body_phase_stamp_valid and _m._anim_external_phase
			and not _m._body_pose_dirty
			and phase_ticks == _m._body_phase_ticks_applied
			and key == _m._anim_key):
		return
	if _m._skeletal == null or not _m._skeletal.has_clip(key):
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


## Seed a main-body clip from retail half-frame ticks, pose it immediately, and
## leave it free-running. Distinct from play_body_clip_at(), whose callers own
## every later playhead sample and therefore intentionally pin external phase.
func play_body_clip_seeded(key: String, phase_ticks: int) -> void:
	if _select_body_clip_seeded(key, phase_ticks):
		advance_body_animation(0.0)


func _select_body_clip_seeded(key: String, phase_ticks: int) -> bool:
	if _m._skeletal == null or not _m._skeletal.has_clip(key):
		return false
	_m._anim_key = key
	_m._anim_variant = 0
	var fps: float = _m._skeletal.get_clip_fps(key)
	var seconds := 0.0
	if fps > 0.0:
		seconds = float(maxi(phase_ticks, 0)) / (2.0 * fps)
	_set_body_playhead(seconds)
	_m._anim_playing = true
	_m._anim_external_phase = false
	_m._body_pose_dirty = true
	return true


## Apply one raw compact-organic body-state request with retail's remote
## transition arbitration. A phase belongs only to an immediately accepted
## player transition; queued states promote at tick zero when the current clip
## reaches its completion boundary.
func apply_remote_body_state(state_id: int, key: String, flags: int,
		phase_ticks: int = -1) -> void:
	if state_id < 0 or key.is_empty() or _m._skeletal == null or not _m._skeletal.has_clip(key):
		return
	if _m._remote_state < 0:
		_accept_remote_body_state(state_id, key, flags, phase_ticks)
		return
	if state_id == _m._remote_state:
		_clear_remote_body_pending()
		return
	if ((_m._remote_flags & 0x4) != 0
			or ((_m._remote_flags & 0x20) != 0 and (flags & 0x1) == 0)):
		_queue_remote_body_state(state_id, key, flags)
		return
	_accept_remote_body_state(state_id, key, flags, phase_ticks)


func reset_remote_body_state() -> void:
	_m._remote_state = -1
	_m._remote_flags = 0
	_clear_remote_body_pending()


func _accept_remote_body_state(state_id: int, key: String, flags: int,
		phase_ticks: int) -> void:
	_clear_remote_body_pending()
	_m._remote_state = state_id
	_m._remote_flags = flags
	if _select_body_clip_seeded(key, phase_ticks if phase_ticks >= 0 else 0):
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
	return _select_body_clip_seeded(key, 0)


func stop_body_clip() -> void:
	_m._anim_playing = false
	_m._anim_external_phase = false
	_m._body_phase_stamp_valid = false
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
# The original stores a per-channel direction + per-tick rate on the AI struct and a per-frame consumer
# sweeps a 16.16 phase (0..65536, full range in ANIMTIME at 62.5Hz), clamping at the ends; it is
# velocity-from-current (it does NOT reset the phase). We drive part channel `slot` through the model's
# PANM control register at index `slot`, value 0..65535 == that phase, fed to evaluate_panm() each frame.
# See notes/mission/anim-ai-grill-2026-06-07.md.


## Play a model part animation, mirroring the runtime PLAYPARTANIM action so editor preview and host
## playback share one path. channel: 1 or 2. play_type: 1 play / 0 stop / -1 reverse. time_s: seconds
## for the part to traverse its full range (ANIMTIME).
func play_part_anim(channel: int, play_type: int, time_s: float) -> void:
	var slot := channel - 1
	if slot < 0 or slot > 1:
		return  # the original validates channel in {1,2}; anything else is ignored
	var register: String = _m._resolve_anim_channel_register(slot)
	if register.is_empty():
		return
	if play_type == 0:
		_m._part_anims.erase(register)  # Stop: freeze the part at its current value
		return
	var dir := 1 if play_type > 0 else -1
	var speed := 65535.0 / time_s if time_s > 0.0 else 1.0e9  # full range crossed in time_s seconds
	_m._part_anims[register] = {
		"register": register,
		"dir": dir,
		"speed": speed,
		"value": float(int(_m._ctrl_values.get(register, 0))),  # velocity from current (no reset)
	}


## Editor-preview convenience: seed the channel at its rest start (0 forward / max reverse) then play,
## so a preview always shows the full motion from rest. The runtime uses play_part_anim directly
## (velocity-from-current, faithful to the action); only the editor preview restarts.
func restart_part_anim(channel: int, play_type: int, time_s: float) -> void:
	var slot := channel - 1
	if slot < 0 or slot > 1:
		return
	var register: String = _m._resolve_anim_channel_register(slot)
	# Stop (play_type == 0) must freeze the part where it is, so do NOT reseed the register: reseeding
	# to 0 would jump the part to its 0 pose before play_part_anim's stop erases the channel. Only the
	# forward/reverse previews seed a rest start (0 forward / max reverse).
	if not register.is_empty() and play_type != 0:
		_m._ctrl_values[register] = 0 if play_type >= 0 else 65535
	play_part_anim(channel, play_type, time_s)


## Pose a part channel directly to an engine-computed phase (0..65535 == 0..1 over the part's range).
## The faithful runtime path: NovaSimulation/the AI brain integrates the PLAYPARTANIM phase in-engine
## (Entity_ApplyCommand @0x43ab60 + the per-frame consumer), and the host just writes it to the PANM
## control register here. Distinct from play_part_anim (the editor/object-preview host-side integrator).
func set_part_phase(channel: int, phase: int) -> void:
	var register: String = _m._resolve_anim_channel_register(channel - 1)
	if register.is_empty():
		return
	var next_phase := clampi(phase, 0, 65535)
	if int(_m._ctrl_values.get(register, -1)) == next_phase and not _m._part_anims.has(register):
		return
	_m._part_anims.erase(register)  # the engine owns this channel's phase; no host integrator on it
	_m._ctrl_values[register] = next_phase
	_m._bounds_dirty = true


func clear_part_anims() -> void:
	_m._part_anims.clear()


func get_active_part_anims() -> Dictionary:
	return _m._part_anims.duplicate(true)


# [orig: ANIMNUM channel (1/2) -> part-anim slot 0/1 -> the model's PANM control register at index `slot`.]
func _resolve_anim_channel_register(slot: int) -> String:
	if _m.object_data == null:
		return ""
	var regs: Array = _m.object_data.get_control_registers()
	if slot < 0 or slot >= regs.size():
		return ""
	return String((regs[slot] as Dictionary).get("name", ""))


# Advance each active channel's phase toward its endpoint at the authored speed, clamping at [0,65535].
# Writes straight into _ctrl_values (NOT set_ctrl_value, which would eagerly re-evaluate per channel);
# the enclosing _apply_runtime_state applies the result once, in the same frame, to materials + PANM.
func _advance_part_anims(delta: float) -> bool:
	if _m._part_anims.is_empty() or delta <= 0.0:
		return false
	var finished: Array = []
	var changed := false
	for register in _m._part_anims.keys():
		var anim: Dictionary = _m._part_anims[register]
		var old_value := int(_m._ctrl_values.get(register, 0))
		var value := clampf(float(anim["value"]) + float(anim["speed"]) * float(anim["dir"]) * delta, 0.0, 65535.0)
		anim["value"] = value
		var next_value := int(round(value))
		_m._ctrl_values[register] = next_value
		changed = changed or old_value != next_value
		if (int(anim["dir"]) > 0 and value >= 65535.0) or (int(anim["dir"]) < 0 and value <= 0.0):
			finished.append(register)  # reached the clamp endpoint; the part holds there
	for register in finished:
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
## the bone pose. Public so deterministic hosts can advance the render-time channel without
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
		_m._skeletal.pose_skeleton(
				_m._skeleton, _m._anim_key, _m._anim_time, _m._anim_variant,
				_m._aim_overlay_classes if use_overlay else PackedInt32Array(),
				_m._aim_overlay_deltas if use_overlay else [],
				_m._wpn_key if use_overlay else "", wpn_time, _m._collapse_right_hand)
		_m._body_pose_dirty = false
		return
	# Script fallback for duck-typed skeletal doubles (tests) without the native
	# batch entry.
	var pose: Array
	if use_overlay and _m._skeletal.has_method("eval_pose_overlay"):
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
