#include <runtime/renderer/device_fog.h>

#include <formats/env/env_weather.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// The .gdshader passes' copy of the law (tests/CMakeLists.txt defines its path in the source tree).
#ifndef DEVICE_FOG_GDSHADERINC
#define DEVICE_FOG_GDSHADERINC "godot/shaders/device_fog.gdshaderinc"
#endif

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

using opennova::renderer::device_fog_exponential;
using opennova::renderer::device_fog_visibility;
using opennova::renderer::kDeviceFogGlsl;
using opennova::renderer::kDeviceFogLn64;

void check_disabled_and_clamps() {
	CHECK(device_fog_visibility(5000.0f, 5000.0f, 0.0f, 100.0f, 1, false) == 1.0f);
	CHECK(device_fog_visibility(-10.0f, -10.0f, 0.5f, 100.0f, 1, true) == 1.0f);
	CHECK(device_fog_visibility(100.0f, 100.0f, 0.5f, 100.0f, 1, true) == 0.0f);
	CHECK(device_fog_visibility(1000.0f, 1000.0f, 0.5f, 100.0f, 2, true) == 0.0f);
}

void check_exponential_type_zero() {
	// D3DFOG_EXP with density ln(64)/end: 1/64 visibility at the fog end,
	// independent of the resolved start, over the view depth (table fog or
	// non-range vertex fog: the radial distance does not enter).
	CHECK(nearly(kDeviceFogLn64, std::log(64.0f), 1.0e-6f));
	CHECK(nearly(device_fog_visibility(100.0f, 500.0f, 0.5f, 100.0f, 0, true),
			1.0f / 64.0f));
	CHECK(nearly(device_fog_visibility(100.0f, 100.0f, 75.0f, 100.0f, 0, true),
			1.0f / 64.0f));
	CHECK(nearly(device_fog_visibility(50.0f, 90.0f, 0.5f, 100.0f, 0, true),
			std::exp(-kDeviceFogLn64 * 0.5f)));
	CHECK(device_fog_visibility(0.0f, 0.0f, 0.5f, 100.0f, 0, true) == 1.0f);
	CHECK(device_fog_exponential(0.5f, 100.0f, 0));
	CHECK(!device_fog_exponential(0.5f, 100.0f, 1));
}

void check_linear_types_use_the_resolved_start() {
	// Type 1: linear from the caller's 0.5 start, over the radial distance
	// (RANGEFOGENABLE on: the view depth does not enter).
	CHECK(nearly(device_fog_visibility(10.0f, 50.25f, 0.5f, 100.0f, 1, true), 0.5f));
	// Types 2/3 consume the Render_SetFogState start as resolved by
	// compute_fog_params, including the (1 - overcast) factor, and never
	// re-derive it from the end distance.
	const opennova::env::FogParams clear_2 =
			opennova::env::compute_fog_params(2, 100.0f, 0.0f);
	CHECK(nearly(clear_2.start, 50.0f));
	CHECK(nearly(device_fog_visibility(75.0f, 75.0f, clear_2.start, clear_2.end, 2,
			true), 0.5f));
	const opennova::env::FogParams overcast_2 =
			opennova::env::compute_fog_params(2, 100.0f, 0.5f);
	CHECK(nearly(overcast_2.start, 25.0f));
	CHECK(nearly(device_fog_visibility(75.0f, 75.0f, overcast_2.start, overcast_2.end,
			2, true), 1.0f / 3.0f));
	const opennova::env::FogParams clear_3 =
			opennova::env::compute_fog_params(3, 100.0f, 0.0f);
	CHECK(nearly(clear_3.start, 25.0f));
	CHECK(nearly(device_fog_visibility(62.5f, 62.5f, clear_3.start, clear_3.end, 3,
			true), 0.5f));
	// A short range keeps its own slope (no one-unit floor on the divisor).
	CHECK(nearly(device_fog_visibility(0.7f, 0.7f, 0.5f, 0.9f, 1, true), 0.5f));
}

void check_degenerate_range_parks() {
	// start == end parks the device fog at vertex LINEAR 1000..1001 with range
	// fog off [orig: CD3DDevice_SetFogParameters @ 0x677973..0x6779a7], for
	// every type: unfogged short of view depth 1000, fully fogged past 1001.
	CHECK(device_fog_visibility(999.0f, 5000.0f, 100.0f, 100.0f, 1, true) == 1.0f);
	CHECK(nearly(device_fog_visibility(1000.5f, 1000.5f, 100.0f, 100.0f, 1, true), 0.5f));
	CHECK(device_fog_visibility(1002.0f, 1002.0f, 0.0f, 0.0f, 2, true) == 0.0f);
	CHECK(device_fog_visibility(500.0f, 500.0f, 0.5f, 0.5f, 0, true) == 1.0f);
	CHECK(!device_fog_exponential(0.5f, 0.5f, 0));
	// compute_fog_params' degenerate pair (types 2/3 at end 0) parks.
	const opennova::env::FogParams zero_2 = opennova::env::compute_fog_params(2, 0.0f, 0.0f);
	CHECK(device_fog_visibility(10.0f, 10.0f, zero_2.start, zero_2.end, 2, true) == 1.0f);
}

void check_shader_copy_is_the_engine_text() {
	// The GLSL form spells the C++ constants.
	const std::string glsl(kDeviceFogGlsl);
	CHECK(glsl.find("4.1588830833596715") != std::string::npos);
	CHECK(glsl.find("1001.0 - 1000.0") != std::string::npos);
	CHECK(opennova::renderer::kDeviceFogParkedStart == 1000.0f);
	CHECK(opennova::renderer::kDeviceFogParkedEnd == 1001.0f);
	// godot/shaders/device_fog.gdshaderinc carries the text verbatim (a
	// checkout's CR line ends aside).
	std::ifstream file(DEVICE_FOG_GDSHADERINC, std::ios::binary);
	CHECK(file.good());
	std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::string normalized;
	normalized.reserve(text.size());
	for (char c : text) {
		if (c != '\r') {
			normalized.push_back(c);
		}
	}
	if (normalized.find(glsl) == std::string::npos) {
		std::printf("FAIL: %s differs from renderer/device_fog.h kDeviceFogGlsl; "
				"copy the engine text into it\n",
				DEVICE_FOG_GDSHADERINC);
		++failures;
	}
}

} // namespace

int main() {
	check_disabled_and_clamps();
	check_exponential_type_zero();
	check_linear_types_use_the_resolved_start();
	check_degenerate_range_parks();
	check_shader_copy_is_the_engine_text();
	if (failures != 0) {
		std::printf("renderer_device_fog: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_device_fog ok\n");
	return 0;
}
