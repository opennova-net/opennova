#pragma once
// Shared plumbing for the editor core tests: a throwaway project directory under the
// system temp directory (std::filesystem::temp_directory_path, so no env read of our
// own) that is wiped on construction and destruction, a file writer, every missing
// required file of a session's project created, the project settings applied, and an operation
// that holds the documents.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <editor/requirements/requirements.h>
#include <editor/session/project_session.h>
#include <editor/session/session_operation.h>
#include <editor/session/session_view.h>

namespace editor_test {

// Create all missing, as the editor asks for it: the roles of every Required row the
// project does not meet (a CreateMissing naming none makes nothing).
inline void create_missing_files(opennova::editor::ProjectSession &session) {
	opennova::editor::EditorRequest request = opennova::editor::make_request(opennova::editor::EditorRequestKind::CreateMissing);
	request.names = opennova::editor::unmet_required_roles(session.view().requirements);
	session.handle(request);
}

// The project settings dialog's Apply with only the settings `change` names, the others as
// they are (what came of it is the view's settings_result); and two of them alone: the
// game install folder, the missions feature.
inline void apply_settings(opennova::editor::ProjectSession &session, const opennova::editor::ProjectSettingsChange &change) {
	opennova::editor::EditorRequest request = opennova::editor::make_request(opennova::editor::EditorRequestKind::ApplyProjectSettings);
	request.settings = change;
	session.handle(request);
}
inline void set_retail_directory(opennova::editor::ProjectSession &session, const std::string &dir) {
	opennova::editor::ProjectSettingsChange change;
	change.retail_directory = dir;
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
