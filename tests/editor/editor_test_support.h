#pragma once
// Shared plumbing for the editor core tests: a throwaway project directory under the
// system temp directory (std::filesystem::temp_directory_path, so no env read of our
// own) that is wiped on construction and destruction, a file writer, every missing
// required file of a session's project created, the project settings applied, a request and the
// operation it starts run to their end, and an operation that holds the documents.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <editor/requirements/requirements.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>

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
				event->path == view.documents.active && event->address == view.documents.selection;
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
// project does not meet (a CreateMissing naming none makes nothing).
inline void create_missing_files(opennova::editor::ProjectSession &session) {
	session.handle(opennova::editor::request::create_missing(
	        opennova::editor::unmet_required_roles(*session.view().project.requirements)));
}

// The project settings dialog's Apply with only the settings `change` names, the others as
// they are (what came of it is the view's settings_result, and its SettingsApplied event); and
// two of them alone: the game install folder, the missions feature.
inline void apply_settings(opennova::editor::ProjectSession &session, const opennova::editor::ProjectSettingsChange &change) {
	session.handle(opennova::editor::request::apply_project_settings(change));
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

struct TempProjectDir {
	std::filesystem::path path;

	explicit TempProjectDir(const char *name) {
		path = std::filesystem::temp_directory_path() / name;
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
	}
	~TempProjectDir() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
	std::string root() const { return path.generic_string(); }
	std::string file(const char *relative) const {
		return (path / relative).generic_string();
	}
};

inline bool write_bytes(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::ofstream out(path, std::ios::binary);
	if (!out) return false;
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

inline bool write_text(const std::string &path, const std::string &text) {
	return write_bytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

} // namespace editor_test
