#pragma once

#include <godot_cpp/classes/texture.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <base/vfs/file_source.h>

namespace opennova {

// Where a model's textures come from when it is not a mounted model (ObjectData::
// open_from_model): an embedder's own files by flat name (the editor's project), read
// the way ResourceRoot reads a mount's, through the same candidate list and the
// renderer's material-texture rule. The files are an archive's as the game would mount
// them: a loose file never takes precedence.
class TextureFiles {
public:
	explicit TextureFiles(std::shared_ptr<const FileSource> files) : files_(std::move(files)) {}

	// A texture by name, the first candidate that decodes (texture_candidate_filenames).
	godot::Ref<godot::Texture2D> load_texture(const godot::String &name) const;
	// One material row's texture of runtime `type` (ResourceRoot::load_material_texture's rule).
	godot::Ref<godot::Texture> load_material_texture(const godot::String &name, uint8_t type) const;

private:
	godot::PackedByteArray read_(const godot::String &name) const;
	bool has_(const std::string &name) const;

	std::shared_ptr<const FileSource> files_;
	mutable std::unordered_map<std::string, godot::Ref<godot::Texture2D>> cache_;
	// A material row's chunk textures by type and file ("material-chunk:<type>:<file>").
	mutable std::unordered_map<std::string, godot::Ref<godot::Texture>> chunks_;
};

} // namespace opennova
