#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/model/node.h>
#include <formats/textlayout/text_layout.h>

namespace opennova::editor {

// What a text record document keeps of its file beside its rows (ADR 0046 S23 B): the file's modeled layout
// (formats/textlayout; the maintainer's ruling of 2026-10-04, "model it, generate it", ADR 0003 holding), which
// its writer generates the file from (each record names its lines by its note), so a file read and saved again
// is the file as it was and one field changed changes that one line. Never shown or edited: carried for the
// save alone. Made once as the file is read and shared by every state of the document, so a state's footprint
// leaves it out (the def catalogs' rule, CatalogFileState).
struct NotedFileState : FileState {
	std::shared_ptr<const textlayout::Notes> notes;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<NotedFileState>(*this); }
	size_t footprint() const override { return sizeof(NotedFileState); }
};

// The notes a document's file state holds; null for none (a file made in the editor: its writer's own form).
inline const textlayout::Notes *noted_layout(const FileState *state) {
	const auto *noted = dynamic_cast<const NotedFileState *>(state);
	return noted && noted->notes ? noted->notes.get() : nullptr;
}

// The 1-based line of a byte offset in a text (CR LF and LF each end a line, as an editor counts them).
inline size_t line_of_offset(const std::string &text, size_t offset) {
	size_t line = 1;
	for (size_t i = 0; i < offset && i < text.size(); ++i)
		if (text[i] == '\n') ++line;
	return line;
}

} // namespace opennova::editor
