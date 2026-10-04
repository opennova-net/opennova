#pragma once

#include <string>

#include <editor/session/file_page.h>
#include <editor/session/view/view_revisions.h>

namespace opennova::editor {

class Workspace;

// A file page as last made, for the file it was made for, kept while what it reads stands (the graph, the
// files, the findings, the project): made again only when one of them moves, never every frame.
struct FilePageCache {
	std::string path;
	RevisionKey key;
	bool made = false;
	FilePage page;
};

// The page of a file the editor has no editor for (the plain-words lane, the audit's 4.6), in its tab of
// the Document window: the file's name, kind and size, what it holds and what in the game reads it, where
// a build puts it, what the editor does with it, its Problems rows, Show in Files and Show in folder, then
// who names it and what it names (session/file_page.h's model), each line going where it leads.
void draw_file_page(Workspace &workspace, const std::string &path, FilePageCache &cache);

} // namespace opennova::editor
