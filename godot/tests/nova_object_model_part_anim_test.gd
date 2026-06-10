extends GutTest

# NovaObjectModel.play_part_anim drives a per-channel phase sweep into the model's PANM control
# registers, mirroring the runtime PLAYPARTANIM action. The channel->register resolution is the only
# object_data touch, so it is overridden here to test the sweep logic asset-free (no loaded .3di).
# [orig: Jointops Entity_ApplyCommand @0x43ab60 case 0x22 -- velocity-from-current, clamp at [0,65535].]


class PartAnimModel:
	extends NovaObjectModel
	var regs: Array = ["reg0", "reg1"]  # the model's two part-anim channels
	func _resolve_anim_channel_register(slot: int) -> String:
		return String(regs[slot]) if slot >= 0 and slot < regs.size() else ""


func _model() -> PartAnimModel:
	var m := PartAnimModel.new()
	autofree(m)
	return m


func test_play_forward_sweeps_register_to_max() -> void:
	var m := _model()
	m.play_part_anim(1, 1, 1.0)  # channel 1 -> reg0, play forward, 1 second to cross the full range
	m._advance_part_anims(0.5)
	assert_almost_eq(int(m.get_ctrl_values().get("reg0", -1)), 32768, 2, "halfway ~ 32768")
	m._advance_part_anims(0.6)   # past the end
	assert_eq(int(m.get_ctrl_values().get("reg0", -1)), 65535, "clamps at the maximum")
	assert_false(m.get_active_part_anims().has("reg0"), "a finished sweep is dropped")


func test_play_reverse_sweeps_to_zero() -> void:
	var m := _model()
	m.set_ctrl_value("reg0", 65535)   # start at the top
	m.play_part_anim(1, -1, 1.0)      # reverse over 1 second
	m._advance_part_anims(0.5)
	assert_almost_eq(int(m.get_ctrl_values().get("reg0", -1)), 32767, 2, "halfway down")
	m._advance_part_anims(0.6)
	assert_eq(int(m.get_ctrl_values().get("reg0", -1)), 0, "clamps at zero")


func test_channel_2_uses_the_second_register() -> void:
	var m := _model()
	m.play_part_anim(2, 1, 1.0)       # channel 2 -> reg1
	m._advance_part_anims(0.5)
	assert_true(m.get_ctrl_values().has("reg1"), "channel 2 drives the second register")
	assert_false(m.get_ctrl_values().has("reg0"), "and leaves the first untouched")


func test_stop_freezes_and_clears_the_sweep() -> void:
	var m := _model()
	m.play_part_anim(1, 1, 1.0)
	m._advance_part_anims(0.25)
	var frozen := int(m.get_ctrl_values().get("reg0", -1))
	m.play_part_anim(1, 0, 1.0)       # Stop (play_type 0)
	assert_false(m.get_active_part_anims().has("reg0"), "stop drops the running sweep")
	m._advance_part_anims(1.0)        # no further movement
	assert_eq(int(m.get_ctrl_values().get("reg0", -1)), frozen, "value is frozen at the stop point")


func test_invalid_channel_is_a_noop() -> void:
	var m := _model()
	m.play_part_anim(3, 1, 1.0)       # only channels 1 and 2 are valid
	m.play_part_anim(0, 1, 1.0)
	assert_true(m.get_active_part_anims().is_empty(), "channels outside {1,2} are ignored")


func test_unknown_register_is_a_noop() -> void:
	var m := _model()
	m.regs = []                       # model exposes no control registers
	m.play_part_anim(1, 1, 1.0)
	assert_true(m.get_active_part_anims().is_empty(), "no register -> no sweep")


func test_reissue_replaces_the_sweep_from_current_value() -> void:
	var m := _model()
	m.play_part_anim(1, 1, 4.0)       # slow
	m._advance_part_anims(0.5)
	var after_slow := int(m.get_ctrl_values().get("reg0", -1))
	m.play_part_anim(1, 1, 1.0)       # faster, resuming from the current value (no reset)
	var anims := m.get_active_part_anims()
	assert_eq(anims.size(), 1, "still one sweep on reg0 (replaced, not duplicated)")
	assert_almost_eq(float((anims["reg0"] as Dictionary)["value"]), float(after_slow), 1.0, "resumes from current")


func test_zero_time_snaps_to_the_endpoint() -> void:
	var m := _model()
	m.play_part_anim(1, 1, 0.0)       # time 0 == instant
	m._advance_part_anims(0.001)
	assert_eq(int(m.get_ctrl_values().get("reg0", -1)), 65535, "time 0 snaps to the endpoint")


func test_restart_seeds_start_then_plays() -> void:
	# restart_part_anim is the editor-preview variant: it seeds the channel at its rest start so a
	# preview shows the full motion regardless of where the part currently sits.
	var m := _model()
	m.set_ctrl_value("reg0", 40000)   # part sitting partway through
	m.restart_part_anim(1, 1, 1.0)    # forward restart -> seed 0
	assert_eq(int(m.get_ctrl_values().get("reg0", -1)), 0, "forward restart seeds the start at 0")
	m._advance_part_anims(0.5)
	assert_almost_eq(int(m.get_ctrl_values().get("reg0", -1)), 32768, 2, "then sweeps up from 0")


func test_restart_reverse_seeds_max() -> void:
	var m := _model()
	m.restart_part_anim(1, -1, 1.0)   # reverse restart -> seed the max end
	assert_eq(int(m.get_ctrl_values().get("reg0", -1)), 65535, "reverse restart seeds the start at max")
