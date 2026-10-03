#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/session/view/view_revisions.h>
#include <editor/ui/document_views.h>

namespace opennova::editor {

class TextureViewportView;
struct GraphEdge;

// A texture's Document tab (ADR 0046 S18; the texture type's row of ui/document_views, its MainViewport
// role): a column of what the texture is (its facts in a modder's words: the format, the size, the
// texels, the compression, the alpha, the mip levels, the palette, and whether the game loads it), its
// palette as swatches where it has one (an entry's index and colour as the pointer rests on it), and
// what it is used as (the session's texture uses, S18: each use by its role and where, a model's
// material with its shader and cut-out, a name the game opens itself with what for; a use whose loader
// opens another file said so; a Go to on each referrer); beside it the texture viewport filling the rest
// (ui/texture_viewport_view).
class TextureView final : public DocumentView {
public:
	TextureView();
	~TextureView() override;

	void draw(Workspace &workspace, const DocumentBase &document) override;
	bool main_viewport(Workspace &workspace, const DocumentBase &document) override;
	void end_frame(Workspace &workspace) override;
	bool holds_back_itself() const override { return true; }

private:
	void draw_info(Workspace &workspace, const DocumentBase &document);
	void draw_palette(const std::vector<uint8_t> &palette);
	void draw_uses(Workspace &workspace, const DocumentBase &document);

	std::unique_ptr<TextureViewportView> viewport_;
};

std::unique_ptr<DocumentView> make_texture_view();

} // namespace opennova::editor
