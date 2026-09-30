#include "authoring/editor_app.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_preview_json.h>
#include <editor/preview/menu_preview_state.h>
#include <editor/preview/menu_render_check.h>
#include <editor/preview/menu_report.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/preview/model_preview_json.h>
#include <editor/run/launch_plan.h>
#include <editor/session/file_preferences_store.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/session_operation.h>
#include <editor/session/session_view.h>

#include "resource_index/launch_flags.h"
#include "util/string_convert.h"

using opennova::editor::EditorRequest;
using opennova::editor::EditorRequestKind;
using opennova::editor::PickPurpose;
using opennova::editor::PlayLauncher;
using opennova::editor::ProjectSession;
using opennova::editor::SessionView;

namespace godot {

namespace {

constexpr const char *kSmokeFlag = "--editor-smoke";
// `--project <dir>` opens that project at boot (an agent's launch, a shortcut).
constexpr const char *kProjectFlag = "--project";
// The editor's MCP transport, loaded by path: the game products never ship
// res://editor/, and the shell names no GDScript class (ADR 0043 d12's pattern).
constexpr const char *kMcpServicePath = "res://editor/mcp/editor_mcp_service.gd";

} // namespace

void EditorApp::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create_file", "path"), &EditorApp::create_file);
	ClassDB::bind_method(D_METHOD("open_document", "path"), &EditorApp::open_document);
	ClassDB::bind_method(D_METHOD("get_row_count"), &EditorApp::get_row_count);
	ClassDB::bind_method(D_METHOD("get_row_id", "index"), &EditorApp::get_row_id);
	ClassDB::bind_method(D_METHOD("get_row_name", "index"), &EditorApp::get_row_name);
	ClassDB::bind_method(D_METHOD("add_record", "kind", "parent", "position"), &EditorApp::add_record, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_record", "id"), &EditorApp::remove_record);
	ClassDB::bind_method(D_METHOD("set_field", "id", "field", "value"), &EditorApp::set_field);
	ClassDB::bind_method(D_METHOD("clear_field", "id", "field"), &EditorApp::clear_field);
	ClassDB::bind_method(D_METHOD("write_field", "id", "field"), &EditorApp::write_field);
	ClassDB::bind_method(D_METHOD("get_field", "id", "field"), &EditorApp::get_field);
	ClassDB::bind_method(D_METHOD("save_documents"), &EditorApp::save_documents);
	ClassDB::bind_method(D_METHOD("undo"), &EditorApp::undo);
	ClassDB::bind_method(D_METHOD("redo"), &EditorApp::redo);
	ClassDB::bind_method(D_METHOD("is_document_dirty"), &EditorApp::is_document_dirty);
	ClassDB::bind_method(D_METHOD("get_child_records", "id", "kind"), &EditorApp::get_child_records, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_record_owner", "id"), &EditorApp::get_record_owner);
	ClassDB::bind_method(D_METHOD("get_record_name", "id"), &EditorApp::get_record_name);
	ClassDB::bind_method(D_METHOD("find_record", "symbol", "scope"), &EditorApp::find_record, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("has_unsaved_prompt"), &EditorApp::has_unsaved_prompt);
	ClassDB::bind_method(D_METHOD("resolve_unsaved", "choice"), &EditorApp::resolve_unsaved);
	ClassDB::bind_method(D_METHOD("get_play_mcp_port"), &EditorApp::get_play_mcp_port);
	ClassDB::bind_method(D_METHOD("duplicate_record", "id"), &EditorApp::duplicate_record);
	ClassDB::bind_method(D_METHOD("move_record", "id", "position", "parent"), &EditorApp::move_record, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("select_record", "id", "mode"), &EditorApp::select_record, DEFVAL("replace"));
	ClassDB::bind_method(D_METHOD("get_selected_records"), &EditorApp::get_selected_records);
	ClassDB::bind_method(D_METHOD("copy_records"), &EditorApp::copy_records);
	ClassDB::bind_method(D_METHOD("cut_records"), &EditorApp::cut_records);
	ClassDB::bind_method(D_METHOD("paste_records", "parent", "position"), &EditorApp::paste_records, DEFVAL(0), DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("end_edit"), &EditorApp::end_edit);
	ClassDB::bind_method(D_METHOD("request_json", "json"), &EditorApp::request_json);
	ClassDB::bind_method(D_METHOD("get_outcome_json"), &EditorApp::get_outcome_json);
	ClassDB::bind_method(D_METHOD("get_operation_json"), &EditorApp::get_operation_json);
	ClassDB::bind_method(D_METHOD("get_view_json", "output_cursor", "output_limit", "import_offset", "import_limit"),
			&EditorApp::get_view_json, DEFVAL(0), DEFVAL(200), DEFVAL(0), DEFVAL(200));
	ClassDB::bind_method(D_METHOD("get_document_json", "path", "with_rows"), &EditorApp::get_document_json);
	ClassDB::bind_method(D_METHOD("get_record_json", "id"), &EditorApp::get_record_json);
	ClassDB::bind_method(D_METHOD("get_reference_choices_json", "id", "field"), &EditorApp::get_reference_choices_json);
	ClassDB::bind_method(D_METHOD("get_reference_targets_json", "id", "field"), &EditorApp::get_reference_targets_json);
	ClassDB::bind_method(D_METHOD("search_document_json", "path", "text", "match_case"), &EditorApp::search_document_json,
	                     DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_problems_json", "query"), &EditorApp::get_problems_json, DEFVAL("{}"));
	ClassDB::bind_method(D_METHOD("get_request_kinds"), &EditorApp::get_request_kinds);
	ClassDB::bind_method(D_METHOD("get_references_json", "path"), &EditorApp::get_references_json);
	ClassDB::bind_method(D_METHOD("get_referrers_json", "path"), &EditorApp::get_referrers_json);
	ClassDB::bind_method(D_METHOD("get_symbol_referrers_json", "kind", "name", "scope"), &EditorApp::get_symbol_referrers_json,
	                     DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_usages_json", "path"), &EditorApp::get_usages_json);
	ClassDB::bind_method(D_METHOD("get_missing_references_json"), &EditorApp::get_missing_references_json);
	ClassDB::bind_method(D_METHOD("get_symbols_json", "kind"), &EditorApp::get_symbols_json);
	ClassDB::bind_method(D_METHOD("search_project_json", "text"), &EditorApp::search_project_json);
	ClassDB::bind_method(D_METHOD("get_menu_preview_json"), &EditorApp::get_menu_preview_json);
	ClassDB::bind_method(D_METHOD("menu_preview_hit_json", "x", "y"), &EditorApp::menu_preview_hit_json);
	ClassDB::bind_method(D_METHOD("set_menu_preview_options", "options"), &EditorApp::set_menu_preview_options);
	ClassDB::bind_method(D_METHOD("menu_preview_drag", "id", "handle", "dx", "dy", "snap"), &EditorApp::menu_preview_drag);
	ClassDB::bind_method(D_METHOD("menu_preview_arrange", "ids", "op"), &EditorApp::menu_preview_arrange);
	ClassDB::bind_method(D_METHOD("get_model_preview_json"), &EditorApp::get_model_preview_json);
	ClassDB::bind_method(D_METHOD("model_preview_hit_json", "x", "y"), &EditorApp::model_preview_hit_json);
	ClassDB::bind_method(D_METHOD("set_model_preview_options", "options"), &EditorApp::set_model_preview_options);
	ClassDB::bind_method(D_METHOD("set_model_preview_camera", "camera"), &EditorApp::set_model_preview_camera);
	ClassDB::bind_method(D_METHOD("model_preview_drag", "id", "handle", "x", "y", "snap"), &EditorApp::model_preview_drag,
			DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_model_preview_camera"), &EditorApp::get_model_preview_camera);
	ClassDB::bind_method(D_METHOD("get_model_preview_model"), &EditorApp::get_model_preview_model);
	ClassDB::bind_method(D_METHOD("get_menu_render_json", "path", "screen"), &EditorApp::get_menu_render_json);
	ClassDB::bind_method(D_METHOD("get_menu_tree_json", "path"), &EditorApp::get_menu_tree_json);
	ClassDB::bind_method(D_METHOD("get_menu_findings_json", "path"), &EditorApp::get_menu_findings_json);
	ClassDB::bind_method(D_METHOD("edit_menu_json", "path", "json"), &EditorApp::edit_menu_json);
	ClassDB::bind_method(D_METHOD("start_mcp_endpoint", "port"), &EditorApp::start_mcp_endpoint);
	ClassDB::bind_method(D_METHOD("get_mcp_port"), &EditorApp::get_mcp_port);

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
	ClassDB::bind_method(D_METHOD("stop_play"), &EditorApp::stop_play);
	ClassDB::bind_method(D_METHOD("pump"), &EditorApp::pump);
	ClassDB::bind_method(D_METHOD("set_poll_budget", "ms", "step_bytes"), &EditorApp::set_poll_budget);
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
	ClassDB::bind_method(D_METHOD("_on_files_selected", "files"), &EditorApp::_on_files_selected);
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
	preferences_ = std::make_unique<opennova::editor::FilePreferencesStore>(opennova::to_std(settings));
	session_ = std::make_unique<ProjectSession>(*platform_, *preferences_);
	// What Play launches: asked when the game is spawned, once its build lands, so the port of the
	// game's MCP endpoint is a fresh one then, never one held while the build packs.
	session_->set_launcher_source([this](bool p_with_mcp_port) {
		return make_launcher(p_with_mcp_port ? allocate_mcp_port() : 0);
	});
#if OPENNOVA_EDITOR_UI
	windows_->set_view(&session_->view());
#endif
}

bool EditorApp::is_source_run() const {
	return !OS::get_singleton()->has_feature("template");
}

// Where Play finds the game (opennova::editor::make_play_launcher): this binary at the
// checkout for a source run, else the runtime packaged beside the editor.
PlayLauncher EditorApp::make_launcher(int p_mcp_port) const {
	const bool source_run = is_source_run();
	std::vector<std::string> engine_args;
	for (int i = 0; i < play_engine_args_.size(); ++i) {
		engine_args.push_back(opennova::to_std(play_engine_args_[i]));
	}
	return opennova::editor::make_play_launcher(source_run, opennova::to_std(OS::get_singleton()->get_executable_path()),
			source_run ? opennova::to_std(ProjectSettings::get_singleton()->globalize_path("res://")) : std::string(),
			p_mcp_port, std::move(engine_args));
}

void EditorApp::_ready() {
	if (get_tree()->get_current_scene() == this) get_tree()->set_auto_accept_quit(false);
	ensure_session();
	ImGuiPassNode::_ready();
	// The session pumps whether or not the workspace draws (headless tests, the smoke).
	set_process(true);
#if OPENNOVA_EDITOR_UI
	// The menu preview renders through the runtime's MenuFrame into an offscreen viewport
	// the Preview window's menu pane draws; headless runs keep it too (its JSON: the MCP,
	// the tests).
	menu_preview_ = std::make_unique<MenuPreview>(*this);
	// The model preview the same way, through the runtime's ObjectModel.
	model_preview_ = std::make_unique<ModelPreview>(*this);
#endif
	if (is_available()) {
#if OPENNOVA_EDITOR_UI
		windows_->set_menu_preview_viewport(menu_preview_.get());
		windows_->set_model_preview_viewport(model_preview_.get());
#endif
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
	for (int i = 0; i + 1 < args.size(); ++i) {
		if (args[i] == kProjectFlag) open_project(args[i + 1]);
	}
	// The editor's own endpoint: the same flag the game reads (docs/mcp.md).
	const int mcp_port = LaunchFlags::mcp_port();
	if (mcp_port > 0) start_mcp_endpoint(mcp_port);
}

int EditorApp::start_mcp_endpoint(int p_port) {
	if (mcp_service_ != nullptr) return mcp_port_;
	if (p_port < 0 || p_port > 65535) return 0;
	ResourceLoader *loader = ResourceLoader::get_singleton();
	if (!loader->exists(kMcpServicePath)) {
		UtilityFunctions::push_warning("--mcp-port ", p_port, " ignored: the editor MCP transport is not in this build (",
				kMcpServicePath, ").");
		return 0;
	}
	Ref<Script> script = loader->load(kMcpServicePath);
	if (script.is_null()) {
		UtilityFunctions::push_warning("The editor MCP transport failed to load from ", kMcpServicePath, ".");
		return 0;
	}
	Node *service = Object::cast_to<Node>(script->call("new"));
	if (service == nullptr) {
		UtilityFunctions::push_warning("The editor MCP transport is not a Node (", kMcpServicePath, ").");
		return 0;
	}
	service->set_name("EditorMcpService");
	add_child(service);
	// The transport's class is reached by name: the one dynamic call of the load-by-path seam.
	const int err = int(service->call("setup", this, p_port));
	if (err != OK) {
		UtilityFunctions::push_warning("The editor MCP failed to start on port ", p_port, ": ", err, ".");
		service->queue_free();
		return 0;
	}
	mcp_service_ = service;
	mcp_port_ = int(service->call("get_port"));
	return mcp_port_;
}

void EditorApp::_exit_tree() {
	mcp_service_ = nullptr; // a child: it leaves with the tree and stops its server
	mcp_port_ = 0;
#if OPENNOVA_EDITOR_UI
	if (menu_preview_) {
		windows_->set_menu_preview_viewport(nullptr);
		menu_preview_.reset();
	}
	if (model_preview_) {
		windows_->set_model_preview_viewport(nullptr);
		model_preview_.reset();
	}
#endif
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
#if OPENNOVA_EDITOR_UI
	// The one model runtime-frame driver in the editor (menu_shell.gd's for the game's
	// menus): the preview's part animations, flipbooks and generators run on it, whether
	// the Preview window shows the model pane or not (pump() refreshed both devices).
	if (model_preview_) {
		model_preview_->tick(p_delta);
		ObjectModel::advance_awake_frame(p_delta);
	}
#endif
}

void EditorApp::before_layout(double) {
#if OPENNOVA_EDITOR_UI
	windows_->begin_frame();
#endif
}

void EditorApp::after_layout(uint64_t, bool, int64_t) {
#if OPENNOVA_EDITOR_UI
	windows_->end_frame();
#endif
}

void EditorApp::pump() {
	ensure_session();
	// The windows' requests of this frame (a burst of keystrokes, a drag) validate once,
	// at the poll; a request that arrives any other way returns validated.
	session_->hold_validation();
	drain_requests();
	session_->poll();
#if OPENNOVA_EDITOR_UI
	if (menu_preview_) menu_preview_->refresh(session_->view());
	if (model_preview_) model_preview_->refresh(session_->view());
#endif
	apply_window_title();
	if (session_->view().quit_requested) get_tree()->quit(0);
}

void EditorApp::set_poll_budget(int p_ms, int64_t p_step_bytes) {
	ensure_session();
	session_->set_poll_budget({std::max(p_ms, 0), uint64_t(std::max<int64_t>(p_step_bytes, 1))});
}

void EditorApp::apply_window_title() {
	// The editor's own window: a test's EditorApp is not the scene the OS window shows.
	if (!is_inside_tree() || get_tree()->get_current_scene() != this) return;
	const String title = opennova::to_gd(opennova::editor::editor_window_title(session_->view()));
	if (title == window_title_) return;
	window_title_ = title;
	DisplayServer::get_singleton()->window_set_title(title);
}

void EditorApp::drain_requests() {
#if OPENNOVA_EDITOR_UI
	EditorRequest request;
	while (windows_->take_request(request)) {
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
		picker_->connect("files_selected", Callable(this, "_on_files_selected"));
		picker_->connect("canceled", Callable(this, "_on_picker_canceled"));
	}
	pending_pick_ = p_purpose;
	picker_->set_file_mode(p_directory ? FileDialog::FILE_MODE_OPEN_DIR :
			p_purpose == PickPurpose::ImportFiles ? FileDialog::FILE_MODE_OPEN_FILES : FileDialog::FILE_MODE_OPEN_FILE);
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
		case PickPurpose::GameInstall:
			picker_->set_title("Choose the game install folder");
			break;
		case PickPurpose::ImportFiles:
			picker_->set_title("Import files or PFF contents");
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

void EditorApp::_on_files_selected(const PackedStringArray &p_files) {
#if OPENNOVA_EDITOR_UI
	// Through the windows' request path, as every other picker result: drained with the frame's
	// requests, never handled from the dialog's signal.
	std::vector<std::string> paths;
	for (int i = 0; i < p_files.size(); ++i) paths.push_back(opennova::to_std(p_files[i]));
	windows_->deliver_picks(pending_pick_, paths);
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
// let it go (the child binds it moments later: the session asks at spawn time).
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

// Whether the project at `dir` is the one open now: a switch that failed or was refused leaves
// the project that was open before it open (or none).
bool EditorApp::project_open_at(const std::string &p_dir) const {
	return session_->project_open() &&
			session_->view().project_root == opennova::editor::ProjectPaths::for_root(p_dir).root;
}

bool EditorApp::new_project(const String &p_dir, const String &p_title) {
	ensure_session();
	const std::string dir = opennova::to_std(p_dir);
	session_->handle(opennova::editor::request::new_project(dir, opennova::to_std(p_title)));
	// Made and opened: the request went through (a folder that holds a project already refuses
	// it, the open project's own among them) and the project it made is the one open.
	return session_->outcome().done() && project_open_at(dir);
}

bool EditorApp::open_project(const String &p_dir) {
	ensure_session();
	const std::string dir = opennova::to_std(p_dir);
	session_->handle(opennova::editor::request::open_project(dir));
	return project_open_at(dir);
}

void EditorApp::close_project() {
	ensure_session();
	session_->handle(opennova::editor::request::close_project());
}

int EditorApp::create_missing_files() {
	ensure_session();
	const auto roles = opennova::editor::unmet_required_roles(session_->view().requirements);
	session_->handle(opennova::editor::request::create_missing(roles));
	return get_required_missing();
}

void EditorApp::stop_play() {
	ensure_session();
	session_->handle(opennova::editor::request::stop_play());
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


namespace {
using opennova::editor::Document;
using opennova::editor::Edit;
using opennova::editor::EditOperation;
using opennova::editor::NodeAddress;
using opennova::editor::Value;

bool to_value(const Variant &variant, Value &out) {
	switch (variant.get_type()) {
	case Variant::INT: out = int64_t(variant); return true;
	case Variant::FLOAT: out = double(variant); return true;
	case Variant::STRING: out = opennova::to_std(String(variant)); return true;
	case Variant::BOOL: out = int64_t(bool(variant) ? 1 : 0); return true;
	default: return false;
	}
}

Variant to_variant(const Value &value) {
	if (const auto *number = std::get_if<int64_t>(&value)) return Variant(*number);
	if (const auto *real = std::get_if<double>(&value)) return Variant(*real);
	return Variant(opennova::to_gd(std::get<std::string>(value)));
}

// True when the request on the active document went through (a Move that leaves a
// record where it is included: nothing was wrong, nothing changed); false for one the session
// refused before its edit ran (an operation holding the documents), whose edit flag is the last
// edit's.
bool edited(ProjectSession &session, Document &document, EditorRequest request) {
	request.path = document.path();
	session.handle(request);
	return session.outcome().done() && session.last_edit_ok();
}

// A record's identity as the seam names it: the nested record's, else the row's.
int64_t identity_of(const NodeAddress &address) { return int64_t(address.child ? address.child : address.row); }

// An optional field of a record of the active document left out (Clear) or written again
// (Write).
bool presence_edited(ProjectSession &session, int64_t id, const String &field, EditOperation operation) {
	auto *document = session.document_for();
	if (!document) return false;
	Edit edit;
	edit.operation = operation;
	edit.address = document->address_of(uint64_t(id));
	edit.field = opennova::to_std(field);
	const EditorRequest request = opennova::editor::request::edit_record({}, std::move(edit));
	return edited(session, *document, request);
}
} // namespace

bool EditorApp::create_file(const String &p_path) {
	ensure_session();
	session_->handle(opennova::editor::request::create_file(opennova::to_std(p_path)));
	return session_->outcome().done();
}
bool EditorApp::open_document(const String &p_path) {
	ensure_session();
	session_->handle(opennova::editor::request::open_document(opennova::to_std(p_path)));
	return session_->document_for(opennova::to_std(p_path)) != nullptr;
}
int EditorApp::get_row_count() const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document ? int(document->rows().size()) : 0;
}
int64_t EditorApp::get_row_id(int p_index) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document && p_index >= 0 && size_t(p_index) < document->rows().size() ? int64_t(document->rows()[p_index]->id) : 0;
}
String EditorApp::get_row_name(int p_index) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document && p_index >= 0 && size_t(p_index) < document->rows().size() ? opennova::to_gd(document->rows()[p_index]->name()) : String();
}
int64_t EditorApp::add_record(const String &p_kind, int64_t p_parent, int64_t p_position) {
	ensure_session();
	auto *document = session_->document_for();
	if (!document || p_parent < 0) return 0;
	const auto kind = document->kind_from_name(opennova::to_std(p_kind));
	if (kind < 0) return 0;
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kind, 0};
	add.parent = uint64_t(p_parent);
	add.position = p_position < 0 ? SIZE_MAX : size_t(p_position);
	const EditorRequest request = opennova::editor::request::edit_record({}, std::move(add));
	return edited(*session_, *document, request) ? int64_t(document->last_added()) : 0;
}
bool EditorApp::remove_record(int64_t p_id) {
	ensure_session();
	auto *document = session_->document_for();
	if (!document) return false;
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = document->address_of(uint64_t(p_id));
	const EditorRequest request = opennova::editor::request::edit_record({}, std::move(remove));
	return edited(*session_, *document, request);
}
bool EditorApp::set_field(int64_t p_id, const String &p_field, const Variant &p_value) {
	ensure_session();
	auto *document = session_->document_for();
	if (!document) return false;
	Edit set;
	set.address = document->address_of(uint64_t(p_id));
	set.field = opennova::to_std(p_field);
	if (!to_value(p_value, set.value)) return false;
	const EditorRequest request = opennova::editor::request::edit_record({}, std::move(set));
	return edited(*session_, *document, request);
}
bool EditorApp::clear_field(int64_t p_id, const String &p_field) {
	ensure_session();
	return presence_edited(*session_, p_id, p_field, EditOperation::Clear);
}
bool EditorApp::write_field(int64_t p_id, const String &p_field) {
	ensure_session();
	return presence_edited(*session_, p_id, p_field, EditOperation::Write);
}
Variant EditorApp::get_field(int64_t p_id, const String &p_field) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	if (!document) return Variant();
	const NodeAddress address = document->address_of(uint64_t(p_id));
	const std::string field = opennova::to_std(p_field);
	Value value;
	if (!document->get(address, field, value) || !document->present(address, field)) return Variant();
	return to_variant(value);
}
bool EditorApp::save_documents() {
	ensure_session();
	session_->handle(opennova::editor::request::save_all());
	return !session_->documents_dirty();
}
void EditorApp::undo() { ensure_session(); session_->handle(opennova::editor::request::undo()); }
void EditorApp::redo() { ensure_session(); session_->handle(opennova::editor::request::redo()); }
bool EditorApp::is_document_dirty() const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document && document->dirty();
}
PackedInt64Array EditorApp::get_child_records(int64_t p_id, const String &p_kind) const {
	PackedInt64Array children;
	const auto *document = session_ ? session_->document_for() : nullptr;
	if (!document) return children;
	const std::string kind = opennova::to_std(p_kind);
	for (const Document::Collection &collection : document->collections_of(document->address_of(uint64_t(p_id)))) {
		if (!kind.empty() && kind != document->kind_token(collection.spec.kind)) continue;
		for (const opennova::editor::NodeId id : collection.ids) children.push_back(int64_t(id));
	}
	return children;
}
int64_t EditorApp::get_record_owner(int64_t p_id) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	Document::Placement at;
	if (!document || !document->placement(document->address_of(uint64_t(p_id)), at)) return 0;
	return identity_of(at.owner);
}
String EditorApp::get_record_name(int64_t p_id) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	if (!document) return String();
	return opennova::to_gd(document->record_name(document->address_of(uint64_t(p_id))));
}
int64_t EditorApp::find_record(const String &p_symbol, const String &p_scope) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	NodeAddress address;
	if (!document || !document->find(opennova::to_std(p_symbol), address, opennova::to_std(p_scope))) return 0;
	return identity_of(address);
}
bool EditorApp::has_unsaved_prompt() const { return session_ && session_->view().unsaved_prompt.open; }
void EditorApp::resolve_unsaved(int p_choice) {
	if (p_choice < 0 || p_choice > 2) return;
	ensure_session();
	const auto choice = static_cast<opennova::editor::UnsavedChoice>(p_choice);
	session_->handle(opennova::editor::request::resolve_unsaved(choice));
}
int EditorApp::get_play_mcp_port() const { return session_ ? session_->view().play_mcp_port : 0; }
bool EditorApp::duplicate_record(int64_t p_id) {
	ensure_session();
	auto *document = session_->document_for();
	if (!document) return false;
	Edit duplicate;
	duplicate.operation = EditOperation::Duplicate;
	duplicate.address = document->address_of(uint64_t(p_id));
	// The copy goes right after the record, in its owner's collection or among the rows.
	Document::Placement at;
	if (document->placement(duplicate.address, at)) duplicate.position = at.index + 1;
	for (size_t i = 0; !duplicate.address.child && i < document->rows().size(); ++i)
		if (document->rows()[i]->id == duplicate.address.row) duplicate.position = i + 1;
	const EditorRequest request = opennova::editor::request::edit_record({}, std::move(duplicate));
	return edited(*session_, *document, request);
}
bool EditorApp::move_record(int64_t p_id, int p_position, int64_t p_parent) {
	ensure_session();
	auto *document = session_->document_for();
	if (!document || p_position < 0 || p_parent < 0) return false;
	Edit move;
	move.operation = EditOperation::Move;
	move.address = document->address_of(uint64_t(p_id));
	move.position = size_t(p_position);
	move.parent = uint64_t(p_parent);
	const EditorRequest request = opennova::editor::request::edit_record({}, std::move(move));
	return edited(*session_, *document, request);
}
bool EditorApp::select_record(int64_t p_id, const String &p_mode) {
	ensure_session();
	auto *document = session_->document_for();
	opennova::editor::SelectMode mode;
	if (!document || !opennova::editor::select_mode_from_token(opennova::to_std(p_mode), mode)) return false;
	const NodeAddress address = document->address_of(uint64_t(p_id));
	if (!address.row) return false;
	session_->handle(opennova::editor::request::select_record(document->path(), address, mode));
	return true;
}
PackedInt64Array EditorApp::get_selected_records() const {
	PackedInt64Array ids;
	if (session_)
		for (const NodeAddress &address : session_->view().selected) ids.push_back(identity_of(address));
	return ids;
}
bool EditorApp::copy_records() {
	ensure_session();
	auto *document = session_->document_for();
	return document && edited(*session_, *document, opennova::editor::request::copy());
}
bool EditorApp::cut_records() {
	ensure_session();
	auto *document = session_->document_for();
	return document && edited(*session_, *document, opennova::editor::request::cut());
}
bool EditorApp::paste_records(int64_t p_parent, int p_position) {
	ensure_session();
	auto *document = session_->document_for();
	if (!document || p_parent < 0) return false;
	opennova::editor::PasteAt at;
	at.parent = uint64_t(p_parent);
	at.position = p_position < 0 ? SIZE_MAX : size_t(p_position);
	return edited(*session_, *document, opennova::editor::request::paste(std::string(), at));
}
void EditorApp::end_edit() {
	ensure_session();
	if (auto *document = session_->document_for())
		session_->handle(opennova::editor::request::end_edit(document->path()));
}

