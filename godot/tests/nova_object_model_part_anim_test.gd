extends GutTest

# NovaObjectModel.play_part_anim drives a per-channel phase sweep onto the retail
# VEHICLE_SPECIAL1/2 CTRL bus. The resolver is overridden for the missing-register
# case; time advancement uses the same public deterministic-frame seam as runtime
# owners, backed by the smallest committed object fixture.
# [orig: Jointops Entity_ApplyCommand @0x43ab60 case 0x22; integrator @0x456710,
#  velocity-from-current, wrapping dword arithmetic, and strict overshoot clamps.]

const RUNTIME_3DI := "res://../fixtures/threedi/3di3/Shed.3di"


class PartAnimModel:
	extends NovaObjectModel
	var regs: Array = ["VEHICLE_SPECIAL1", "VEHICLE_SPECIAL2"]
	func _resolve_anim_channel_register(slot: int) -> String:
		return String(regs[slot]) if slot >= 0 and slot < regs.size() else ""
	func mark_body_pose_clean() -> void:
		_body_pose_dirty = false
	func is_body_pose_dirty() -> bool:
		return _body_pose_dirty


func _model() -> PartAnimModel:
	var m := PartAnimModel.new()
	autofree(m)
	return m


func _runtime_model() -> PartAnimModel:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(RUNTIME_3DI)), OK,
			"the committed runtime fixture opens")
	var m := PartAnimModel.new()
	add_child_autofree(m)
	m.set_process(false)
	m.set_object_data(data)
	m.visible = false
	return m


func test_play_forward_sweeps_register_to_max() -> void:
	var m := _runtime_model()
	m.play_part_anim(1, 1, 1.0)
	m.advance_runtime_frame(0.5)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 32488,
			"31 retail 16 ms ticks use the truncated 1048 phase rate")
	m.advance_runtime_frame(0.6)   # past the end
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 65536, "keeps retail's exact 1.0 endpoint")
	assert_false(m.get_active_part_anims().has("VEHICLE_SPECIAL1"), "a finished sweep is dropped")


func test_play_reverse_sweeps_to_zero() -> void:
	var m := _runtime_model()
	m.set_ctrl_value("VEHICLE_SPECIAL1", 65536)   # start at the top
	m.play_part_anim(1, -1, 1.0)      # reverse over 1 second
	m.advance_runtime_frame(0.5)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 33048,
			"reverse uses the same 31 fixed ticks")
	m.advance_runtime_frame(0.6)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 0, "clamps at zero")


func test_channel_2_uses_the_second_register() -> void:
	var m := _runtime_model()
	m.play_part_anim(2, 1, 1.0)
	m.advance_runtime_frame(0.5)
	assert_true(m.get_ctrl_values().has("VEHICLE_SPECIAL2"), "channel 2 drives the second register")
	assert_false(m.get_ctrl_values().has("VEHICLE_SPECIAL1"), "and leaves the first untouched")


func test_production_channels_use_retail_semantic_registers() -> void:
	var m := NovaObjectModel.new()
	autofree(m)
	m.set_part_phase(1, 0x1111)
	m.set_part_phase(2, 0x2222)
	m.set_part_phase(3, 0x3333)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 0x1111)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL2", -1)), 0x2222)
	assert_eq(m.get_ctrl_values().size(), 2, "channels outside retail's pair are ignored")


func test_control_values_preserve_signed_dwords() -> void:
	var m := _model()
	m.set_ctrl_value("HELO_GUNYAW", -0x2000)
	m.set_ctrl_value("HEAT_GLOW", 0x10000)
	m.set_ctrl_value("LOD_FRAC", -2147483648)
	m.set_ctrl_value("TEX_CAMO3", 2147483647)
	assert_eq(int(m.get_ctrl_values()["HELO_GUNYAW"]), -0x2000)
	assert_eq(int(m.get_ctrl_values()["HEAT_GLOW"]), 0x10000)
	assert_eq(int(m.get_ctrl_values()["LOD_FRAC"]), -2147483648)
	assert_eq(int(m.get_ctrl_values()["TEX_CAMO3"]), 2147483647)


func test_control_value_names_canonicalize_and_clear_as_one_global_slot() -> void:
	var m := _model()
	m.set_ctrl_value("heat_glow", 123)
	m.set_ctrl_value("HeAt_GlOw", 456)
	assert_eq(m.get_ctrl_values(), {"HEAT_GLOW": 456})
	m.clear_ctrl_value("hEaT_gLoW")
	assert_true(m.get_ctrl_values().is_empty())
	m.set_ctrl_value("not_a_retail_register", 99)
	assert_true(m.get_ctrl_values().is_empty(),
			"runtime ingress ignores names outside the canonical 96-slot bus")


func test_owned_store_is_single_value_and_stale_clear_cannot_rollback() -> void:
	var m := _model()
	m.set_ctrl_value("HEAT_GLOW", 123)
	m.set_ctrl_override("present:world_heat", "HEAT_GLOW", 0)
	assert_eq(int(m.get_ctrl_values()["HEAT_GLOW"]), 0,
			"the dedicated writer overwrites the one retail slot")
	m.set_ctrl_override("probe:newer_writer", "HEAT_GLOW", 456)
	assert_eq(int(m.get_ctrl_values()["HEAT_GLOW"]), 456,
			"the latest store replaces the prior value")
	m.clear_ctrl_override("present:world_heat", "HEAT_GLOW")
	assert_eq(int(m.get_ctrl_values()["HEAT_GLOW"]), 456,
			"an older lifecycle cannot clear or resurrect through a later store")
	m.clear_ctrl_override("probe:newer_writer", "HEAT_GLOW")
	assert_true(m.get_ctrl_values().is_empty(),
			"releasing the current writer removes the retained-model snapshot")


