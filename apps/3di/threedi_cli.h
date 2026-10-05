// opennova-3di internals: one translation unit per command, dispatched by
// main.cpp. `build` (build.cpp) mints a .3di from .o3d scene text, `scene`
// (scene.cpp) writes a .3di back out as .o3d for an importer, `info`
// (info.cpp) prints a model, `compare` (compare.cpp) tells whether two models
// are the same model, and `catalog` (main.cpp) prints the engine's tables a
// front end offers: the CTRL registers, the generator styles, the shader tags,
// the anim slot keys, the weapon actions and the event trigger bits. The
// `anim` commands keep their own surface (anim_cli.h), and the `weapon`
// commands theirs (weapon_timing.h).
#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <formats/threedi/scene_text.h>

namespace opennova::threedi_cli {

// A scene reader's findings on stderr as `path:line: message` (`path: message`
// for the whole file), notes marked `note:`. True when none is an error.
inline bool print_findings(const char *path, const std::vector<opennova::threedi::SceneFinding> &findings) {
	bool clean = true;
	for (const opennova::threedi::SceneFinding &f : findings) {
		clean = clean && !f.error;
		if (f.line > 0) std::fprintf(stderr, "%s:%d: %s%s\n", path, f.line, f.error ? "" : "note: ", f.message.c_str());
		else std::fprintf(stderr, "%s: %s%s\n", path, f.error ? "" : "note: ", f.message.c_str());
	}
	return clean;
}

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
