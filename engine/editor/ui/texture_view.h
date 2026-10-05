#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/session/texture_import_state.h>
#include <editor/session/view/view_revisions.h>
#include <editor/ui/document_views.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

class TextureViewportView;
struct GraphEdge;
struct TextureImage;

// A texture's Document tab (ADR 0046 S18; the texture type's row of ui/document_views, its MainViewport
// role): a column of what the texture is (its facts in a modder's words: the format, the size, the
// texels, the compression, the alpha, the mip levels, the palette, and whether the game loads it), its
// palette as swatches where it has one (an entry's index and colour as the pointer rests on it), how it is
// made where an import makes it (its source, each option of the import that applies as a control that sets
// it, what its uses ask of it and the one click that makes it so), else its whole-image edits (its size,
// its alpha, its stored form, an upside-down TGA's rows, an 8-bit PCX's indices: texture_operation, one
// undo step each), and what it is used as (the session's texture uses, S18: each use by its role and where, a model's
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
	void draw_import(Workspace &workspace, const DocumentBase &document);
	void draw_edits(Workspace &workspace, const DocumentBase &document, const TextureImage &image);
	static bool replaceable_image(const std::string &path);

	std::unique_ptr<TextureViewportView> viewport_;
	// A palette index move as typed (the workspace's, as last taken).
	int remap_from_ = 0, remap_to_ = 0;
	ui_kit::Held<std::pair<int, int>> remap_held_;
	// The import shown, read again when the file or what it reads moves; a typed value as it is typed.
	struct ImportShown {
		bool made = false;
		RevisionKey key;
		std::string path;
		bool imported = false;
		TextureImportState state;
		std::map<std::string, std::string> drafts;
	};
	ImportShown import_;
};

std::unique_ptr<DocumentView> make_texture_view();

} // namespace opennova::editor
