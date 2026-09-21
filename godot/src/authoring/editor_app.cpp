#include "authoring/editor_app.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>
#include <vector>

#include "util/string_convert.h"

using opennova::editor::EditorRequest;
using opennova::editor::EditorRequestKind;
using opennova::editor::PickPurpose;
using opennova::editor::PlayLauncher;
using opennova::editor::PlayState;
using opennova::editor::ProjectSession;
using opennova::editor::SessionView;

namespace godot {

namespace {

constexpr const char *kSmokeFlag = "--editor-smoke";

} // namespace

void EditorApp::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_settings_path", "path"), &EditorApp::set_settings_path);
	ClassDB::bind_method(D_METHOD("get_settings_path"), &EditorApp::get_settings_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "settings_path"), "set_settings_path", "get_settings_path");
	ClassDB::bind_method(D_METHOD("set_play_engine_args", "args"), &EditorApp::set_play_engine_args);
	ClassDB::bind_method(D_METHOD("get_play_engine_args"), &EditorApp::get_play_engine_args);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "play_engine_args"), "set_play_engine_args",
			"get_play_engine_args");
	ClassDB::bind_method(D_METHOD("new_project", "dir", "title"), &EditorApp::new_project);
	ClassDB::bind_method(D_METHOD("open_project", "dir"), &EditorApp::open_project);
	ClassDB::bind_method(D_METHOD("close_project"), &EditorApp::close_project);
	ClassDB::bind_method(D_METHOD("create_missing_files"), &EditorApp::create_missing_files);
	ClassDB::bind_method(D_METHOD("build"), &EditorApp::build);
	ClassDB::bind_method(D_METHOD("play"), &EditorApp::play);
	ClassDB::bind_method(D_METHOD("stop_play"), &EditorApp::stop_play);
	ClassDB::bind_method(D_METHOD("pump"), &EditorApp::pump);
	ClassDB::bind_method(D_METHOD("is_project_open"), &EditorApp::is_project_open);
	ClassDB::bind_method(D_METHOD("get_project_title"), &EditorApp::get_project_title);
	ClassDB::bind_method(D_METHOD("get_project_root"), &EditorApp::get_project_root);
	ClassDB::bind_method(D_METHOD("get_required_missing"), &EditorApp::get_required_missing);
	ClassDB::bind_method(D_METHOD("get_required_total"), &EditorApp::get_required_total);
	ClassDB::bind_method(D_METHOD("get_last_build_dir"), &EditorApp::get_last_build_dir);
	ClassDB::bind_method(D_METHOD("is_last_build_ok"), &EditorApp::is_last_build_ok);
	ClassDB::bind_method(D_METHOD("get_play_state"), &EditorApp::get_play_state);
	ClassDB::bind_method(D_METHOD("did_game_exit_on_its_own"), &EditorApp::did_game_exit_on_its_own);
	ClassDB::bind_method(D_METHOD("get_problem_count"), &EditorApp::get_problem_count);
	ClassDB::bind_method(D_METHOD("get_output_lines"), &EditorApp::get_output_lines);
	ClassDB::bind_method(D_METHOD("get_recent_projects"), &EditorApp::get_recent_projects);
	ClassDB::bind_method(D_METHOD("get_loaded_variant"), &EditorApp::get_loaded_variant);
	ClassDB::bind_method(D_METHOD("is_source_run"), &EditorApp::is_source_run);
	ClassDB::bind_method(D_METHOD("_on_dir_selected", "dir"), &EditorApp::_on_dir_selected);
	ClassDB::bind_method(D_METHOD("_on_file_selected", "file"), &EditorApp::_on_file_selected);
	ClassDB::bind_method(D_METHOD("_on_picker_canceled"), &EditorApp::_on_picker_canceled);
}

EditorApp::EditorApp() : platform_(std::make_unique<ChildProcessPlatform>()) {
#if OPENNOVA_EDITOR_UI
	windows_ = std::make_unique<opennova::editor::EditorWindows>();
#endif
}

EditorApp::~EditorApp() = default;

opennova::devtools::ImGuiPass *EditorApp::engine_pass() {
#if OPENNOVA_EDITOR_UI
	return &windows_->pass();
#else
	return nullptr;
#endif
}

void EditorApp::ensure_session() {
	if (session_) {
		return;
	}
	const String settings = ProjectSettings::get_singleton()->globalize_path(settings_path_);
	session_ = std::make_unique<ProjectSession>(*platform_, opennova::to_std(settings));
	session_->set_launcher(make_launcher(0));
#if OPENNOVA_EDITOR_UI
	windows_->set_view(&session_->view());
#endif
}