// --- the wire seam ---------------------------------------------------------------

namespace {
String json_text(const opennova::io::JsonValue &value) { return opennova::to_gd(opennova::io::json_write(value)); }

// A Variant as the JSON the portable readers take: a bool, a number, a string, an array or a
// dictionary with string keys, nested; false for anything else.
bool to_json(const Variant &p_value, opennova::io::JsonValue &r_out) {
	using opennova::io::JsonValue;
	switch (p_value.get_type()) {
		case Variant::BOOL:
			r_out = JsonValue::make_bool(bool(p_value));
			return true;
		case Variant::INT:
			r_out = JsonValue::make_number(double(int64_t(p_value)));
			return true;
		case Variant::FLOAT:
			r_out = JsonValue::make_number(double(p_value));
			return true;
		case Variant::STRING:
		case Variant::STRING_NAME:
			r_out = JsonValue::make_string(opennova::to_std(String(p_value)));
			return true;
		case Variant::ARRAY: {
			r_out = JsonValue::make_array();
			const Array items = p_value;
			for (int64_t i = 0; i < items.size(); ++i) {
				JsonValue item;
				if (!to_json(items[i], item)) return false;
				r_out.push(std::move(item));
			}
			return true;
		}
		case Variant::DICTIONARY: {
			r_out = JsonValue::make_object();
			const Dictionary members = p_value;
			const Array keys = members.keys();
			for (int64_t i = 0; i < keys.size(); ++i) {
				JsonValue member;
				if ((keys[i].get_type() != Variant::STRING && keys[i].get_type() != Variant::STRING_NAME) ||
						!to_json(members[keys[i]], member)) {
					return false;
				}
				r_out.set(opennova::to_std(String(keys[i])), std::move(member));
			}
			return true;
		}
		default:
			return false;
	}
}
} // namespace

