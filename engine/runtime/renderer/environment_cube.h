#pragma once

// The witnessed CubeEnvironment facts: the face size by the quality word,
// the six-face refresh cadence, the capture eye placement, the per-face
// 90-degree square view, and the byte multiply the face callback applies to
// every completed face. A device presenter (the Godot capture node) reads
// these; it never re-derives them.
// [orig: Render_InitTextures @ 0x58f6e0 (64/128/256 by quality);
// EnvCube_Update @ 0x6106a0 (all six faces initially, on force,
// and every 128 render frames; eye = local player + 1 Y, raised to at least
// terrain + 10); GTexture_RenderCubeMapFace @ 0x6864d0 (90-degree square
// LH view, near 0.5 / far 1000); face callback @ 0x5c3700 (sky dome and
// sun/moon only, then the 0xFF606060 quad under SRC=DESTCOLOR, DST=ZERO)]

#include <algorithm>
#include <cstdint>

namespace opennova::renderer {

inline constexpr int kEnvironmentCubeFaceCount = 6;

// The cube quality word both cubes take their side from (dword_8409E4, 2 in
// the image): each mission start sets it from the session's object texture
// detail, 0 at detail 0, 1 at detail 1, else 2, ahead of the render
// subsystems' init that makes the cubes [orig: Game_StartMission @0x524360 -
// @0x52466D..0x524719 into sub_589A40 @0x589A40, ahead of
// Render_InitRenderingSubsystems (the call @0x5259D1) -> Render_InitTextures
// @0x58F6E0].
inline constexpr int kCubeQualityWordInitial = 2;
constexpr int cube_quality_word(int object_texdetail) noexcept {
	return object_texdetail == 0 ? 0 : object_texdetail == 1 ? 1 : 2;
}

// The CubeEnvironment's side by that word: 64 at 0, 256 at 2, 128 at any
// other [orig: Render_InitTextures @0x58F6E0..0x58F74A].
constexpr int environment_cube_face_size(int word) noexcept {
	return word == 0 ? 64 : word == 2 ? 256 : 128;
}
inline constexpr std::int64_t kEnvironmentCubeRefreshFrames = 128;
inline constexpr float kEnvironmentCubeEyeRaise = 1.0f;
inline constexpr float kEnvironmentCubeTerrainClearance = 10.0f;
inline constexpr float kEnvironmentCubeFaceFovDegrees = 90.0f;
inline constexpr float kEnvironmentCubeFaceNear = 0.5f;
inline constexpr float kEnvironmentCubeFaceFar = 1000.0f;
// The vertex diffuse of the dim quad: 0xFF606060.
inline constexpr std::uint8_t kEnvironmentCubeDimByte = 0x60;

// Whether a mission start creates the environment cube: the render subsystems' init makes it,
// ahead of the terrain tile cache's page targets, and a dedicated host's start jumps over that
// init. A device backend creates the cube's objects there, ahead of the first capture
// (D-NET-NEW-LF-13).
// [orig: Game_StartMission @0x524360 - the dedicated test @0x5259B4 skipping
//  Render_InitRenderingSubsystems (the call @0x5259D1); Render_InitRenderingSubsystems @0x586EE0 ->
//  Render_InitTextures @0x58F6E0 (the call @0x586EFC, Terrain_CreateTileCacheTargets @0x586F01
//  past it): the cube stored @0x58F750, its target made by GTexture_CreateRenderTarget (the call
//  @0x58F756)]
constexpr bool creates_environment_cube(bool dedicated_host) noexcept {
	return !dedicated_host;
}

// Whether the six faces are re-rendered on this render frame: the first
// frame, a forced refresh, and every 128th frame thereafter.
inline constexpr bool environment_cube_refresh_due(std::int64_t render_frame,
		bool forced, bool cube_ready) {
	return forced || !cube_ready ||
			(render_frame % kEnvironmentCubeRefreshFrames) == 0;
}

// The capture eye height: the local player's origin + 1, raised to at least
// terrain + 10 when a terrain height is known.
inline constexpr float environment_cube_eye_height(float player_y,
		bool has_terrain, float terrain_y) {
	const float raised = player_y + kEnvironmentCubeEyeRaise;
	return has_terrain ?
			std::max(raised, terrain_y + kEnvironmentCubeTerrainClearance) :
			raised;
}

// One framebuffer byte after the dim quad: SRC=DESTCOLOR, DST=ZERO multiplies
// the completed gamma byte by 0x60/255. The fixed-function blender's exact
// rounding is unwitnessed; the ported product rounds half up, and every
// device copy of the faces (the CPU convert of 2026-08-22, the RD blit that
// replaced it) reproduces this byte exactly.
inline constexpr std::uint8_t environment_cube_dim_byte(std::uint8_t value) {
	return static_cast<std::uint8_t>(
			(static_cast<unsigned>(value) * kEnvironmentCubeDimByte + 127u) /
			255u);
}

} // namespace opennova::renderer