bool EditorApp::is_source_run() const {
	return !OS::get_singleton()->has_feature("template");
}

// Where Play finds the game: the Godot binary at the checkout for a source run, else
// the runtime packaged beside the editor (<editor dir>/../runtime/opennova.exe).
PlayLauncher EditorApp::make_launcher(int p_mcp_port) const {
	PlayLauncher launcher;
	launcher.source_run = is_source_run();
	launcher.mcp_port = p_mcp_port;
	const String executable = OS::get_singleton()->get_executable_path();
	if (launcher.source_run) {
		launcher.executable = opennova::to_std(executable);
		launcher.godot_project_dir = opennova::to_std(ProjectSettings::get_singleton()->globalize_path("res://"));
	} else {
		const String runtime = executable.get_base_dir().get_base_dir().path_join("runtime").path_join("opennova.exe");
		launcher.executable = opennova::to_std(runtime);
	}
	for (int i = 0; i < play_engine_args_.size(); ++i) {
		launcher.engine_args.push_back(opennova::to_std(play_engine_args_[i]));
	}
	return launcher;
}

void EditorApp::_ready() {
	ensure_session();
	ImGuiPassNode::_ready();
	// The session pumps whether or not the workspace draws (headless tests, the smoke).
	set_process(true);
	if (is_available()) {
		UtilityFunctions::print_verbose("OpenNova Editor: editor variant loaded, workspace attached");
	} else {
		UtilityFunctions::print_verbose("OpenNova Editor: editor variant loaded, no ImGui context (headless)");
	}
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	for (int i = 0; i < args.size(); ++i) {
		if (args[i] == kSmokeFlag) {
			UtilityFunctions::print_verbose("OpenNova Editor: smoke ok");
			get_tree()->quit(0);
			return;
		}
	}
}

void EditorApp::_exit_tree() {
	if (picker_ != nullptr) {
		picker_->queue_free();
		picker_ = nullptr;
	}
	ImGuiPassNode::_exit_tree();
	set_process(false);
}

void EditorApp::_process(double p_delta) {
	ImGuiPassNode::_process(p_delta); // the layout pass (no-op headless): the windows raise requests
	pump();
}

void EditorApp::after_layout(uint64_t, bool, int64_t) {}

void EditorApp::pump() {
	ensure_session();
	drain_requests();
	session_->poll();
}

void EditorApp::drain_requests() {
#if OPENNOVA_EDITOR_UI
	EditorRequest request;
	while (windows_->take_request(request)) {
		if (request.kind == EditorRequestKind::Play) {
			// The port is the shell's to allocate: a fresh loopback port per run.
			session_->set_launcher(make_launcher(allocate_mcp_port()));
		}
		if (!session_->handle(request)) {
			serve(request);
		}
	}
#endif
}

// The requests only an OS can serve.
void EditorApp::serve(const EditorRequest &p_request) {
	switch (p_request.kind) {
		case EditorRequestKind::PickDirectory:
			show_picker(p_request.purpose, true);
			break;
		case EditorRequestKind::PickFile:
			show_picker(p_request.purpose, false);
			break;
		case EditorRequestKind::RevealPath:
			if (!p_request.path.empty()) {
				OS::get_singleton()->shell_show_in_file_manager(opennova::to_gd(p_request.path), true);
			}
			break;
		case EditorRequestKind::Quit:
			get_tree()->quit(0);
			break;
		default:
			break;
	}
}

void EditorApp::show_picker(PickPurpose p_purpose, bool p_directory) {
	if (picker_ == nullptr) {
		picker_ = memnew(FileDialog);
		picker_->set_use_native_dialog(true);
		picker_->set_access(FileDialog::ACCESS_FILESYSTEM);
		add_child(picker_);
		picker_->connect("dir_selected", Callable(this, "_on_dir_selected"));
		picker_->connect("file_selected", Callable(this, "_on_file_selected"));
		picker_->connect("canceled", Callable(this, "_on_picker_canceled"));
	}
	pending_pick_ = p_purpose;
	picker_->set_file_mode(p_directory ? FileDialog::FILE_MODE_OPEN_DIR : FileDialog::FILE_MODE_OPEN_FILE);
	PackedStringArray filters;
	switch (p_purpose) {
		case PickPurpose::NewProjectLocation:
			picker_->set_title("Choose a folder for the new project");
			break;
		case PickPurpose::OpenProject:
			picker_->set_title("Open a project folder");
			break;
		case PickPurpose::RuntimeExecutable:
			picker_->set_title("Choose the game runtime (opennova.exe)");
			filters.push_back("*.exe ; Game runtime");
			break;
		case PickPurpose::None:
			break;
	}
	picker_->set_filters(filters);
	picker_->popup_centered_ratio(0.75f);
}