String EditorApp::request_json(const String &p_json) {
	ensure_session();
	opennova::io::JsonValue json, answer = opennova::io::JsonValue::make_object();
	std::string error;
	EditorRequest request;
	bool ok = opennova::io::json_parse(opennova::to_std(p_json), json, error) &&
			opennova::editor::editor_request_from_json(json, request, error);
	bool served = false;
	// A kind the shell serves with a person to answer it (the pickers) never comes through here.
	const auto served_by = opennova::editor::request_kind_row(request.kind).served_by;
	if (ok && served_by == opennova::editor::ServedBy::ShellNeedsPerson) {
		ok = false;
		error = "The pickers need a person: pass the path with new_project, open_project, apply_project_settings "
				"or preview_import instead.";
	}
	if (ok) {
		served = session_->handle(request);
		if (!served) serve(request);
	}
	answer.set("ok", opennova::io::JsonValue::make_bool(ok));
	answer.set("served", opennova::io::JsonValue::make_bool(served));
	if (!error.empty()) answer.set("error", opennova::io::JsonValue::make_string(error));
	// What the request came to (`ok` only says it parsed): the session's outcome, or a
	// plain done for a kind the shell served.
	if (ok) {
		answer.set("outcome", opennova::editor::action_outcome_to_json(
				served ? session_->outcome() : opennova::editor::ActionOutcome()));
	}
	answer.set("status", opennova::io::JsonValue::make_string(session_->view().status));
	const double revision = double(session_->view().revisions.any());
	answer.set("revision", opennova::io::JsonValue::make_number(revision));
	return json_text(answer);
}

