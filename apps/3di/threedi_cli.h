// opennova-3di internals: one translation unit per command, dispatched by
// main.cpp. `build` (build.cpp) mints a .3di from .o3d scene text, `scene`
// (scene.cpp) writes a .3di back out as .o3d for an importer, `info`
// (info.cpp) prints a model, `compare` (compare.cpp) tells whether two models
// are the same model, and `catalog` (main.cpp) prints the engine's tables a
// front end offers: the CTRL registers, the generator styles, the shader tags,
// the anim slot keys and the event trigger bits. The `anim` commands keep
// their own surface (anim_cli.h).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::threedi_cli {

// The seven PANM tracks in on-disk order and their .o3d target names.
inline constexpr int kTrackCount = 7;
inline const char *track_label(int t) {
	static const char *const kNames[kTrackCount] = {"rotx", "roty", "rotz", "scalex", "scaley", "scalez", "trans"};
	return t >= 0 && t < kTrackCount ? kNames[t] : "?";
}
inline int track_index(const std::string &name) {
	for (int i = 0; i < kTrackCount; ++i)
		if (name == track_label(i)) return i;
	return -1;
}
// A PANM row's tracks in that order.
inline std::array<opennova::threedi::ThreediTransform *, kTrackCount> panm_tracks(
		opennova::threedi::ThreediPartAnimation &pa) {
	return {&pa.rotation_x, &pa.rotation_y, &pa.rotation_z, &pa.scale_x, &pa.scale_y, &pa.scale_z, &pa.translation};
}
inline std::array<const opennova::threedi::ThreediTransform *, kTrackCount> panm_tracks(
		const opennova::threedi::ThreediPartAnimation &pa) {
	return {&pa.rotation_x, &pa.rotation_y, &pa.rotation_z, &pa.scale_x, &pa.scale_y, &pa.scale_z, &pa.translation};
}

// A colour channel (0..1) as the byte it was authored as: the inverse of the
// builder's threedi_byte_unit.
inline int byte_of(float unit) { return static_cast<int>(std::lround(unit * 255.0f)); }

// Write `size` bytes to `path` whole or not at all: into `path`.part, checked
// through fclose, then renamed over `path`. A full disk, a crash or a refused
// model never leaves a truncated file where the last good one was.
inline bool write_output(const char *path, const void *data, size_t size) {
	const std::filesystem::path target(path);
	std::filesystem::path part = target;
	part += ".part";
	FILE *f = std::fopen(part.string().c_str(), "wb");
	bool ok = f != nullptr;
	if (ok) {
		ok = size == 0 || std::fwrite(data, 1, size, f) == size;
		ok = std::fclose(f) == 0 && ok;
	}
	std::error_code ec;
	if (ok) std::filesystem::rename(part, target, ec);
	if (!ok || ec) {
		std::filesystem::remove(part, ec);
		std::fprintf(stderr, "opennova-3di: cannot write %s\n", path);
		return false;
	}
	return true;
}

int cmd_info(const char *path, int verbose);
int cmd_build(const char *scene_path, const char *out_path);
int cmd_scene(const char *model_path, const char *out_path);
// `strict`: drift (a heuristic derived value, or a move within tolerance)
// counts as a difference too.
int cmd_compare(const char *expected_path, const char *actual_path, bool strict = false);

} // namespace opennova::threedi_cli
