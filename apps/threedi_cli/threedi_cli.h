// opennova-3di internals: one translation unit per command, dispatched by
// main.cpp. `build` (build.cpp) mints a .3di from .o3d scene text, `scene`
// (scene.cpp) writes a .3di back out as .o3d for an importer, `info`
// (info.cpp) prints a model, `compare` (compare.cpp) tells whether two models
// are the same model, and `catalog` (main.cpp) prints the engine's CTRL
// register and generator-style tables for a front end.
#pragma once

#include <string>

namespace threedi_cli {

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

int cmd_info(const char *path, int verbose);
int cmd_build(const char *scene_path, const char *out_path);
int cmd_scene(const char *model_path, const char *out_path);
int cmd_compare(const char *expected_path, const char *actual_path);

} // namespace threedi_cli