String EditorApp::get_outcome_json() const {
	return json_text(opennova::editor::action_outcome_to_json(session_ ? session_->outcome() : opennova::editor::ActionOutcome()));
}

String EditorApp::get_operation_json() const {
	opennova::io::JsonValue answer = opennova::io::JsonValue::make_object();
	const SessionView empty;
	const SessionView &view = session_ ? session_->view() : empty;
	answer.set("operation", opennova::editor::operation_status_to_json(view.operation));
	answer.set("last_operation", opennova::editor::operation_outcome_to_json(view.last_operation));
	return json_text(answer);
}

String EditorApp::get_view_json(int p_output_cursor, int p_output_limit, int p_import_offset, int p_import_limit) const {
	if (!session_) return String("{}");
	opennova::editor::SessionJsonOptions options;
	options.output_cursor = size_t(std::max(p_output_cursor, 0));
	options.output_limit = size_t(std::max(p_output_limit, 0));
	options.import_offset = size_t(std::max(p_import_offset, 0));
	options.import_limit = size_t(std::max(p_import_limit, 0));
	return json_text(opennova::editor::session_view_to_json(session_->view(), options));
}

String EditorApp::get_document_json(const String &p_path, bool p_with_rows) const {
	const Document *document = session_ ? session_->document_for(opennova::to_std(p_path)) : nullptr;
	if (!document) return String("null");
	return json_text(opennova::editor::document_to_json(*document, p_with_rows));
}

