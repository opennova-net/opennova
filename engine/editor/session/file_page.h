#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/graph/reference_queries.h>

namespace opennova::editor {

struct SessionView;

// The page of a file the editor has no editor for (ADR 0046, the UX round's plain-words lane; the
// audit's 4.6: a double-click on a texture, a sound bank or charattr.def did nothing and said nothing):
// what the file is and what in the game reads it (assets/asset_kind_words), where a build puts it, what it
// defines with who names each (DI-17), who names it and what it names, in words, each line somewhere to
// go. A Go to whose target the editor does not edit lands here (DI-17): the line of the record it names is
// marked. The Document window's page and the wire's file_page query read it; made when asked (a page is
// one file).
struct FilePageLine {
	std::string text;       // "ITEMS.DEF: Drivable Dune Buggy - Shadow texture"
	bool missing = false;   // a name it makes that resolves to nothing
	std::string name;       // that name, as the file writes it
	bool at = false;        // the record a Go to landed on (the page's locator and field)
	// Where a click goes: the file opened at the record, or its page; a missing symbol's, the file where it
	// belongs (missing_target); a missing file's none (Problems shows its finding, with its fixes).
	ReferenceTarget target;
};

// A name the file defines (graph/reference_queries' file_definitions): its words, whether a lookup of the
// game finds it, and the uses that reach it, each a line somewhere to go.
struct FilePageDefinition {
	std::string text;     // "Particle effect Effect_AmHitDirt"
	bool read = true;     // a lookup of the game finds it (false: inert)
	std::string unread;   // why none does, in a few words
	bool at = false;      // the definition a Go to landed on
	std::vector<FilePageLine> users;
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
	bool wave = false;               // a wave: the page plays it (play_sound)
	// Where a Go to landed on it: the record (a ReferenceTarget's locator) and its field; both "" none.
	std::string at_locator, at_field;
	std::vector<FilePageDefinition> defines; // what it defines that others name
	std::vector<FilePageLine> used_by;       // the records naming it, then those naming what it defines
	std::vector<FilePageLine> names;         // what it names, each with whether the project has it
	size_t errors = 0, warnings = 0;         // its Problems rows (the modder's, the game's own data's apart)
};

// The page of the file at `path`, with the line of the record `locator` and `field` name marked (a
// ReferenceTarget's: a record of the file by its locator, else its path as the graph names it; both ""
// none).
FilePage file_page(const SessionView &view, const std::string &path, const std::string &locator = std::string(),
                   const std::string &field = std::string());
// The same, with the page's own marked record where `path` is the page the Document window shows.
FilePage shown_file_page(const SessionView &view, const std::string &path);

} // namespace opennova::editor
