#pragma once

// The witnessed CubeEnvironment facts: the highest-quality face size,
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
// Highest quality; the 64 and 128 selections of the lower settings are not
// presented.
inline constexpr int kEnvironmentCubeFaceSize = 256;
inline constexpr std::int64_t kEnvironmentCubeRefreshFrames = 128;
inline constexpr float kEnvironmentCubeEyeRaise = 1.0f;
inline constexpr float kEnvironmentCubeTerrainClearance = 10.0f;
inline constexpr float kEnvironmentCubeFaceFovDegrees = 90.0f;
inline constexpr float kEnvironmentCubeFaceNear = 0.5f;
inline constexpr float kEnvironmentCubeFaceFar = 1000.0f;
// The vertex diffuse of the dim quad: 0xFF606060.
inline constexpr std::uint8_t kEnvironmentCubeDimByte = 0x60;

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