String EditorApp::get_record_json(int64_t p_id) const {
	const Document *document = session_ ? session_->document_for() : nullptr;
	if (!document) return String("null");
	return json_text(opennova::editor::record_to_json(*document, document->address_of(uint64_t(p_id)), session_->view()));
}

String EditorApp::get_reference_choices_json(int64_t p_id, const String &p_field) const {
	const Document *document = session_ ? session_->document_for() : nullptr;
	if (!document) return String("null");
	return json_text(opennova::editor::reference_choices_to_json(*document, document->address_of(uint64_t(p_id)),
			opennova::to_std(p_field), session_->view()));
}

String EditorApp::get_reference_targets_json(int64_t p_id, const String &p_field) const {
	const Document *document = session_ ? session_->document_for() : nullptr;
	if (!document) return String("null");
	return json_text(opennova::editor::reference_targets_to_json(*document, document->address_of(uint64_t(p_id)),
			opennova::to_std(p_field), session_->view()));
}

String EditorApp::search_document_json(const String &p_path, const String &p_text, bool p_match_case) const {
	const Document *document = session_ ? session_->document_for(opennova::to_std(p_path)) : nullptr;
	if (!document) return String("null");
	opennova::editor::SearchOptions options;
	options.match_case = p_match_case;
	return json_text(opennova::editor::document_hits_to_json(
			opennova::editor::find_in_document(*document, opennova::to_std(p_text), options)));
}

