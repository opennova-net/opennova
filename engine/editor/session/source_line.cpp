#include <editor/session/source_line.h>

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/cp1252.h>
#include <base/io/file_io.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/core_finding.h>
#include <editor/model/document_base.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

bool source_line_crlf_only(AssetKind kind) {
	return file_kind_facts(kind).line_reader != LineReader::None;
}

bool find_source_line(const std::string &bytes, size_t line, bool crlf_only, SourceLine &out, size_t &lines) {
	lines = 0;
	size_t at = 0;
	while (at < bytes.size()) {
		size_t end = at, ending = 0;
		for (; end < bytes.size(); ++end) {
			if (bytes[end] == '\r' && end + 1 < bytes.size() && bytes[end + 1] == '\n') {
				ending = 2;
				break;
			}
			if (!crlf_only && bytes[end] == '\n') {
				ending = 1;
				break;
			}
		}
		if (++lines == line) {
			out = SourceLine{at, end, ending};
			return true;
		}
		at = end + ending;
	}
	return false;
}

std::string with_source_line(const std::string &bytes, const SourceLine &at, const std::string &text) {
	if (text.empty()) return bytes.substr(0, at.begin) + bytes.substr(at.end + at.ending);
	return bytes.substr(0, at.begin) + text + bytes.substr(at.end);
}

bool read_source_line(const std::string &root, const std::string &relative, AssetKind kind, size_t line,
                      std::string &text, std::string &error) {
	std::vector<uint8_t> read;
	if (!io::read_file_bytes(join_path(root, relative), read, error)) return false;
	const std::string bytes(read.begin(), read.end());
	SourceLine at;
	size_t lines = 0;
	if (!find_source_line(bytes, line, source_line_crlf_only(kind), at, lines)) {
		error = relative + " has " + std::to_string(lines) + (lines == 1 ? " line" : " lines") + ", not " +
		        std::to_string(line) + ".";
		return false;
	}
	text = cp1252_to_utf8(bytes.substr(at.begin, at.end - at.begin));
	return true;
}

namespace {

const std::string *value_of(const EditorRequest &request, const char *key) {
	for (const auto &[name, value] : request.values)
		if (name == key) return &value;
	return nullptr;
}

// A line's number as values.line gives it: digits alone, from 1; 0 for anything else.
size_t line_number(const std::string *text) {
	if (!text || text->empty() || text->size() > 9) return 0;
	size_t number = 0;
	for (const char c : *text) {
		if (c < '0' || c > '9') return 0;
		number = number * 10 + size_t(c - '0');
	}
	return number;
}

} // namespace

size_t correct_line_number(const EditorRequest &request) { return line_number(value_of(request, "line")); }

void correct_source_line(SessionCore &core, const EditorRequest &request) {
	const SessionView &view = core.view();
	if (!view.project.open || !view.project.scan) return;
	const AssetEntry *file = view.project.scan->named(request.path);
	const auto refuse = [&](const std::string &message) {
		core.refuse_now(CoreFinding::SourceLine, message, file ? file->relative_path : request.path);
	};
	if (!file) return refuse("The project has no file " + request.path + ".");
	for (const auto &[name, value] : request.values)
		if (name != "line" && name != "text" && name != "was")
			return refuse("A correct_line takes no value \"" + name + "\" (it takes line, text and was).");
	const size_t line = line_number(value_of(request, "line"));
	if (!line) return refuse("Name the line to correct as a number from 1 (values.line).");
	// Problems' Correct the line... opened on the line, its text as the file holds it (workspace_follows): refused
	// here where the file has no such line.
	if (request.ask_name) {
		std::string text, error;
		if (!read_source_line(core.paths().root, file->relative_path, file->kind, line, text, error)) return refuse(error);
		if (text.size() >= kWorkspaceLine)
			refuse("Line " + std::to_string(line) + " of " + file->relative_path + " is longer than the dialog holds (" +
			       std::to_string(kWorkspaceLine - 1) + " characters): correct it in a text editor.");
		return;
	}
	const std::string *text = value_of(request, "text");
	const std::string *was = value_of(request, "was");
	if (text && text->find_first_of("\r\n") != std::string::npos)
		return refuse("A line's text holds no line end: correct one line at a time.");
	// The game's text files are the code page's bytes: the text written as they hold it.
	std::string corrected;
	if (text && !utf8_to_cp1252(*text, corrected))
		return refuse("The game's code page (Windows-1252) has no byte for a character of the text: correct the line "
		              "without it.");
	const std::string relative = file->relative_path;
	DocumentBase *open = core.documents().document_for(relative);
	if (open && open->dirty())
		return refuse(file->logical_name + " is open with unsaved edits: save or discard them first.");
	const std::string absolute = join_path(core.paths().root, relative);
	std::vector<uint8_t> read;
	std::string error;
	if (!io::read_file_bytes(absolute, read, error)) return refuse(error);
	const std::string bytes(read.begin(), read.end());
	SourceLine at;
	size_t lines = 0;
	if (!find_source_line(bytes, line, source_line_crlf_only(file->kind), at, lines))
		return refuse(relative + " has " + std::to_string(lines) + (lines == 1 ? " line" : " lines") + ", not " +
		              std::to_string(line) + ".");
	const std::string before = bytes.substr(at.begin, at.end - at.begin);
	// The line as the correction was asked over: one another program changed since is not written over.
	if (was && *was != cp1252_to_utf8(before))
		return refuse("Line " + std::to_string(line) + " of " + relative + " changed since: it reads \"" +
		              cp1252_to_utf8(before) + "\".");
	if (before == corrected) {
		core.note("Line " + std::to_string(line) + " of " + relative + " already reads so: nothing written.");
		return;
	}
	if (!io::write_file_atomic(absolute, with_source_line(bytes, at, corrected), error)) return refuse(error);
	core.note((corrected.empty() ? "Took line " + std::to_string(line) + " out of " + relative
	                             : "Corrected line " + std::to_string(line) + " of " + relative) +
	          "; it is read again.");
	// The file read again: the scan's entry and the validation, and an open document of it as an outside
	// change is (its records, the validation of what it now holds).
	core.update_files({relative});
	core.documents().reload_changed();
}

} // namespace opennova::editor
