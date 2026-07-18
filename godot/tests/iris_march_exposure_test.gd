extends GutTest

# The marched iris-exposure combiner (D-RLIT-2's curve half):
# NovaWeatherCore.set_exposure_from_iris_samples — per-sample classification
# (indoor vs ceiling/floor, indoor-no-data 255, outdoor sun level x/8), INT /3
# average, 62-tick modulator chase [orig: compute_ambient_light_along_direction
# @ 0x5c7a00; terrain_sector_compute_lighting @ 0x5c7550]. The sampling half
# (NovaSimulation.compute_iris_samples) needs a loaded world and rides the
# asset-gated visual probes.
#
# Block/iris fixture = the FULL_01.ENV 0800 keyframes: sun (159,159,141),
# sky (84,85,86), ground (41,43,41), iris 15% / 1.0. Hand-derived gains
# (the libs iris curve, luminance 0.25/0.5/0.25):
#   outdoor 8/8 -> m = horiz 0.88557 -> gain 59  (the live-dump value)
#   indoor ceiling (70,70,70) / floor (10,10,10) -> m = 0.27451 -> gain 71
#   indoor-no-data -> 255 (the all-zero-inputs clamp)
#   mixed [8, -1, -2] -> (59 + 71 + 255) / 3 = 128

const SUN := Color(159.0 / 255.0, 159.0 / 255.0, 141.0 / 255.0)
const SKY := Color(84.0 / 255.0, 85.0 / 255.0, 86.0 / 255.0)
const GROUND := Color(41.0 / 255.0, 43.0 / 255.0, 41.0 / 255.0)
const FOG := Color(98.0 / 255.0, 92.0 / 255.0, 118.0 / 255.0)
const CEILING := Color(70.0 / 255.0, 70.0 / 255.0, 70.0 / 255.0)
const FLOOR_C := Color(10.0 / 255.0, 10.0 / 255.0, 10.0 / 255.0)
const LIGHT_DIR := Vector3(0.63, 0.473, 0.615)
const IRIS_PERCENT := 15.0
const IRIS_CENTER := 1.0
# The modulator chases its target over 62 ticks [orig: ColorBlock_SetStepDeltas
# @ 0x57d940]; run a margin past that so the render color settles exactly.
const SETTLE_TICKS := 80


func _settled_gain(samples: PackedInt32Array) -> float:
	var core := NovaWeatherCore.new()
	core.snap_colors(GROUND, SUN, FOG, SKY)
	core.set_exposure_from_iris_samples(
		samples, LIGHT_DIR, CEILING, FLOOR_C, IRIS_PERCENT, IRIS_CENTER)
	for _i in SETTLE_TICKS:
		core.tick(GROUND, SUN, FOG, SKY, Color.BLACK, 0.0)
	# get_color_src_gain = modulator render bytes / 64 (identity = 1.0)
	# [orig: Render_UnpackModulatorToLightScale @ 0x58db30].
	return core.get_color_src_gain().x


func test_outdoor_full_sun_matches_the_outdoor_sample() -> void:
	# Level-8 outdoor samples equal the legacy outdoor fallback: gain 59 at the
	# FULL_01 0800 fixture (the value the live play dump serves).
	assert_almost_eq(_settled_gain(PackedInt32Array([8, 8, 8])), 59.0 / 64.0, 0.0001)


func test_empty_samples_fall_back_to_the_outdoor_sample() -> void:
	assert_almost_eq(_settled_gain(PackedInt32Array()), 59.0 / 64.0, 0.0001)


func test_indoor_samples_dilate_via_ceiling_floor() -> void:
	# All-indoor: dir zeroed, sky/ground <- ceiling/floor -> m collapses to the
	# ceiling luminance and the iris opens: gain 71
	# [orig: the indoor swap @ 0x5c7660..0x5c76fe].
	assert_almost_eq(_settled_gain(PackedInt32Array([-1, -1, -1])), 71.0 / 64.0, 0.0001)


func test_indoor_without_interior_data_serves_255() -> void:
	# [orig: the pool_entry[12] == 0 skip @ 0x5c7652 — all-zero inputs, clamp 255].
	# The settled render byte is 254: the witnessed 12.20 step chase truncates
	# one LSB short of a 255 target and holds [orig: ColorBlock_SetStepDeltas
	# @ 0x57d940 — |target<<20 + frames/2 − cur|/frames].
	assert_almost_eq(_settled_gain(PackedInt32Array([-2, -2, -2])), 254.0 / 64.0, 0.0001)


func test_mixed_samples_average_as_ints() -> void:
	# (59 + 71 + 255) / 3 = 128 truncating [orig: (s0+s1+s2)/3 @ 0x5c7b45].
	assert_almost_eq(_settled_gain(PackedInt32Array([8, -1, -2])), 128.0 / 64.0, 0.0001)


func test_sun_occlusion_level_raises_the_gain() -> void:
	# Blocked sun rays scale the directional block by level/8, shrinking m and
	# opening the iris — level 5 must serve a strictly higher gain than level 8
	# [orig: light_scale = level/8/255 @ 0x5c77e9].
	var full: float = _settled_gain(PackedInt32Array([8, 8, 8]))
	var shaded: float = _settled_gain(PackedInt32Array([5, 5, 5]))
	assert_gt(shaded, full)
