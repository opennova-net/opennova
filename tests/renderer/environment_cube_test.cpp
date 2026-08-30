#include <runtime/renderer/environment_cube.h>

#include <cstdint>
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

using namespace opennova::renderer;

void check_constants() {
	CHECK(kEnvironmentCubeFaceCount == 6);
	CHECK(kEnvironmentCubeFaceSize == 256);
	CHECK(kEnvironmentCubeRefreshFrames == 128);
	CHECK(kEnvironmentCubeDimByte == 0x60);
	CHECK(kEnvironmentCubeFaceFovDegrees == 90.0f);
	CHECK(kEnvironmentCubeFaceNear == 0.5f);
	CHECK(kEnvironmentCubeFaceFar == 1000.0f);
}

void check_refresh_cadence() {
	// First frame, forced, and every 128th frame; nothing in between.
	CHECK(environment_cube_refresh_due(0, false, false));
	CHECK(environment_cube_refresh_due(0, false, true));
	CHECK(!environment_cube_refresh_due(1, false, true));
	CHECK(environment_cube_refresh_due(1, true, true));
	CHECK(environment_cube_refresh_due(5, false, false));
	CHECK(!environment_cube_refresh_due(127, false, true));
	CHECK(environment_cube_refresh_due(128, false, true));
	CHECK(!environment_cube_refresh_due(129, false, true));
	CHECK(environment_cube_refresh_due(256, false, true));
}

void check_eye_height() {
	// Player + 1 without terrain; raised to terrain + 10 when that is higher.
	CHECK(environment_cube_eye_height(34.0f, false, 0.0f) == 35.0f);
	CHECK(environment_cube_eye_height(34.0f, true, 20.0f) == 35.0f);
	CHECK(environment_cube_eye_height(34.0f, true, 30.0f) == 40.0f);
	CHECK(environment_cube_eye_height(-5.0f, true, -5.0f) == 5.0f);
}

void check_dim_byte() {
	// The 0x60/255 multiply, rounded half up: full white lands exactly on the
	// vertex diffuse, black stays black, and the ramp is monotonic.
	CHECK(environment_cube_dim_byte(0xFF) == 0x60);
	CHECK(environment_cube_dim_byte(0x00) == 0x00);
	CHECK(environment_cube_dim_byte(0x80) == 48);
	CHECK(environment_cube_dim_byte(0x73) == 43);
	CHECK(environment_cube_dim_byte(0x01) == 0);
	CHECK(environment_cube_dim_byte(0x02) == 1);
	std::uint8_t previous = 0;
	for (int value = 0; value < 256; ++value) {
		const std::uint8_t dimmed =
				environment_cube_dim_byte(static_cast<std::uint8_t>(value));
		const unsigned reference = (static_cast<unsigned>(value) * 96u + 127u) / 255u;
		CHECK(dimmed == reference);
		CHECK(dimmed >= previous);
		CHECK(dimmed <= 0x60);
		previous = dimmed;
	}
}

} // namespace

int main() {
	check_constants();
	check_refresh_cadence();
	check_eye_height();
	check_dim_byte();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_environment_cube: ok\n");
	return 0;
}
