#include "util/texture_files.h"

#include <runtime/renderer/material_texture.h>

#include "util/data_format.h"
#include "util/string_convert.h"
#include "util/texture_path_resolver.h"

#include <string>
#include <vector>

using namespace godot;

namespace opennova {

PackedByteArray TextureFiles::read_(const String &name) const {
	std::vector<uint8_t> bytes;
	if (!files_ || name.is_empty() || !files_->read(to_std(name), bytes)) return PackedByteArray();
	return to_packed_bytes(bytes);
}

bool TextureFiles::has_(const std::string &name) const {
	return files_ && !name.empty() && files_->stamp(name) != 0;
}

Ref<Texture2D> TextureFiles::load_texture(const String &name) const {
	const String file = name.replace("\\", "/").get_file();
	if (file.is_empty()) return Ref<Texture2D>();
	const std::string key = "texture:" + to_std(file.to_lower());
	const auto cached = cache_.find(key);
	if (cached != cache_.end()) return cached->second;
	Ref<Texture2D> result;
	for (const String &candidate : texture_candidate_filenames(file)) {
		const PackedByteArray bytes = read_(candidate);
		if (bytes.is_empty()) continue;
		const Ref<Texture2D> texture = load_texture_from_bytes(candidate, bytes);
		if (texture.is_valid()) {
			result = texture;
			break;
		}
	}
	cache_.emplace(key, result);
	return result;
}

// ResourceRoot::load_material_texture's rule over these files: the one file the row's loader
// opens and the reader that decodes it (renderer::material_texture_source), a loose file never
// preferred.
Ref<Texture> TextureFiles::load_material_texture(const String &name, uint8_t type) const {
	renderer::MaterialTextureSource source;
	if (!name.is_empty())
		source = renderer::material_texture_source(to_std(name), type, [this](const std::string &file) { return has_(file); });
	if (source.reader == renderer::MaterialTextureReader::Chunk) {
		// A chunk decoded once per type and file (a model's build decodes its textures ahead of the
		// scene, whose materials load them again).
		const std::string key = "material-chunk:" + std::to_string(type) + ":" + to_std(to_gd(source.file).to_lower());
		auto cached = chunks_.find(key);
		if (cached == chunks_.end())
			cached = chunks_.emplace(key, prepare_material_chunk(read_(to_gd(source.file)), type)).first;
		return cached->second;
	}
	Ref<Texture2D> image;
	if (source.reader != renderer::MaterialTextureReader::None) {
		const std::string key = "material-image:" + std::to_string(static_cast<int>(source.reader)) + ":" +
		                        to_std(to_gd(source.file).to_lower());
		auto cached = cache_.find(key);
		if (cached == cache_.end())
			cached = cache_.emplace(key, load_material_image_from_bytes(source.reader, read_(to_gd(source.file)))).first;
		image = cached->second;
	}
	return prepare_material_texture(image, name, type);
}

} // namespace opennova
