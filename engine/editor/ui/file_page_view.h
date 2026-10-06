#pragma once

#include <string>

#include <editor/session/file_page.h>
#include <editor/session/view/view_revisions.h>

namespace opennova::editor {

class Workspace;

// A file page as last made, for the file and the record a Go to marked on it, kept while what it reads
// stands (the graph, the files, the findings, the project): made again only when one of them moves, never
// every frame.
struct FilePageCache {
	std::string path, locator, field;
	RevisionKey key;
	bool made = false;
	FilePage page;
};

// The page of a file the editor has no editor for (the plain-words lane, the audit's 4.6), in its tab of
// the Document window: the file's name, kind and size, what it holds and what in the game reads it, where
// a build puts it, what the editor does with it, its Problems rows, Show in Files and Show in folder (a
// wave's Play and Stop, DI-17), then what it defines with who names each, who names it and what it names
// (session/file_page.h's model), each line going where it leads. A Go to lands here (DI-17): the line of
// the record it names is marked, and with `reveal` (the ask that showed the page) scrolled to.
void draw_file_page(Workspace &workspace, const std::string &path, FilePageCache &cache, bool reveal = false);

} // namespace opennova::editor
