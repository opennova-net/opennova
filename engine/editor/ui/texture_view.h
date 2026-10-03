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
// what uses it (the graph's references to the file, each by the referring record's words and field,
// a Go to on each); beside it the texture viewport filling the rest (ui/texture_viewport_view).
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
	void draw_users(Workspace &workspace, const DocumentBase &document);

	std::unique_ptr<TextureViewportView> viewport_;
	// The uses listed, asked of the graph once while it and the file set stand.
	struct Users {
		RevisionKey key;
		std::string file;
		std::vector<const GraphEdge *> edges;
		std::vector<std::string> lines;
	};
	Users users_;
};

std::unique_ptr<DocumentView> make_texture_view();

} // namespace opennova::editor