void EditorApp::_on_dir_selected(const String &p_dir) {
#if OPENNOVA_EDITOR_UI
	windows_->deliver_pick(pending_pick_, opennova::to_std(p_dir));
#endif
	pending_pick_ = PickPurpose::None;
}

void EditorApp::_on_file_selected(const String &p_file) {
#if OPENNOVA_EDITOR_UI
	windows_->deliver_pick(pending_pick_, opennova::to_std(p_file));
#endif
	pending_pick_ = PickPurpose::None;
}

void EditorApp::_on_picker_canceled() {
	pending_pick_ = PickPurpose::None;
}

// A free loopback port for the game's MCP endpoint: bind an ephemeral port, read it,
// let it go (the child binds it moments later).
int EditorApp::allocate_mcp_port() {
	Ref<TCPServer> probe;
	probe.instantiate();
	if (probe->listen(0, "127.0.0.1") != OK) {
		return 0;
	}
	const int port = probe->get_local_port();
	probe->stop();
	return port;
}

// --- the typed seam --------------------------------------------------------------

bool EditorApp::new_project(const String &p_dir, const String &p_title) {
	ensure_session();
	session_->handle(opennova::editor::make_request(EditorRequestKind::NewProject, opennova::to_std(p_dir),
			opennova::to_std(p_title)));
	return session_->project_open();
}

bool EditorApp::open_project(const String &p_dir) {
	ensure_session();
	session_->handle(opennova::editor::make_request(EditorRequestKind::OpenProject, opennova::to_std(p_dir)));
	return session_->project_open();
}

void EditorApp::close_project() {
	ensure_session();
	session_->handle(opennova::editor::make_request(EditorRequestKind::CloseProject));
}

int EditorApp::create_missing_files() {
	ensure_session();
	session_->handle(opennova::editor::make_request(EditorRequestKind::CreateMissing));
	return get_required_missing();
}

bool EditorApp::build() {
	ensure_session();
	session_->handle(opennova::editor::make_request(EditorRequestKind::Build));
	session_->finish_build();
	return is_last_build_ok();
}

bool EditorApp::play() {
	ensure_session();
	session_->set_launcher(make_launcher(allocate_mcp_port()));
	session_->handle(opennova::editor::make_request(EditorRequestKind::Play));
	session_->finish_build();
	return session_->view().play_state == PlayState::Running;
}

void EditorApp::stop_play() {
	ensure_session();
	session_->handle(opennova::editor::make_request(EditorRequestKind::StopPlay));
}

bool EditorApp::is_project_open() const {
	return session_ && session_->project_open();
}

String EditorApp::get_project_title() const {
	return session_ ? opennova::to_gd(session_->view().document.title) : String();
}

String EditorApp::get_project_root() const {
	return session_ ? opennova::to_gd(session_->view().project_root) : String();
}

int EditorApp::get_required_missing() const {
	if (!session_) {
		return 0;
	}
	const SessionView &v = session_->view();
	return v.requirements.required_missing + v.requirements.required_wrong_kind;
}

int EditorApp::get_required_total() const {
	return session_ ? session_->view().requirements.required_total : 0;
}

String EditorApp::get_last_build_dir() const {
	return session_ ? opennova::to_gd(session_->view().last_build.build_dir) : String();
}

bool EditorApp::is_last_build_ok() const {
	return session_ && session_->view().has_build && session_->view().last_build.ok;
}

String EditorApp::get_play_state() const {
	return session_ ? String(opennova::editor::play_state_label(session_->view().play_state)) : String("stopped");
}

bool EditorApp::did_game_exit_on_its_own() const {
	return session_ && session_->view().play_exited_on_its_own;
}

int EditorApp::get_problem_count() const {
	return session_ ? static_cast<int>(session_->view().diagnostics.size()) : 0;
}

PackedStringArray EditorApp::get_output_lines() const {
	PackedStringArray lines;
	if (session_) {
		for (const std::string &line : session_->view().output) {
			lines.push_back(opennova::to_gd(line));
		}
	}
	return lines;
}

PackedStringArray EditorApp::get_recent_projects() const {
	PackedStringArray roots;
	if (session_) {
		for (const std::string &root : session_->view().recent_projects) {
			roots.push_back(opennova::to_gd(root));
		}
	}
	return roots;
}

} // namespace godot
