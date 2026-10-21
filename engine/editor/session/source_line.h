#pragma once

#include <cstddef>
#include <string>

#include <editor/assets/asset_kind.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class SessionCore;

// A line of a file's source as the finding that names it counts lines (Problems' Correct the line..., the
// correct_line request): a kind whose game reader ends a line at CR LF alone (base/resource_index/file_kind.h's
// LineReader: the defs, the AI profiles, the animation maps, the environments, the terrains...) by CR LF pairs,
// an LF alone a byte of its line, as the game's walk cuts them [orig: File_ParseASCIIFile @ 0x53D8C7..0x53D8F5];
// any other kind by LF, a CR before it part of the ending. A last line no ending ends is a line too. `begin` and
// `end` bound the line's text, `ending` its ending's length (0 for none).
struct SourceLine {
	size_t begin = 0;
	size_t end = 0;
	size_t ending = 0;
};

// Line `line` (1-based) of `bytes`, the ending rule `crlf_only` (source_line_crlf_only); false past the last, the
// file's count of lines in `lines`.
bool find_source_line(const std::string &bytes, size_t line, bool crlf_only, SourceLine &out, size_t &lines);
// Whether a file of `kind` ends a line at CR LF alone.
bool source_line_crlf_only(AssetKind kind);
// `bytes` with the line `at` holding `text`, its ending kept and every other byte as it was; an empty `text` takes
// the line out, its ending with it.
std::string with_source_line(const std::string &bytes, const SourceLine &at, const std::string &text);

// The text of line `line` of the project file `relative` (of `kind`) under the project's `root` as it stands on disk,
// as UTF-8: the game's text files are the code page's bytes (cp1252, as the def tables read them), which the dialog
// and the wire take as UTF-8; false, why in `error`.
bool read_source_line(const std::string &root, const std::string &relative, AssetKind kind, size_t line,
                      std::string &text, std::string &error);
// The line a correct_line names (values.line: digits alone, from 1); 0 for none.
size_t correct_line_number(const EditorRequest &request);

// CorrectLine (session/request_kinds.h): the line written in the file on disk (or taken out), the rest of its bytes
// untouched, the file read again (an open document of it with its records and its validation, as an outside
// change is). The text and `was` are UTF-8, the line written as its code-page bytes (refused for a character the
// page has no byte for). With ask_name, Problems' Correct the line... opened on the line instead
// (workspace_follows), refused for a line longer than the dialog holds.
void correct_source_line(SessionCore &core, const EditorRequest &request);

} // namespace opennova::editor
