// The texture file names a NovaLogic texture reference may resolve to, in
// probe order. Models, terrain and menus name textures with inconsistent case
// and an extension that often differs from the file on disk (a `.tga` name
// served by a `.dds`, compound `x.dds.tga`, overlay `_O` twins), so every
// resolver tries the same list: the Godot runtime's loose-folder and archive
// lookups (godot/src/util/texture_path_resolver.cpp,
// godot/src/resource_index/resource_root.cpp) and opennova-3di's `scene`,
// which tells an importer which file sits beside a model. Case is left to the
// caller's case-insensitive directory match.
#pragma once

#include <string>
#include <vector>

namespace opennova {

// Candidate FILE NAMES (no directories) in priority order: the name as given,
// the inner names a compound extension exposes (`x.dds.tga` -> `x.dds`), then
// every stem (the base name and its `_O` twin) with each fallback extension.
std::vector<std::string> texture_candidate_filenames(const std::string &filename);

} // namespace opennova
