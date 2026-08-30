#include <runtime/renderer/device_fog.h>

#include <formats/env/env_weather.h>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                        \
		if (!(condition)) {                                                      \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
			++failures;                                                           \
		}                                                                       \
	} while (0)

bool nearly(float lhs, float rhs, float tolerance = 1.0e-5f) {
	return std::fabs(lhs - rhs) <= tolerance;
}

using opennova::renderer::device_fog_visibility;
using opennova::renderer::kDeviceFogLn64;

void check_disabled_and_clamps() {
	CHECK(device_fog_visibility(5000.0f, 0.0f, 100.0f, 1, false) == 1.0f);
	CHECK(device_fog_visibility(-10.0f, 0.5f, 100.0f, 1, true) == 1.0f);
	CHECK(device_fog_visibility(100.0f, 0.5f, 100.0f, 1, true) == 0.0f);
	CHECK(device_fog_visibility(1000.0f, 0.5f, 100.0f, 2, true) == 0.0f);
}

void check_exponential_type_zero() {
	// D3DFOG_EXP with density ln(64)/end: 1/64 visibility at the fog end,
	// independent of the resolved start.
	CHECK(nearly(kDeviceFogLn64, std::log(64.0f), 1.0e-6f));
	CHECK(nearly(device_fog_visibility(100.0f, 0.0f, 100.0f, 0, true),
			1.0f / 64.0f));
	CHECK(nearly(device_fog_visibility(100.0f, 75.0f, 100.0f, 0, true),
			1.0f / 64.0f));
	CHECK(nearly(device_fog_visibility(50.0f, 0.0f, 100.0f, 0, true),
			std::exp(-kDeviceFogLn64 * 0.5f)));
	CHECK(device_fog_visibility(0.0f, 0.0f, 100.0f, 0, true) == 1.0f);
}

void check_linear_types_use_the_resolved_start() {
	// Type 1: linear from the caller's 0.5 start.
	CHECK(nearly(device_fog_visibility(50.25f, 0.5f, 100.0f, 1, true), 0.5f));
	// Types 2/3 consume the Render_SetFogState start as resolved by
	// compute_fog_params, including the (1 - overcast) factor, and never
	// re-derive it from the end distance.
	const opennova::env::FogParams clear_2 =
			opennova::env::compute_fog_params(2, 100.0f, 0.0f);
	CHECK(nearly(clear_2.start, 50.0f));
	CHECK(nearly(device_fog_visibility(75.0f, clear_2.start, clear_2.end, 2,
			true), 0.5f));
	const opennova::env::FogParams overcast_2 =
			opennova::env::compute_fog_params(2, 100.0f, 0.5f);
	CHECK(nearly(overcast_2.start, 25.0f));
	CHECK(nearly(device_fog_visibility(75.0f, overcast_2.start, overcast_2.end,
			2, true), 1.0f / 3.0f));
	const opennova::env::FogParams clear_3 =
			opennova::env::compute_fog_params(3, 100.0f, 0.0f);
	CHECK(nearly(clear_3.start, 25.0f));
	CHECK(nearly(device_fog_visibility(62.5f, clear_3.start, clear_3.end, 3,
			true), 0.5f));
	// A degenerate start == end range is guarded (the device disables fog
	// there; the visibility falls off over one unit instead of dividing by 0).
	CHECK(nearly(device_fog_visibility(99.5f, 100.0f, 100.0f, 1, true), 0.5f));
}

} // namespace

int main() {
	check_disabled_and_clamps();
	check_exponential_type_zero();
	check_linear_types_use_the_resolved_start();
	if (failures != 0) {
		std::printf("renderer_device_fog: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_device_fog ok\n");
	return 0;
}
