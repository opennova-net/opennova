#pragma once
// Shared plumbing for the editor core tests: a throwaway project directory under the
// system temp directory (std::filesystem::temp_directory_path, so no env read of our
// own) that is wiped on construction and destruction (through the system's paths, so a tree past
// MAX_PATH goes too), a file writer, a tree's digest, every missing
// required file of a session's project created, the project settings applied, a request and the
// operation it starts run to their end, an operation that holds the documents, and a finding made
// by its code's token with what it is about.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <base/io/hash.h>
#include <base/io/os_path.h>
#include <editor/assets/asset_registry.h>
#include <editor/project/project_files.h>
#include <editor/requirements/requirements.h>
#include <editor/session/finding_codes.h>
#include <editor/session/original_files.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>
#include <formats/lwf/lwf.h>

namespace editor_test {

// A shared member of a hand-made view (ProjectView's document, scan, requirements and imports,
// ActivityView's last build, an import preview's plan, a rename preview's sites: each read only
// through the view), made the test's own to change: a copy of what it holds, which the view holds
// from then on.
template <typename T> T &own(std::shared_ptr<const T> &held) {
	std::shared_ptr<T> copy = std::make_shared<T>(*held);
	T &out = *copy;
	held = std::move(copy);
	return out;
}

// The field the newest RevealRecord event asks to show, while it is about the selection in the
// active document ("" otherwise): what a Go to or a Problems row lights in the Inspector.
inline std::string revealed_field(const opennova::editor::SessionView &view) {
	const auto &held = view.events.held();
	for (auto event = held.rbegin(); event != held.rend(); ++event) {
		if (event->kind != opennova::editor::ViewEventKind::RevealRecord) continue;
		const bool selected =
				event->path == view.documents.active && event->address == view.documents.selection.primary;
		return selected ? event->field : std::string();
	}
	return std::string();
}

// The events of `kind` a view posted after seq `after` (view_events.h), oldest first.
inline std::vector<opennova::editor::ViewEvent> events_after(
		const opennova::editor::SessionView &view, uint64_t after,
		opennova::editor::ViewEventKind kind) {
	std::vector<opennova::editor::ViewEvent> out;
	for (const opennova::editor::ViewEvent &event : view.events.held())
		if (event.seq > after && event.kind == kind) out.push_back(event);
	return out;
}

// A request handled and what it came to with the operation it started (or joined) run to its end
// (S13 A3: an Open, a Rescan, a Reimport, an import's plan and its write, a rename's commit, a
// build): the request's outcome with the operation's findings after its own, done only when the
// request was and the operation ended done (a rename's commit, an import's write or a Reimport's
// pass may still refuse or fail once the request that started it is done).
inline opennova::editor::ActionOutcome handle_to_end(opennova::editor::ProjectSession &session,
		const opennova::editor::EditorRequest &request) {
	session.handle(request);
	opennova::editor::ActionOutcome outcome = session.outcome();
	session.run_operations();
	const opennova::editor::OperationOutcome &ended = session.view().activity.last_operation;
	if (outcome.operation != 0 && ended.id == outcome.operation) {
		outcome.findings.insert(outcome.findings.end(), ended.findings.begin(), ended.findings.end());
		if (ended.end != opennova::editor::OperationEnd::Done) outcome.refused = true;
	}
	return outcome;
}

// Create all missing, as the editor asks for it: the roles of every Required row the
// project does not meet (a CreateMissing naming none makes nothing); then the validation it
// left due, run to its end (S13 A3: the polls step it, and no request runs it).
inline void create_missing_files(opennova::editor::ProjectSession &session) {
	session.handle(opennova::editor::request::create_missing(
	        opennova::editor::unmet_required_roles(*session.view().project.requirements)));
	session.run_operations();
}

// The project settings dialog's Apply with only the settings `change` names, the others as
// they are (what came of it is the view's settings_result, and its SettingsApplied event), the
// validation it left due run to its end; and two of them alone: the game install folder, the
// missions feature.
inline void apply_settings(opennova::editor::ProjectSession &session, const opennova::editor::ProjectSettingsChange &change) {
	session.handle(opennova::editor::request::apply_project_settings(change));
	session.run_operations();
}
inline void set_game_install(opennova::editor::ProjectSession &session, const std::string &dir) {
	opennova::editor::ProjectSettingsChange change;
	change.game_install = dir;
	apply_settings(session, change);
}
inline void set_missions(opennova::editor::ProjectSession &session, bool on) {
	opennova::editor::ProjectSettingsChange change;
	change.mission = on;
	apply_settings(session, change);
}

// An operation that writes the project's files and open documents and cannot be cancelled (a
// test's: what S13 A3's rename and import operations will be), done on its first step: while it
// runs the busy gate refuses every edit, and ProjectSession::run_operations() ends it.
struct HoldingOperation : opennova::editor::SessionOperation {
	opennova::editor::OperationKind kind() const override {
		return opennova::editor::OperationKind::RenameApply;
	}
	bool step(const opennova::editor::StepBudget &) override { return true; }
	opennova::editor::OperationProgress progress() const override {
		return {0, 1, opennova::editor::OperationUnit::Steps, "Holding the documents"};
	}
	bool cancellable() const override { return false; }
	void cancel() override {}
	opennova::editor::OperationOutcome finish(opennova::editor::SessionCore &) override { return {}; }
};

// The process's own suffix to a temporary directory's name: two runs of a test at once (a parallel
// ctest beside another worktree's, an agent's beside a person's) share the system temp directory,
// and each wipes its directory on construction and destruction, so a name both used would have one
// run delete the other's project mid-test.
inline std::string process_suffix() {
#ifdef _WIN32
	return "_" + std::to_string(_getpid());
#else
	return "_" + std::to_string(getpid());
#endif
}

// `name` and every path it gives are UTF-8 (the editor's paths), turned into the system's own
// through base/io/os_path.h, not the editor's conversions under test.
struct TempProjectDir {
	std::filesystem::path path;