func test_part_phase_overwrites_instead_of_stacking_a_base_value() -> void:
	var m := _model()
	m.set_ctrl_value("VEHICLE_SPECIAL1", 777)
	m.set_part_phase(1, 0x2345)
	assert_eq(int(m.get_ctrl_values()["VEHICLE_SPECIAL1"]), 0x2345)
	m.clear_part_phase(1)
	assert_false(m.get_ctrl_values().has("VEHICLE_SPECIAL1"),
			"an overwritten value is not resurrected when PLAYPARTANIM releases")


func test_stop_freezes_and_clears_the_sweep() -> void:
	var m := _runtime_model()
	m.play_part_anim(1, 1, 1.0)
	m.advance_runtime_frame(0.25)
	var frozen := int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1))
	m.play_part_anim(1, 0, 1.0)       # Stop (play_type 0)
	assert_false(m.get_active_part_anims().has("VEHICLE_SPECIAL1"), "stop drops the running sweep")
	m.advance_runtime_frame(1.0)        # no further movement
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), frozen, "value is frozen at the stop point")


func test_invalid_channel_is_a_noop() -> void:
	var m := _model()
	m.play_part_anim(3, 1, 1.0)       # only channels 1 and 2 are valid
	m.play_part_anim(0, 1, 1.0)
	assert_true(m.get_active_part_anims().is_empty(), "channels outside {1,2} are ignored")


func test_invalid_play_type_does_not_seed_or_start_a_channel() -> void:
	var m := _model()
	m.set_ctrl_value("VEHICLE_SPECIAL1", 12345)
	m.restart_part_anim(1, 2, 1.0)
	assert_eq(int(m.get_ctrl_values()["VEHICLE_SPECIAL1"]), 12345)
	assert_true(m.get_active_part_anims().is_empty(),
			"retail ignores play types outside {-1,0,1}")


func test_unknown_register_is_a_noop() -> void:
	var m := _model()
	m.regs = []                       # model exposes no control registers
	m.play_part_anim(1, 1, 1.0)
	assert_true(m.get_active_part_anims().is_empty(), "no register -> no sweep")


func test_reissue_replaces_the_sweep_from_current_value() -> void:
	var m := _runtime_model()
	m.play_part_anim(1, 1, 4.0)       # slow
	m.advance_runtime_frame(0.5)
	var after_slow := int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1))
	m.play_part_anim(1, 1, 1.0)       # faster, resuming from the current value (no reset)
	var anims := m.get_active_part_anims()
	assert_eq(anims.size(), 1, "still one sweep on channel 1 (replaced, not duplicated)")
	assert_eq(int((anims["VEHICLE_SPECIAL1"] as Dictionary)["value"]), after_slow,
			"resumes exactly from the current signed-dword phase")


func test_zero_time_uses_retail_wrapping_add_sub() -> void:
	var m := _runtime_model()
	m.set_ctrl_value("VEHICLE_SPECIAL1", 0)
	m.play_part_anim(1, 1, 0.0)
	m.advance_runtime_frame(0.016)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", 0)), -2147483648,
			"zero-time forward adds INT_MIN without a lower clamp")
	assert_true(m.get_active_part_anims().has("VEHICLE_SPECIAL1"),
			"the negative wrapped value does not satisfy the strict upper clamp")
	m.advance_runtime_frame(0.016)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 0,
			"a second wrapping ADD returns to zero")
	assert_true(m.get_active_part_anims().has("VEHICLE_SPECIAL1"))
	m.clear_part_anims()
	m.set_ctrl_value("VEHICLE_SPECIAL1", 0)
	m.play_part_anim(1, -1, 0.0)
	m.advance_runtime_frame(0.016)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 0,
			"zero-time reverse subtracts INT_MIN, sees a negative result, and clamps low")
	assert_false(m.get_active_part_anims().has("VEHICLE_SPECIAL1"),
			"the negative reverse result clears its direction")


func test_restart_seeds_start_then_plays() -> void:
	# restart_part_anim is the editor-preview variant: it seeds the channel at its rest start so a
	# preview shows the full motion regardless of where the part currently sits.
	var m := _runtime_model()
	m.set_ctrl_value("VEHICLE_SPECIAL1", 40000)   # part sitting partway through
	m.restart_part_anim(1, 1, 1.0)    # forward restart -> seed 0
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 0, "forward restart seeds the start at 0")
	m.advance_runtime_frame(0.5)
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 32488,
			"then advances on retail's fixed 16 ms cadence")


func test_restart_reverse_seeds_max() -> void:
	var m := _model()
	m.restart_part_anim(1, -1, 1.0)   # reverse restart -> seed the max end
	assert_eq(int(m.get_ctrl_values().get("VEHICLE_SPECIAL1", -1)), 65536, "reverse restart seeds the exact 1.0 endpoint")


func test_unchanged_aim_overlay_does_not_redirty_body_pose() -> void:
	var m := _model()
	m.mark_body_pose_clean()
	m.set_aim_overlay([])
	assert_false(m.is_body_pose_dirty(),
			"repeated disabled overlays preserve the body-pose fast path")

	var deltas: Array = [Basis.from_euler(Vector3(0.1, -0.2, 0.3))]
	m.set_aim_overlay(deltas)
	assert_true(m.is_body_pose_dirty(), "a new overlay invalidates the pose")
	m.mark_body_pose_clean()
	m.set_aim_overlay(deltas.duplicate())
	assert_false(m.is_body_pose_dirty(),
			"an identical active overlay cannot re-evaluate and upload the skeleton")
