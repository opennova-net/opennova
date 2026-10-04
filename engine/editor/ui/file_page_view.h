#pragma once

#include <string>

namespace opennova::editor {

class Workspace;

// The page of a file the editor has no editor for (the plain-words lane, the audit's 4.6), in its tab of
// the Document window: the file's name, kind and size, what it holds and what in the game reads it, where
// a build puts it, what the editor does with it, its Problems rows, Show in Files and Show in folder, then
// who names it and what it names (session/file_page.h's model), each line going where it leads.
void draw_file_page(Workspace &workspace, const std::string &path);

} // namespace opennova::editor