String EditorApp::get_problems_json(const String &p_query) {
	ensure_session();
	return opennova::to_gd(session_->problems_json(opennova::to_std(p_query)));
}

// The kinds request_json serves: every row of the request table but those a person answers.
PackedStringArray EditorApp::get_request_kinds() const {
	PackedStringArray kinds;
	for (size_t i = 0; i < opennova::editor::kEditorRequestKindCount; ++i) {
		const auto &row = opennova::editor::request_kind_row(static_cast<EditorRequestKind>(i));
		if (row.served_by != opennova::editor::ServedBy::ShellNeedsPerson)
			kinds.push_back(opennova::to_gd(row.token));
	}
	return kinds;
}

String EditorApp::get_references_json(const String &p_path) const {
	const auto *graph = session_ ? session_->view().graph.get() : nullptr;
	if (!graph) return String("[]");
	return json_text(opennova::editor::graph_edges_to_json(*graph, graph->references_of(opennova::to_std(p_path))));
}

String EditorApp::get_referrers_json(const String &p_path) const {
	const auto *graph = session_ ? session_->view().graph.get() : nullptr;
	if (!graph) return String("[]");
	return json_text(opennova::editor::graph_edges_to_json(*graph, graph->referrers_of_file(opennova::to_std(p_path))));
}

