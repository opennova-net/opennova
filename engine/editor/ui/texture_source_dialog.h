#pragma once

#include <cstdint>

namespace opennova::editor {

class Workspace;

// The dialog that asks before a texture is made from an image (ADR 0046 S18): a Replace (an image picked or
// dropped) or an Edit externally of a texture with no source yet. It draws the view's texture_source preview
// (PreviewTextureSource) while it is open: the texture now and then, each a picture and its sides and form;
// the stored forms the texture's name offers, a choice planning again; what changes, in words; and the one
// button that does it (replace_texture, or edit_externally), or Cancel (cancel_texture_source). Refused, it
// says why and offers Cancel alone. Drawn every frame with the workspace's modals.
class TextureSourceDialog {
public:
	void draw(Workspace &workspace);

private:
	uint64_t shown_ = 0; // the preview serial the popup is open on
};

} // namespace opennova::editor
