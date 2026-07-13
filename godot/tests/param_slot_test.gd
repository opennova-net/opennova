extends GutTest

# MissionParamSlot widget: the only place the seconds<->raw (16.16) conversion lives, plus the
# raw-int contract for picker kinds. These pin the round-trip the scripting inspector relies on
# for the AI-change action params (PLAYPARTANIM's ANIMTIME / ANIMNUM).

const Schema := preload("res://modtools/mission/mission_param_schema.gd")


func _make_slot() -> MissionParamSlot:
	var s := MissionParamSlot.new()
	s.setup("Test", "Param", -2147483648.0, 2147483647.0)
	add_child_autofree(s)
	return s


func _spec(kind: int, label: String) -> MissionParamSlotSpec:
	var spec := MissionParamSlotSpec.new()
	spec.kind = kind
	spec.label = label
	return spec


func test_fixed_seconds_shows_seconds_and_stores_raw() -> void:
	var s := _make_slot()
	s.configure(_spec(Schema.Kind.FIXED_SECONDS, "Time (s)"), [])
	assert_false(s.is_picker(), "fixed-seconds uses the spin, not a picker")
	s.set_value(98304)  # 1.5 * 65536
	assert_almost_eq(s.get_spin().value, 1.5, 0.0001, "spin shows raw / 65536 seconds")
	assert_eq(s.read_value(), 98304, "read_value returns the raw 16.16 value")


func test_fixed_seconds_round_trips_original_granularity() -> void:
	var s := _make_slot()
	s.configure(_spec(Schema.Kind.FIXED_SECONDS, "Time (s)"), [])
	# All multiples of 256 (the original ANIMTIME step), so the seconds<->raw conversion is exact.
	for raw in [0, 256, 65536, 196608, 327424]:
		s.set_value(raw)
		assert_eq(s.read_value(), raw, "round-trips raw %d" % raw)


func test_non_fixed_seconds_stays_raw_int() -> void:
	var s := _make_slot()
	s.configure(_spec(Schema.Kind.RAW, "Value"), [])
	s.set_value(12345)
	assert_eq(s.read_value(), 12345, "raw kind stores the int verbatim")