String EditorApp::get_symbol_referrers_json(const String &p_kind, const String &p_name, const String &p_scope) const {
	const auto *graph = session_ ? session_->view().graph.get() : nullptr;
	opennova::editor::ReferenceKind kind;
	if (!graph || !opennova::editor::reference_kind_from_token(opennova::to_std(p_kind), kind)) return String("[]");
	return json_text(opennova::editor::graph_edges_to_json(
	        *graph, graph->referrers_of(kind, opennova::to_std(p_name), opennova::to_std(p_scope))));
}

String EditorApp::get_usages_json(const String &p_path) const {
	const auto *graph = session_ ? session_->view().graph.get() : nullptr;
	if (!graph) return String("[]");
	return json_text(opennova::editor::graph_edges_to_json(*graph, graph->usages_of(opennova::to_std(p_path))));
}

String EditorApp::get_missing_references_json() const {
	const auto *graph = session_ ? session_->view().graph.get() : nullptr;
	if (!graph) return String("[]");
	return json_text(opennova::editor::graph_edges_to_json(*graph, graph->missing()));
}

String EditorApp::search_project_json(const String &p_text) const {
	const auto *graph = session_ ? session_->view().graph.get() : nullptr;
	if (!graph) return json_text(opennova::editor::graph_search_to_json({}));
	return json_text(opennova::editor::graph_search_to_json(graph->search(opennova::to_std(p_text))));
}

String EditorApp::get_symbols_json(const String &p_kind) const {
	const auto *graph = session_ ? session_->view().graph.get() : nullptr;
	opennova::editor::ReferenceKind kind;
	const bool filtered = !p_kind.is_empty();
	if (!graph || (filtered && !opennova::editor::reference_kind_from_token(opennova::to_std(p_kind), kind))) return String("[]");
	opennova::io::JsonValue out = opennova::io::JsonValue::make_array();
	for (const opennova::editor::GraphSymbol &symbol : graph->symbols())
		if (!filtered || symbol.kind == kind) out.push(opennova::editor::graph_symbol_to_json(symbol));
	return json_text(out);
}
String EditorApp::get_menu_preview_json() {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	if (menu_preview_) {
		menu_preview_->refresh(session_->view());
		return json_text(opennova::editor::menu_preview_to_json(menu_preview_->snapshot(session_->view())));
	}
#endif
	opennova::editor::MenuPreviewModel none;
	return json_text(opennova::editor::menu_preview_to_json(
			opennova::editor::menu_preview_snapshot(session_->view(), none, nullptr, nullptr)));
}

String EditorApp::menu_preview_hit_json(double p_x, double p_y) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	if (menu_preview_) {
		menu_preview_->refresh(session_->view());
		return json_text(opennova::editor::menu_preview_hit_to_json(menu_preview_->snapshot(session_->view()),
				float(p_x), float(p_y)));
	}
#endif
	opennova::editor::MenuPreviewModel none;
	return json_text(opennova::editor::menu_preview_hit_to_json(
			opennova::editor::menu_preview_snapshot(session_->view(), none, nullptr, nullptr), float(p_x), float(p_y)));
}

String EditorApp::get_menu_render_json(const String &p_path, int64_t p_screen) const {
	opennova::editor::MenuPreviewSnapshot none;
	none.status = opennova::editor::MenuPreviewStatus::NoScreen;
	if (!session_ || !session_->view().render_check) {
		none.status = opennova::editor::MenuPreviewStatus::NoProject;
		return json_text(opennova::editor::menu_preview_to_json(none));
	}
	const opennova::editor::SessionView &view = session_->view();
	const std::string path = opennova::to_std(p_path);
	const opennova::editor::MenuRenderCheck &check = *view.render_check;
	// The open document when the menu is open (its current state), else the file as the
	// check read it.
	const opennova::editor::MnuDocument *document = check.document(path);
	for (const auto &open : view.documents) {
		if (open && open->path() == path) {
			document = dynamic_cast<const opennova::editor::MnuDocument *>(open.get());
		}
	}
	const opennova::editor::NodeId row = opennova::editor::NodeId(p_screen);
	const opennova::editor::Node *screen = document != nullptr ? document->row(row) : nullptr;
	const opennova::editor::MenuScreenRender *render = check.render(path, row);
	if (document == nullptr || screen == nullptr || render == nullptr) {
		none.document = document;
		return json_text(opennova::editor::menu_preview_to_json(none));
	}
	return json_text(opennova::editor::menu_preview_to_json(
			opennova::editor::render_snapshot(*render, *document, *screen)));
}

String EditorApp::get_menu_tree_json(const String &p_path) const {
	if (!session_) return String("null");
	return json_text(opennova::editor::menu_tree_to_json(session_->view(), opennova::to_std(p_path)));
}

String EditorApp::get_menu_findings_json(const String &p_path) const {
	if (!session_) return String("null");
	return json_text(opennova::editor::menu_findings_to_json(session_->view(), opennova::to_std(p_path)));
}

