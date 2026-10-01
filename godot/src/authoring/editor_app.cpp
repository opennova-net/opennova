#include "authoring/editor_app.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <runtime/devtools/imgui_pass.h>

#include <editor/preview/viewport_device_cache.h>
#include <editor/preview/viewports.h>
#include <editor/run/launch_plan.h>
#include <editor/session/file_preferences_store.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view/viewport_kind.h>

#include "authoring/viewport_device.h"
#include "authoring/viewport_devices.h"
#include "object/object_model.h"
#include "resource_index/launch_flags.h"
#include "util/string_convert.h"

using opennova::editor::EditorRequest;
using opennova::editor::EditorRequestKind;
using opennova::editor::PickPurpose;
using opennova::editor::PlayLauncher;
using opennova::editor::ProjectSession;
using opennova::editor::ViewportKind;

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
	ClassDB::bind_method(D_METHOD("set_build_budget_ms", "ms"), &EditorApp::set_build_budget_ms);
	ClassDB::bind_method(D_METHOD("get_build_budget_ms"), &EditorApp::get_build_budget_ms);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "build_budget_ms"), "set_build_budget_ms", "get_build_budget_ms");
	ClassDB::bind_method(D_METHOD("get_viewport_device", "path", "kind"), &EditorApp::get_viewport_device);
	ClassDB::bind_method(D_METHOD("start_mcp_endpoint", "port"), &EditorApp::start_mcp_endpoint);
	ClassDB::bind_method(D_METHOD("get_mcp_port"), &EditorApp::get_mcp_port);
	ClassDB::bind_method(D_METHOD("get_status_text"), &EditorApp::get_status_text);

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
	// The viewports' devices render offscreen through the runtime's MenuFrame and ObjectModel, or are a
	// Control placed over the canvas's rect (the script device's CodeEdit, S13 V10)
	// (authoring/viewport_devices), drawn only by a viewport's view; headless runs keep them too (the
	// MCP, the tests read what they placed), each Preview-role kind's target and the active document's
	// Main view given one, where the workspace gives one only to the kind the Preview window shows (its
	// views ask for the rest as they draw). A device given up retires its SubViewport here, freed at the
	// next frame; a Control device's requests (the script device's edits) are served at once, those it
	// raises inside a pump (its burst's EndEdit as it is given up) at the next pump's start, and its
	// notices (an edit it refused) are on the status line.
	devices_ = std::make_unique<opennova::editor::ViewportDeviceCache>([this](ViewportKind p_kind) {
		return make_viewport_device(*this, p_kind,
				[this](SubViewport *p_viewport) { retired_.push_back(p_viewport->get_instance_id()); },
				ViewportDeviceSink{
						[this](const EditorRequest &p_request) { serve_device_request_(p_request); },
						[this](const EditorRequest &p_request) { queue_device_request_(p_request); },
						[this](const std::string &p_text) { post_device_notice_(p_text); } });
	});
	devices_->set_pin_all_targets(!is_available());
	// A planner with no canvas (the wire's drag and drop) reads the device of its viewport there.
	session_->viewports().set_devices(devices_.get());
	if (is_available()) {
#if OPENNOVA_EDITOR_UI
		windows_->set_devices(devices_.get());
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
	windows_->set_devices(nullptr);
#endif
	if (session_) session_->viewports().set_devices(nullptr);
	devices_.reset();
	free_retired_();
	if (picker_ != nullptr) {
		picker_->queue_free();
		picker_ = nullptr;
	}
	ImGuiPassNode::_exit_tree();
	set_process(false);
}

void EditorApp::free_retired_() {
	for (const uint64_t id : retired_)
		if (Node *viewport = Object::cast_to<Node>(ObjectDB::get_instance(id))) viewport->queue_free();
	retired_.clear();
}

void EditorApp::_process(double p_delta) {
	// What the last frame gave up: its draw is done.
	free_retired_();
	ImGuiPassNode::_process(p_delta); // the layout pass (no-op headless): the windows raise requests
	pump();
	// The one model runtime-frame driver in the editor (menu_shell.gd's for the game's menus): the
	// preview clock runs, and the viewports' part animations, flipbooks, generators and clips run on
	// it, whether a canvas draws them or not (pump() synced the devices). Then the devices' builds a
	// unit further (S13 V6): one unit a frame in all, the most recently used device's first, the next
	// while the frame's build budget lasts (shared by every build in flight).
	if (devices_) {
		session_->advance(p_delta);
		devices_->tick(session_->viewports());
		const uint64_t start = Time::get_singleton()->get_ticks_usec();
		const uint64_t budget = uint64_t(build_budget_ms_) * 1000;
		devices_->step(session_->viewports(),
				[start, budget] { return Time::get_singleton()->get_ticks_usec() - start < budget; });
		ObjectModel::advance_awake_frame(p_delta);
	}
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
	// What the devices raised inside the last pump (a burst's EndEdit as its device was given up),
	// then the windows' requests of this frame (a burst of keystrokes, a drag), then the poll, whose
	// budget steps the validation they left due first (S13 A3: no request runs it).
	serve_queued_device_requests_();
	drain_requests();
	session_->poll();
	// The devices follow their viewports: the Preview's targets given one, each taking what its
	// viewport asks.
	if (devices_) devices_->sync(session_->viewports(), session_->view());
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

// A device's request (the script device's span edits and its keystroke burst's end, S13 V10), served
// at once: the device raises it from the control's deferred signals, outside the pump and its sync.
void EditorApp::serve_device_request_(const EditorRequest &p_request) {
	if (!session_) return;
	if (!session_->handle(p_request)) serve(p_request);
}

// A device's request raised inside a pump, where it is not served (a device given up as the devices
// sync, with its burst open: its EndEdit): kept for the start of the next pump.
void EditorApp::queue_device_request_(const EditorRequest &p_request) {
	queued_device_requests_.push_back(p_request);
}

void EditorApp::serve_queued_device_requests_() {
	std::vector<EditorRequest> queued;
	queued.swap(queued_device_requests_);
	for (const EditorRequest &request : queued) serve_device_request_(request);
}

// A device's notice (an edit the script device refused: a character the game's code page has no byte
// for) where the person sees it: the pass's status line in the menu bar, an error for a few seconds
// and kept in its history under the pointer.
void EditorApp::post_device_notice_(const std::string &p_text) {
#if OPENNOVA_EDITOR_UI
	windows_->pass().post_status(p_text, opennova::devtools::StatusLevel::Error);
#else
	(void)p_text;
#endif
}

String EditorApp::get_status_text() const {
#if OPENNOVA_EDITOR_UI
	return String::utf8(windows_->pass().status_text());
#else
	return String();
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

// --- the device parity tests' seam --------------------------------------------------------------

SubViewport *EditorApp::get_viewport_device(const String &p_path, const String &p_kind) const {
	ViewportKind kind = ViewportKind::kCount;
	if (!devices_ || !opennova::editor::viewport_kind_from_token(opennova::to_std(p_kind), kind)) return nullptr;
	// The Shell's cache makes its devices by the kinds' table alone (authoring/viewport_devices).
	auto *device = static_cast<ViewportDevice *>(devices_->held(opennova::to_std(p_path), kind));
	return device ? device->sub_viewport() : nullptr;
}

void EditorApp::_notification(int p_what) {
	if (p_what == NOTIFICATION_WM_CLOSE_REQUEST) {
		ensure_session(); session_->handle(opennova::editor::request::quit());
	}
}
} // namespace godot
