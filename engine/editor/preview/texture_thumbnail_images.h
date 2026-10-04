#pragma once

#include <cstdint>

namespace opennova::editor {

struct TextureThumbnail;

// Where the windows find a texture thumbnail's picture on the GPU (ADR 0046 S18): the Shell's device
// (godot/src/authoring/thumbnail_images), which uploads each picture once (by its serial), a few a
// frame so a list of hundreds of textures stays smooth, and keeps a bounded number of them; none
// headless or in a test, where the windows draw a framed box in the picture's place.
class TextureThumbnailImages {
public:
	virtual ~TextureThumbnailImages() = default;
	// The ImGui texture id the thumbnail's picture draws with this frame; 0 while there is none (a
	// picture with no texels, or one not uploaded yet: the frame's uploads spent).
	virtual uint64_t texture_id(const TextureThumbnail &thumbnail) = 0;
};

} // namespace opennova::editor
