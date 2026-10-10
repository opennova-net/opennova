#include <editor/documents/hud_layout_type.h>

#include <editor/documents/text_types.h>

namespace opennova::editor {

std::unique_ptr<DocumentBase> make_hud_layout_document() {
	// The file is its text; the game's reader ends a line at CR LF alone.
	return std::make_unique<TextDocument>(nullptr, TextLineEnds::CrLf);
}

std::vector<Diagnostic> validate_hud_layout_file(const DocumentBase &) {
	// Its line ends are the line-ends rule's (documents/line_ends.h); it makes nothing else yet.
	return {};
}

FindingTable hud_layout_finding_codes() {
	return {};
}

} // namespace opennova::editor
