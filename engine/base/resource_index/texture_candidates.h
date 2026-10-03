// The texture file names a NovaLogic texture reference may resolve to, in
// probe order. Models, terrain and menus name textures with inconsistent case
// and an extension that often differs from the file on disk (a `.tga` name
// served by a `.dds`, compound `x.dds.tga`, overlay `_O` twins), so every
// resolver of a texture no model row names tries the same list: the Godot
// runtime's loose-folder and archive lookups
// (godot/src/util/texture_path_resolver.cpp,
// godot/src/resource_index/resource_root.cpp) and the editor's graph. A
// model's texture row loads by its loader's own rule instead
// (runtime/renderer/material_texture.h). Case is left to the caller's
// case-insensitive directory match.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace opennova {

// Candidate FILE NAMES (no directories) in priority order: the name as given,
// the inner names a compound extension exposes (`x.dds.tga` -> `x.dds`), then
// every stem (the base name and its `_O` twin) with each fallback extension.
std::vector<std::string> texture_candidate_filenames(const std::string &filename);

// The regular files of one folder, keyed by lower-case file name, their paths in UTF-8
// (a scene text is read as UTF-8). Listed once per scene: a retail asset folder holds
// some 10,000 files. A name UTF-8 cannot carry (an unpaired surrogate) is no texture,
// so it is skipped, never fatal.
using TextureFolder = std::map<std::string, std::string>;
TextureFolder list_texture_folder(const std::filesystem::path &dir);

} // namespace opennova