String EditorApp::edit_menu_json(const String &p_path, const String &p_json) {
	ensure_session();
	opennova::io::JsonValue json;
	std::string error;
	if (!opennova::io::json_parse(opennova::to_std(p_json), json, error)) {
		opennova::io::JsonValue answer = opennova::io::JsonValue::make_object();
		answer.set("ok", opennova::io::JsonValue::make_bool(false));
		answer.set("error", opennova::io::JsonValue::make_string("The edits are not JSON: " + error));
		return json_text(answer);
	}
	return json_text(opennova::editor::menu_edit_request(*session_, opennova::to_std(p_path), json));
}

bool EditorApp::set_menu_preview_options(const Dictionary &p_options) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	if (!menu_preview_) return false;
	opennova::io::JsonValue json;
	opennova::editor::MenuPreviewOptions options = menu_preview_->options();
	if (!to_json(p_options, json) || !opennova::editor::menu_preview_options_from_json(json, options)) return false;
	menu_preview_->set_options(options);
	menu_preview_->refresh(session_->view());
	return true;
#else
	(void)p_options;
	return false;
#endif
}

String EditorApp::get_model_preview_json() {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	if (model_preview_) {
		model_preview_->refresh(session_->view());
		return json_text(opennova::editor::model_preview_to_json(model_preview_->snapshot(session_->view())));
	}
#endif
	opennova::editor::ModelPreviewModel none;
	return json_text(opennova::editor::model_preview_to_json(
			opennova::editor::model_preview_snapshot(session_->view(), none, false)));
}

String EditorApp::model_preview_hit_json(double p_x, double p_y) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	if (model_preview_) {
		model_preview_->refresh(session_->view());
		return json_text(opennova::editor::model_preview_hit_to_json(model_preview_->snapshot(session_->view()),
				float(p_x), float(p_y)));
	}
#endif
	opennova::editor::ModelPreviewModel none;
	return json_text(opennova::editor::model_preview_hit_to_json(
			opennova::editor::model_preview_snapshot(session_->view(), none, false), float(p_x), float(p_y)));
}

bool EditorApp::set_model_preview_options(const Dictionary &p_options) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	opennova::io::JsonValue json;
	if (!model_preview_ || !to_json(p_options, json) ||
			!opennova::editor::model_preview_options_from_json(json, model_preview_->model())) {
		return false;
	}
	model_preview_->refresh(session_->view());
	return true;
#else
	(void)p_options;
	return false;
#endif
}

bool EditorApp::set_model_preview_camera(const Dictionary &p_camera) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	opennova::io::JsonValue json;
	if (!model_preview_ || !to_json(p_camera, json) ||
			!opennova::editor::model_preview_camera_from_json(json, model_preview_->model())) {
		return false;
	}
	model_preview_->refresh(session_->view());
	return true;
#else
	(void)p_camera;
	return false;
#endif
}

bool EditorApp::model_preview_drag(int64_t p_id, const String &p_handle, double p_x, double p_y, double p_snap) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	opennova::editor::ModelHandle handle;
	if (!model_preview_ || !opennova::editor::model_handle_from_token(p_handle.utf8().get_data(), handle)) return false;
	model_preview_->refresh(session_->view());
	const bool ok = opennova::editor::model_preview_drag(*session_, model_preview_->snapshot(session_->view()),
			opennova::editor::NodeId(p_id), handle, float(p_x), float(p_y), float(p_snap));
	model_preview_->refresh(session_->view());
	return ok;
#else
	(void)p_id;
	(void)p_handle;
	(void)p_x;
	(void)p_y;
	(void)p_snap;
	return false;
#endif
}

Camera3D *EditorApp::get_model_preview_camera() const {
#if OPENNOVA_EDITOR_UI
	return model_preview_ ? model_preview_->camera() : nullptr;
#else
	return nullptr;
#endif
}

ObjectModel *EditorApp::get_model_preview_model() const {
#if OPENNOVA_EDITOR_UI
	return model_preview_ ? model_preview_->object_model() : nullptr;
#else
	return nullptr;
#endif
}

bool EditorApp::menu_preview_drag(int64_t p_id, const String &p_handle, int p_dx, int p_dy, bool p_snap) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	opennova::editor::LayoutHandle handle;
	if (!menu_preview_ || !opennova::editor::layout_handle_from_token(opennova::to_std(p_handle), handle)) return false;
	menu_preview_->refresh(session_->view());
	const bool ok = opennova::editor::menu_preview_drag(*session_, menu_preview_->snapshot(session_->view()),
			opennova::editor::NodeId(p_id), handle, p_dx, p_dy, p_snap);
	menu_preview_->refresh(session_->view());
	return ok;
#else
	(void)p_id;
	(void)p_handle;
	(void)p_dx;
	(void)p_dy;
	(void)p_snap;
	return false;
#endif
}

bool EditorApp::menu_preview_arrange(const PackedInt64Array &p_ids, const String &p_op) {
	ensure_session();
#if OPENNOVA_EDITOR_UI
	opennova::editor::ArrangeOp op;
	if (!menu_preview_ || !opennova::editor::arrange_op_from_token(opennova::to_std(p_op), op)) return false;
	menu_preview_->refresh(session_->view());
	std::vector<opennova::editor::NodeId> windows;
	for (int64_t i = 0; i < p_ids.size(); ++i) windows.push_back(opennova::editor::NodeId(p_ids[i]));
	const bool ok = opennova::editor::menu_preview_arrange(*session_, menu_preview_->snapshot(session_->view()), windows,
			op);
	menu_preview_->refresh(session_->view());
	return ok;
#else
	(void)p_ids;
	(void)p_op;
	return false;
#endif
}

void EditorApp::_notification(int p_what) {
	if (p_what == NOTIFICATION_WM_CLOSE_REQUEST) {
		ensure_session(); session_->handle(opennova::editor::request::quit());
	}
}

} // namespace godot
