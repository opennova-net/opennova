#pragma once
// Shared plumbing for the import tests (import_plan_test.cpp, import_apply_test.cpp): a
// process seam that runs nothing, menu text built from its parts, an archive of named
// texts, what a folder holds (to say nothing was written), a plan's rows looked up, the
// findings asked about, what an import of a plan takes, and a new project of its own with
// the plan over its view.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/import/import_plan.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>
#include <formats/pff/pff.h>

#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

namespace import_test {

using namespace opennova::editor;

using editor_test::NoProcess;

// A menu window with a POSITION (a WINDOW with no child element is never created) and
// `body`; a screen; a window's FONT; an IMAGE appearance; a SCREEN action loading `file`.
inline std::string window(const char *type, const char *name, const std::string &body) {
	return std::string("<WINDOW TYPE=\"") + type + "\" NAME=\"" + name + "\">\r\n" +
	       "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>\r\n" + body + "</WINDOW>\r\n";
}
inline std::string screen(const char *name, const std::string &body) {
	return std::string("<SCREEN>\r\n<NAME>") + name + "</NAME>\r\n" + body + "</SCREEN>\r\n";
}
inline std::string font(const std::string &name) { return "<FONT><NAME>" + name + "</NAME></FONT>\r\n"; }
inline std::string image(const std::string &texture) {
	return "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">" + texture + "</APPEARANCE>\r\n";
}
inline std::string go_to(const std::string &file, const char *target) {
	return "<ACTION TYPE=\"SCREEN\" FILE=\"" + file + "\">" + target + "</ACTION>\r\n";
}

// An archive of the named texts (the boot table's resource.pff makes a game install).
inline bool write_pff(const std::string &path, const std::vector<std::pair<std::string, std::string>> &files) {
	std::vector<opennova::pff::PffWriteEntry> entries;
	for (const auto &file : files)
		entries.push_back({file.first.c_str(), reinterpret_cast<const uint8_t *>(file.second.data()),
		                   uint32_t(file.second.size()), 0, 0, 0});
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	return opennova::pff::pff_write_archive(path.c_str(), opennova::pff::PFF_FORMAT_PFF3, entries.data(),
	                                        uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK;
}

// Every file under `root` with its size and last write: what "nothing written" compares.
inline std::map<std::string, std::pair<uintmax_t, std::filesystem::file_time_type>> snapshot(const std::filesystem::path &root) {
	std::map<std::string, std::pair<uintmax_t, std::filesystem::file_time_type>> files;
	std::error_code ec;
	for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		if (it->is_regular_file(ec)) files[it->path().generic_string()] = {it->file_size(ec), it->last_write_time(ec)};
	return files;
}

inline const ImportPlanRow *row_named(const ImportPlan &plan, const std::string &name) {
	for (const ImportPlanRow &row : plan.rows)
		if (row.name == name) return &row;
	return nullptr;
}

inline const ImportNotFollowed *not_followed(const ImportPlan &plan, ReferenceKind reference, AssetKind kind = AssetKind::Unknown) {
	for (const ImportNotFollowed &entry : plan.not_followed)
		if (entry.reference == reference && entry.kind == kind) return &entry;
	return nullptr;
}

inline bool has_code(const std::vector<Diagnostic> &diagnostics, const char *code) {
	for (const Diagnostic &d : diagnostics)
		if (d.code == code) return true;
	return false;
}

inline bool has_error(const std::vector<Diagnostic> &diagnostics) {
	for (const Diagnostic &d : diagnostics)
		if (d.severity == DiagnosticSeverity::Error) return true;
	return false;
}

// What an import of the plan takes: each selected row's source, once (a converter's outputs
// share theirs).
inline std::vector<ImportSource> selected_sources(const ImportPlan &plan) {
	std::vector<ImportSource> sources;
	for (const ImportPlanRow &row : plan.rows) {
		if (!row.selected) continue;
		bool known = false;
		for (const ImportSource &source : sources)
			known = known || (source.path == row.source.path && source.entry == row.source.entry);
		if (!known) sources.push_back(row.source);
	}
	return sources;
}

// A new project of its own; the plan over its view.
struct Project {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	explicit Project(const char *name) : dir(name), session(platform, preferences) {
		session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Plan"));
	}
	const SessionView &view() const { return session.view(); }
	std::string root() const { return session.view().project.root; }
	ImportPlan plan(const std::vector<ImportSource> &sources, bool with_dependencies = true,
	                const std::string &retail = std::string(), size_t cap = kImportPlanFileCap) const {
		const SessionView &v = session.view();
		return plan_import(sources, with_dependencies, ProjectPaths::for_root(v.project.root), *v.project.document, *v.project.scan, *v.findings.graph,
		                   retail, cap);
	}
};

} // namespace import_test