	explicit TempProjectDir(const char *name) {
		path = std::filesystem::temp_directory_path() / opennova::io::os_path(std::string(name) + process_suffix());
		std::error_code ec;
		std::filesystem::remove_all(opennova::editor::system_path(root()), ec);
		std::filesystem::create_directories(path, ec);
	}
	~TempProjectDir() {
		std::error_code ec;
		std::filesystem::remove_all(opennova::editor::system_path(root()), ec);
	}
	std::string root() const { return opennova::io::utf8_generic_path(path); }
	std::string file(const char *relative) const { return opennova::io::utf8_join(root(), relative); }
};

// A file written at the UTF-8 `path`, its folders made.
inline bool write_bytes(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::error_code ec;
	std::filesystem::create_directories(opennova::io::os_path(path).parent_path(), ec);
	std::ofstream out(opennova::io::os_path(path), std::ios::binary);
	if (!out) return false;
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

// A sound bank minted through the engine's writer (formats/lwf) holding a set of each name, each with one
// layer heard in both views and no member: what a project's sound names resolve against (the sound
// lane), a test's game.lwf.
inline std::vector<uint8_t> sound_bank_of(const std::vector<std::string> &sets) {
	opennova::lwf::File bank;
	for (const std::string &name : sets) {
		opennova::lwf::Multi set;
		set.name = name;
		set.pitch_base = opennova::lwf::kAuthoredSetPitchBase;
		set.target_id = 10000;
		set.playlist_indices.push_back(uint32_t(bank.playlists.size()));
		opennova::lwf::Playlist layer;
		layer.flags = opennova::lwf::kFlagInternal | opennova::lwf::kFlagExternal;
		bank.playlists.push_back(layer);
		bank.multis.push_back(set);
	}
	std::vector<uint8_t> out;
	std::string error;
	opennova::lwf::encode_lwf(bank, out, error);
	return out;
}

// `text` with every line ending in CR LF, the last one included: the one break the retail walk of a
// .def, .trn or .env file splits at, which drops the last byte of a line left without it
// [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5; engine/base/io/ascii_config.h]. A test writes such
// a file's text with LF and stages it through this (the GUT suite's TestFs.crlf).
inline std::string crlf(const std::string &text) {
	std::string lf;
	for (size_t i = 0; i < text.size(); ++i)
		if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')) lf += text[i];
	if (!lf.empty() && lf.back() != '\n') lf += '\n';
	std::string out;
	for (const char c : lf) {
		if (c == '\n') out += '\r';
		out += c;
	}
	return out;
}

// A text as a file named `name` holds it: a .def, .trn or .env with CR LF line ends (crlf above).
inline std::string staged(const std::string &name, const std::string &text) {
	std::string extension = name.size() >= 4 ? name.substr(name.size() - 4) : std::string();
	for (char &c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
	return extension == ".def" || extension == ".trn" || extension == ".env" ? crlf(text) : text;
}

// A text file written at `path` (staged above).
inline bool write_text(const std::string &path, const std::string &text) {
	const std::string bytes = staged(path, text);
	return write_bytes(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

// A test device's ground (the height at a point of the viewport's space, z up): where the segment
// from `from` to `to` first meets it, as ViewportDevice::surface_between answers. False where the
// segment begins under it or never reaches it. The first of 2,048 steps at or under the ground, then
// the crossing between it and the step before, halved until it stands.
template <typename Ground>
bool ground_crossing(const Ground &ground, const double from[3], const double to[3], double point[3]) {
	const auto at = [&](double t, double out[3]) {
		for (int i = 0; i < 3; ++i) out[i] = from[i] + (to[i] - from[i]) * t;
		return out[2] - ground(out[0], out[1]);
	};
	double p[3];
	if (at(0.0, p) < 0.0) return false;
	const int steps = 2048;
	for (int i = 1; i <= steps; ++i) {
		if (at(double(i) / steps, p) > 0.0) continue;
		double low = double(i - 1) / steps, high = double(i) / steps;
		for (int pass = 0; pass < 48; ++pass) {
			const double middle = (low + high) * 0.5;
			(at(middle, p) > 0.0 ? low : high) = middle;
		}
		at(high, point);
		return true;
	}
	return false;
}

// The file's last write set `age` before now. A file a test writes is younger than the hash caches'
// settle window (io::kFileStampSettle), so each pass would read it again (S13 A8); one back-dated
// is a file written a while ago, which a cache keeps by its size and last write.
inline bool backdate(const std::string &path, std::chrono::seconds age) {
	std::error_code ec;
	std::filesystem::last_write_time(opennova::editor::system_path(path),
	                                 std::filesystem::file_time_type::clock::now() - age, ec);
	return !ec;
}

// Every file under `dir` (its dot-folders' too) back-dated by `age`; false when one is not.
inline bool backdate_tree(const std::string &dir, std::chrono::seconds age) {
	namespace fs = std::filesystem;
	std::error_code ec;
	bool ok = true;
	const fs::path root = opennova::editor::system_path(dir);
	for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
	     it.increment(ec)) {
		std::error_code kind;
		if (!it->is_regular_file(kind)) continue;
		std::error_code dated;
		fs::last_write_time(it->path(), fs::file_time_type::clock::now() - age, dated);
		ok = ok && !dated;
	}
	return ok && !ec;
}

// What a directory tree holds, as one text: every entry's path under `dir` in their order, and
// each file's size and FNV-1a of its bytes (S13 A8: a build directory compared before and after a
// Play). Read through the system's paths, so a tree past MAX_PATH reads as well; "" when `dir` is
// no directory.
inline std::string tree_digest(const std::string &dir) {
	namespace fs = std::filesystem;
	const fs::path root = opennova::editor::system_path(dir);
	std::error_code ec;
	if (!fs::is_directory(root, ec)) return std::string();
	std::vector<std::pair<std::string, fs::path>> entries;
	for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
	     it.increment(ec))
		entries.emplace_back(opennova::io::utf8_generic_path(it->path().lexically_relative(root)), it->path());
	std::sort(entries.begin(), entries.end());
	std::string digest;
	for (const auto &[relative, path] : entries) {
		digest += relative;
		std::error_code kind;
		if (fs::is_regular_file(path, kind)) {
			std::vector<uint8_t> bytes;
			std::string error;
			opennova::io::read_file_bytes(opennova::io::utf8_path(path), bytes, error);
			digest += " " + std::to_string(bytes.size()) + " " +
			          opennova::io::hex64(opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, bytes.data(), bytes.size()));
		}
		digest += "\n";
	}
	return digest;
}

// A finding of a code a table declares, by its token (session/finding_codes.h: finding_row), as a
// producer makes it; a token no table declares stops the test, since nothing can make one.
inline opennova::editor::Diagnostic finding_of(opennova::editor::DiagnosticSeverity severity,
		const std::string &code, std::string message, std::string asset = std::string(),
		std::string field = std::string()) {
	const opennova::editor::FindingCodeRow *row = opennova::editor::finding_row(code);
	if (!row) {
		std::fprintf(stderr, "no finding code row declares %s\n", code.c_str());
		std::abort();
	}
	return opennova::editor::make_finding(*row, severity, std::move(message), std::move(asset),
			std::move(field));
}

// What the game install makes of the files at `paths`, as its validation keys them (OriginalData, the game's
// own data's baseline, ready): every finding of `rows` about one of them, by the file's logical name.
inline opennova::editor::OriginalData originals_of(const std::vector<opennova::editor::Diagnostic> &rows,
		const std::vector<std::string> &paths) {
	opennova::editor::OriginalData data;
	data.ready = true;
	for (const opennova::editor::Diagnostic &d : rows)
		if (std::find(paths.begin(), paths.end(), d.asset) != paths.end())
			++data.findings[opennova::pff::normalized_logical_name(opennova::editor::basename_of(d.asset))]
			               [opennova::editor::original_finding_key(d)];
	return data;
}
// A finding's subject of the kind a test reads: a finding about anything else stops the test (a
// check of a member it does not have would pass for nothing).
[[noreturn]] inline void wrong_subject(const opennova::editor::Diagnostic &d, const char *kind) {
	std::fprintf(stderr, "%s (%s) is not about a %s\n", d.code().c_str(), d.message.c_str(), kind);
	std::abort();
}
inline const opennova::editor::ReferenceSubject &reference_of(const opennova::editor::Diagnostic &d) {
	const opennova::editor::ReferenceSubject *subject = opennova::editor::reference_subject(d);
	if (!subject) wrong_subject(d, "reference");
	return *subject;
}
inline const opennova::editor::RequirementSubject &requirement_of(const opennova::editor::Diagnostic &d) {
	const opennova::editor::RequirementSubject *subject = opennova::editor::requirement_subject(d);
	if (!subject) wrong_subject(d, "required file");
	return *subject;
}
// A hand-made finding's subject of that kind, the test's to set: made when the finding is about
// nothing more yet; a finding about the other kind stops the test.
inline opennova::editor::ReferenceSubject &own_reference(opennova::editor::Diagnostic &d) {
	if (std::holds_alternative<std::monostate>(d.subject)) d.subject = opennova::editor::ReferenceSubject();
	if (!std::holds_alternative<opennova::editor::ReferenceSubject>(d.subject)) wrong_subject(d, "reference");
	return std::get<opennova::editor::ReferenceSubject>(d.subject);
}
inline opennova::editor::RequirementSubject &own_requirement(opennova::editor::Diagnostic &d) {
	if (std::holds_alternative<std::monostate>(d.subject)) d.subject = opennova::editor::RequirementSubject();
	if (!std::holds_alternative<opennova::editor::RequirementSubject>(d.subject))
		wrong_subject(d, "required file");
	return std::get<opennova::editor::RequirementSubject>(d.subject);
}

} // namespace editor_test
