// One folder's files as a texture lookup reads them: opennova-3di's `scene` resolves a model's
// texture rows against the folder the model sits in, by the file each row's loader opens
// (runtime/renderer/material_texture.h), with no case. The game never lists a folder: each retail
// loader opens exactly the file its own rule names (runtime/renderer/texture_load_rules.h).
#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace opennova {

// The regular files of one folder, keyed by lower-case file name, their paths in UTF-8
// (a scene text is read as UTF-8). Listed once per scene: a retail asset folder holds
// some 10,000 files. A name UTF-8 cannot carry (an unpaired surrogate) is no texture,
// so it is skipped, never fatal.
using TextureFolder = std::map<std::string, std::string>;
TextureFolder list_texture_folder(const std::filesystem::path &dir);

} // namespace opennova
