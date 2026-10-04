#pragma once

#include <godot_cpp/classes/image_texture.hpp>

#include <cstdint>
#include <list>
#include <unordered_map>

#include <editor/preview/texture_thumbnail_images.h>

namespace godot {

// The texture thumbnails' device (ADR 0046 S18): each thumbnail's picture (the portable cache's texels,
// editor/preview/texture_thumbnails, nothing decoded here) uploaded once as an ImageTexture, by its
// serial, at most kUploadsPerFrame a frame so a list of hundreds of textures stays smooth, and the
// kHeld most recently drawn kept; the windows draw each through the ImGui pass by its texture's RID
// (the id imgui-godot's renderer reads an ImTextureID as).
class ThumbnailImages final : public opennova::editor::TextureThumbnailImages {
public:
	static constexpr int kUploadsPerFrame = 24;
	static constexpr size_t kHeld = 512;

	uint64_t texture_id(const opennova::editor::TextureThumbnail &thumbnail) override;
	// The texture of a thumbnail, uploaded now whatever this frame's uploads (a test's read); null for a
	// picture with no texels.
	Ref<ImageTexture> texture_of(const opennova::editor::TextureThumbnail &thumbnail);
	// A frame begins: its uploads counted afresh.
	void begin_frame() { uploads_ = 0; }
	size_t held() const { return textures_.size(); }

private:
	struct Held {
		Ref<ImageTexture> texture;
		std::list<uint64_t>::iterator used;
	};
	Ref<ImageTexture> upload_(const opennova::editor::TextureThumbnail &thumbnail);

	std::unordered_map<uint64_t, Held> textures_; // by serial
	std::list<uint64_t> recency_;                 // most recently drawn first
	int uploads_ = 0;
};

} // namespace godot
