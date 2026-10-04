#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/graph/reference_queries.h>

namespace opennova::editor {

struct SessionView;

// The page of a file the editor has no editor for (ADR 0046, the UX round's plain-words lane; the
// audit's 4.6: a double-click on a texture, a sound bank or charattr.def did nothing and said nothing):
// what the file is and what in the game reads it (assets/asset_kind_words), where a build puts it, who
// names it and what it names, in words, each line somewhere to go. The Document window's page and the
// wire's file_page query read it; made when asked (a page is one file).
struct FilePageLine {
	std::string text;       // "ITEMS.DEF: Drivable Dune Buggy - Shadow texture"
	bool missing = false;   // a name it makes that resolves to nothing
	ReferenceTarget target; // where a click goes (an empty file: nowhere)
};

struct FilePage {
	bool found = false; // the project has the file
	std::string path;   // project-relative
	std::string name;   // its logical name
	std::string kind;   // its kind in words ("Character attributes")
	uint64_t size = 0;
	std::string what, read_by, cite; // asset_kind_words
	std::string build;               // where a build puts it, in words
	std::string editor;              // that the editor opens no document of its kind, and what it does with it
	std::vector<FilePageLine> used_by; // the records naming it, then those naming what it defines
	std::vector<FilePageLine> names;   // what it names, each with whether the project has it
	size_t errors = 0, warnings = 0;   // its Problems rows (the modder's, the game's own data's apart)
};

FilePage file_page(const SessionView &view, const std::string &path);

} // namespace opennova::editor
