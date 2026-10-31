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
	// The side by the cube quality word, the word by the object texture detail [orig: Game_StartMission
	// @0x52466D..0x524719; Render_InitTextures @0x58F6E0..0x58F74A].
	CHECK(kCubeQualityWordInitial == 2);
	CHECK(cube_quality_word(0) == 0 && cube_quality_word(1) == 1 && cube_quality_word(2) == 2 &&
			cube_quality_word(3) == 2);
	CHECK(environment_cube_face_size(0) == 64 && environment_cube_face_size(1) == 128 &&
			environment_cube_face_size(2) == 256 && environment_cube_face_size(3) == 128);
	CHECK(environment_cube_face_size(cube_quality_word(3)) == 256);
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
	// Player + 1 without terrain; raised to terrain + the title's clearance when that is higher:
	// Jointops.exe's 10, dfx2.exe's 5 (DFX2-43) [orig: EnvCube_Update @0x61072B; @0x60F67B
	// (dfx2.exe)].
	const opennova::Title &jo = *opennova::title_by_code("jo");
	const opennova::Title &dfx2 = *opennova::title_by_code("dfx2");
	CHECK(jo.env_cube_terrain_clearance == 0xA0000 && dfx2.env_cube_terrain_clearance == 0x50000);
	CHECK(opennova::title_by_code("acidgambit")->env_cube_terrain_clearance == 0xA0000);
	CHECK(environment_cube_eye_height(jo, 34.0f, false, 0.0f) == 35.0f);
	CHECK(environment_cube_eye_height(jo, 34.0f, true, 20.0f) == 35.0f);
	CHECK(environment_cube_eye_height(jo, 34.0f, true, 30.0f) == 40.0f);
	CHECK(environment_cube_eye_height(jo, -5.0f, true, -5.0f) == 5.0f);
	CHECK(environment_cube_eye_height(dfx2, 34.0f, false, 0.0f) == 35.0f);
	CHECK(environment_cube_eye_height(dfx2, 34.0f, true, 30.0f) == 35.0f);
	CHECK(environment_cube_eye_height(dfx2, 34.0f, true, 33.0f) == 38.0f);
	CHECK(environment_cube_eye_height(dfx2, -5.0f, true, -5.0f) == 0.0f);
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

// The render subsystems' init creates the cube, and a dedicated host's start jumps over it
// (D-NET-1231) [orig: Game_StartMission @0x524360 - the dedicated test @0x5259B4 skipping
// the call @0x5259D1; Render_InitRenderingSubsystems @0x586EE0 - Render_InitTextures, the call
// @0x586EFC].
void check_cube_creates_on_the_full_arm_alone() {
	CHECK(creates_environment_cube(/*dedicated_host=*/false));
	CHECK(!creates_environment_cube(/*dedicated_host=*/true));
}

} // namespace

int main() {
	check_constants();
	check_refresh_cadence();
	check_eye_height();
	check_dim_byte();
	check_cube_creates_on_the_full_arm_alone();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("renderer_environment_cube: ok\n");
	return 0;
}
