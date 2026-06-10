extends GutTest

# NovaColorSmoother wraps the engine's 12.20 fixed-point eighth-step channel
# smoother [orig: interpolate_weather_color @ 0x57d9e0]. See docs/env/env-tod-re.md.


func test_eighth_step_decays_toward_target() -> void:
	var smoother := NovaColorSmoother.new()
	smoother.snap(Color(1.0, 0.5, 0.25))
	var stepped := smoother.step(Color(0.0, 0.0, 0.0), 255.0)
	# One unclamped eighth-step keeps ~7/8 with +0x80000 rounding: 255->223.
	assert_eq(int(stepped.r * 255.0 + 0.5), 223, "R 255 decays to 223 after one step.")
	assert_eq(int(stepped.g * 255.0 + 0.5), 112, "G 128 decays to 112.")
	assert_eq(int(stepped.b * 255.0 + 0.5), 56, "B 64 decays to 56.")


func test_step_converges_to_target() -> void:
	var smoother := NovaColorSmoother.new()
	smoother.snap(Color(0.0, 0.0, 0.0))
	var target := Color(1.0, 1.0, 1.0)
	for _i in 200:
		smoother.step(target, 255.0)
	var final := smoother.get_current()
	assert_almost_eq(final.r, 1.0, 0.01, "Repeated stepping converges to the target.")


func test_max_step_clamps_rate() -> void:
	var smoother := NovaColorSmoother.new()
	smoother.snap(Color(1.0, 0.0, 0.0))
	# max_step is in byte units; a 1-byte cap limits a full-range channel to a
	# single byte per tick (255 -> 254) instead of the default eighth-step.
	var stepped := smoother.step(Color(0.0, 0.0, 0.0), 1.0)
	assert_eq(int(stepped.r * 255.0 + 0.5), 254, "A 1-byte max step caps decay at one byte per tick.")
