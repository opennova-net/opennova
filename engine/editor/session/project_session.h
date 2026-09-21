#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_session.h>
#include <editor/run/play_session.h>
#include <editor/run/process_platform.h>
#include <editor/session/editor_request.h>
#include <editor/session/editor_settings.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

// How Play starts the game: the packaged runtime beside the editor, or a source run
// that drives the Godot binary at the checkout. The shell fills it (it knows where it
// runs from and allocates the MCP port); the editor settings can override the runtime.
struct PlayLauncher {
	bool source_run = false;
	std::string executable;          // opennova.exe, or the Godot binary for a source run
	std::string godot_project_dir;   // the checkout's godot/ directory (source runs only)
	std::vector<std::string> engine_args; // Godot options for the child, before the game flags
	int mcp_port = 0;                // 0 = no endpoint
};

// The one open project and everything the editor does to it (ADR 0046 d10): open and
// create, scan and evaluate, create-missing, build, play. Portable: the shell hands it
// the process seam and the settings path and drains its view; a test drives it the
// same way. Requests come in typed (EditorRequest), the view goes out (SessionView).
// The build advances one step per poll so the window that hosts the session keeps
// drawing while a project packs.
class ProjectSession {
public:
	ProjectSession(ProcessPlatform &platform, std::string editor_settings_path);
	~ProjectSession();

	const SessionView &view() const { return view_; }
	const PlayLauncher &launcher() const { return launcher_; }
	void set_launcher(PlayLauncher launcher);

	// True when the request was served here; false for the shell-only kinds.
	bool handle(const EditorRequest &request);
	// Once per frame: one build step, the finished build (and the Play waiting on it),
	// the child's state, the game's log tail.
	void poll();
	// Run a build in progress to its end (a test, a command line).
	void finish_build();

	EditableDocument *document_for(const std::string &path = {});
	bool documents_dirty() const;
	bool project_open() const { return view_.project_open; }
	bool build_running() const { return build_ != nullptr; }
	// The directory the running game uses ("" when none): the build never prunes it.
	std::string running_build_dir() const { return play_.running_build_dir(); }

private:
	bool handle_document(const EditorRequest &request);
	bool guard_unsaved(const EditorRequest &request);
	bool save_documents(bool all);
	void update_document_view();
	void validate_documents();
	bool new_project(const std::string &dir, const std::string &title);
	bool open_project(const std::string &dir);
	void close_project();
	void refresh();
	void save_document();
	void create_missing(const std::string &role);
	void start_build(bool then_play);
	void absorb_build();
	void start_play();
	void stop_play();
	std::string resolve_runtime_executable() const;
	void note(std::string line);
	void report(const Diagnostic &d);
	void tail_game_log();
	void save_editor_settings();
	void touch() { ++view_.revision; }

	ProcessPlatform &platform_;
	std::string settings_path_;
	EditorSettings settings_;
	ProjectPaths paths_;
	LocalSettings local_;
	PlaySession play_;
	PlayLauncher launcher_;
	std::unique_ptr<BuildRun> build_;
	bool play_after_build_ = false;
	std::string game_log_file_;
	uint64_t game_log_offset_ = 0;
	std::string game_log_partial_;
	std::vector<std::shared_ptr<EditableDocument>> documents_;
	std::optional<EditorRequest> pending_request_;
	SessionView view_;
};

} // namespace opennova::editor
