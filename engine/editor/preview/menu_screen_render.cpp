#include <editor/preview/menu_screen_render.h>

#include <editor/documents/mnu_document.h>

namespace opennova::editor {

MenuScreenRender::MenuScreenRender() = default;

MenuScreenRender::~MenuScreenRender() {
	assets_.clear(compiler_, decoder_);
}

MenuPreviewStatus MenuScreenRender::configure(const MnuDocument &document, NodeId screen_row, const FileSource &files,
                                              const std::map<std::string, std::string> &vars) {
	state_ = menu::MenuFrameState();
	detail_.clear();
	revision_ = document.revision();
	std::vector<SourceIssue> issues;
	std::shared_ptr<const mnu::Document> image = document.saved_image(&issues);
	const Node *row = document.row(screen_row);
	const size_t position = document.screen_position(screen_row);
	if (!image || !row || position >= image->screens.size()) {
		assets_.clear(compiler_, decoder_);
		image_.reset();
		status_ = !image ? MenuPreviewStatus::Unserializable : MenuPreviewStatus::ScreenMissing;
		if (!image && !issues.empty()) detail_ = issues.front().message;
		else if (image && row) detail_ = row->name();
		return status_;
	}
	// Every configure is a first load of its textures (as the preview's): no other menu's
	// first load fixes a band height here.
	compiler_.reset_texture_loads();
	assets_.configure(compiler_, image.get(), &image->screens[position], files, decoder_, vars);
	image_ = std::move(image); // after the configure: the compiler borrowed the new image
	status_ = MenuPreviewStatus::Ready;
	return status_;
}

const menu::MenuDrawList &MenuScreenRender::compile(float scale_x, float scale_y) {
	return compiler_.compile(state_, scale_x, scale_y);
}

std::vector<menu::MenuFrameNote> MenuScreenRender::notes() const {
	std::vector<menu::MenuFrameNote> notes;
	if (status_ != MenuPreviewStatus::Ready) return notes;
	notes = compiler_.build_notes();
	for (menu::MenuFrameNote &note : compiler_.layout_notes(state_)) notes.push_back(std::move(note));
	return notes;
}

} // namespace opennova::editor
