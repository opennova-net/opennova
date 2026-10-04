#include "authoring/thumbnail_images.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstring>

#include <editor/preview/texture_thumbnails.h>

namespace godot {

Ref<ImageTexture> ThumbnailImages::upload_(const opennova::editor::TextureThumbnail &thumbnail) {
	if (thumbnail.width == 0 || thumbnail.height == 0 || thumbnail.rgba.size() < size_t(thumbnail.width) * thumbnail.height * 4)
		return Ref<ImageTexture>();
	PackedByteArray bytes;
	bytes.resize(int64_t(thumbnail.rgba.size()));
	std::memcpy(bytes.ptrw(), thumbnail.rgba.data(), thumbnail.rgba.size());
	const Ref<Image> picture =
			Image::create_from_data(int32_t(thumbnail.width), int32_t(thumbnail.height), false, Image::FORMAT_RGBA8, bytes);
	Ref<ImageTexture> texture = ImageTexture::create_from_image(picture);
	recency_.push_front(thumbnail.serial);
	textures_[thumbnail.serial] = Held{texture, recency_.begin()};
	// The least recently drawn let go past the most it keeps.
	while (textures_.size() > kHeld && !recency_.empty()) {
		textures_.erase(recency_.back());
		recency_.pop_back();
	}
	return texture;
}

Ref<ImageTexture> ThumbnailImages::texture_of(const opennova::editor::TextureThumbnail &thumbnail) {
	const auto found = textures_.find(thumbnail.serial);
	if (found != textures_.end()) {
		recency_.splice(recency_.begin(), recency_, found->second.used);
		return found->second.texture;
	}
	return upload_(thumbnail);
}

uint64_t ThumbnailImages::texture_id(const opennova::editor::TextureThumbnail &thumbnail) {
	const auto found = textures_.find(thumbnail.serial);
	if (found == textures_.end() && uploads_ >= kUploadsPerFrame) return 0;
	if (found == textures_.end()) ++uploads_;
	const Ref<ImageTexture> texture = texture_of(thumbnail);
	return texture.is_valid() ? texture->get_rid().get_id() : 0;
}

} // namespace godot
