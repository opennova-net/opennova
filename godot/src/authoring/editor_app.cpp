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

#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_preview_json.h>
#include <editor/preview/menu_preview_state.h>
#include <editor/preview/model_preview_json.h>
#include <editor/run/launch_plan.h>
#include <editor/session/file_preferences_store.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>

#include "resource_index/launch_flags.h"
#include "util/string_convert.h"

using opennova::editor::EditorRequest;
using opennova::editor::EditorRequestKind;
using opennova::editor::PickPurpose;
using opennova::editor::PlayLauncher;
using opennova::editor::ProjectSession;

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
	ClassDB::bind_method(D_METHOD("request_json", "json"), &EditorApp::request_json);
	ClassDB::bind_method(D_METHOD("query_json", "name", "args"), &EditorApp::query_json, DEFVAL("{}"));
	ClassDB::bind_method(D_METHOD("pump"), &EditorApp::pump);
	ClassDB::bind_method(D_METHOD("set_poll_budget", "ms", "step_bytes"), &EditorApp::set_poll_budget);
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
	ClassDB::bind_method(D_METHOD("start_mcp_endpoint", "port"), &EditorApp::start_mcp_endpoint);
	ClassDB::bind_method(D_METHOD("get_mcp_port"), &EditorApp::get_mcp_port);

	ClassDB::bind_method(D_METHOD("set_settings_path", "path"), &EditorApp::set_settings_path);
	ClassDB::bind_method(D_METHOD("get_settings_path"), &EditorApp::get_settings_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "settings_path"), "set_settings_path", "get_settings_path");
	ClassDB::bind_method(D_METHOD("set_play_engine_args", "args"), &EditorApp::set_play_engine_args);
	ClassDB::bind_method(D_METHOD("get_play_engine_args"), &EditorApp::get_play_engine_args);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "play_engine_args"), "set_play_engine_args",
			"get_play_engine_args");
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
		if (args[i] == kProjectFlag)
			session_->handle(opennova::editor::request::open_project(opennova::to_std(args[i + 1])));
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
	// The windows' requests of this frame (a burst of keystrokes, a drag), then the poll, whose
	// budget steps the validation they left due first (S13 A3: no request runs it).
	drain_requests();
	session_->poll();
#if OPENNOVA_EDITOR_UI
	if (menu_preview_) menu_preview_->refresh(session_->view());
	if (model_preview_) model_preview_->refresh(session_->view());
#endif
	apply_window_title();
	if (session_->view().dialogs.quit_requested) get_tree()->quit(0);
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
	opennova::io::JsonValue json;
	std::string error;
	opennova::editor::EditorRequest shell;
	if (!opennova::io::json_parse(opennova::to_std(p_json), json, error)) {
		opennova::io::JsonValue answer = session_->handle_json(opennova::io::JsonValue::make_null());
		answer.set("error", opennova::io::JsonValue::make_string("The request is not JSON: " + error));
		return json_text(answer);
	}
	const opennova::io::JsonValue answer = session_->handle_json(json, &shell);
	// A row the shell serves (reveal_path): the session left it to the shell.
	if (answer.get_bool("ok", false) && !answer.get_bool("served", true)) serve(shell);
	return json_text(answer);
}

String EditorApp::query_json(const String &p_name, const String &p_args) {
	ensure_session();
	opennova::io::JsonValue args, answer;
	std::string error;
	if (!opennova::io::json_parse(opennova::to_std(p_args), args, error)) {
		error = "The query's args are not JSON: " + error;
	} else {
		answer = session_->query(opennova::to_std(p_name), args, error);
	}
	if (!error.empty()) {
		answer = opennova::io::JsonValue::make_object();
		answer.set("error", opennova::io::JsonValue::make_string(error));
	}
	return json_text(answer);
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
