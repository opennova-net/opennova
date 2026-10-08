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

// ResourceRoot::load_texture's rule over these files: the files the loader opens for the name
// (renderer::texture_load_attempts), the first that reads and decodes. The files are an
// archive's, so no loose-first search competes and the particle folder's loose leg reads
// nothing.
Ref<Texture2D> TextureFiles::load_texture(const String &name, renderer::TextureLoader loader) const {
	const String file = name.replace("\\", "/").get_file();
	if (file.is_empty()) return Ref<Texture2D>();
	renderer::TextureFileQuery query;
	query.exists = [this](const std::string &candidate) { return has_(candidate); };
	const std::vector<renderer::TextureLoad> attempts = renderer::texture_load_attempts(loader, to_std(file), query);
	const std::string key = "texture:" + texture_load_key(attempts);
	const auto cached = cache_.find(key);
	if (cached != cache_.end()) return cached->second;
	const Ref<Texture2D> result = load_texture_with_mipmaps(attempts, [this](const renderer::TextureLoad &load) {
		return load.source == renderer::TextureFileSource::Mounted ? read_(to_gd(load.file)) : PackedByteArray();
	});
	cache_.emplace(key, result);
	return result;
}

// ResourceRoot::load_material_texture's rule over these files: the one file the row's loader
// opens and the reader that decodes it (renderer::material_texture_source, as a load:
// renderer::material_texture_load), a loose file never preferred.
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
	const renderer::TextureLoad load = renderer::material_texture_load(source, type);
	Ref<Texture2D> image;
	if (load.reader != renderer::TextureReader::None) {
		const std::string key = "material-image:" + std::to_string(static_cast<int>(load.reader)) + ":" +
		                        std::to_string(static_cast<int>(load.transform)) + ":" +
		                        to_std(to_gd(load.file).to_lower());
		auto cached = cache_.find(key);
		if (cached == cache_.end())
			cached = cache_.emplace(key, load_material_image_from_bytes(load, read_(to_gd(load.file)))).first;
		image = cached->second;
	}
	return prepare_material_texture(image, name, type);
}

} // namespace opennova
