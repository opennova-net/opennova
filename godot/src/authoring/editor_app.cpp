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
	ClassDB::bind_method(D_METHOD("create_catalog", "path"), &EditorApp::create_catalog);
	ClassDB::bind_method(D_METHOD("open_catalog", "path"), &EditorApp::open_catalog);
	ClassDB::bind_method(D_METHOD("get_catalog_row_count"), &EditorApp::get_catalog_row_count);
	ClassDB::bind_method(D_METHOD("get_catalog_row_id", "index"), &EditorApp::get_catalog_row_id);
	ClassDB::bind_method(D_METHOD("get_catalog_row_name", "index"), &EditorApp::get_catalog_row_name);
	ClassDB::bind_method(D_METHOD("remove_catalog_record", "id"), &EditorApp::remove_catalog_record);
	ClassDB::bind_method(D_METHOD("set_catalog_text", "id", "field", "value"), &EditorApp::set_catalog_text);
	ClassDB::bind_method(D_METHOD("set_catalog_integer", "id", "field", "value"), &EditorApp::set_catalog_integer);
	ClassDB::bind_method(D_METHOD("set_catalog_real", "id", "field", "value"), &EditorApp::set_catalog_real);
	ClassDB::bind_method(D_METHOD("get_catalog_text", "id", "field"), &EditorApp::get_catalog_text);
	ClassDB::bind_method(D_METHOD("get_catalog_integer", "id", "field"), &EditorApp::get_catalog_integer);
	ClassDB::bind_method(D_METHOD("save_catalogs"), &EditorApp::save_catalogs);
	ClassDB::bind_method(D_METHOD("catalog_undo"), &EditorApp::catalog_undo);
	ClassDB::bind_method(D_METHOD("catalog_redo"), &EditorApp::catalog_redo);
	ClassDB::bind_method(D_METHOD("is_catalog_dirty"), &EditorApp::is_catalog_dirty);
	ClassDB::bind_method(D_METHOD("has_unsaved_prompt"), &EditorApp::has_unsaved_prompt);
	ClassDB::bind_method(D_METHOD("resolve_unsaved", "choice"), &EditorApp::resolve_unsaved);
	ClassDB::bind_method(D_METHOD("get_play_mcp_port"), &EditorApp::get_play_mcp_port);
	ClassDB::bind_method(D_METHOD("add_catalog_record", "kind", "parent"), &EditorApp::add_catalog_record, DEFVAL(0));

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
	if (get_tree()->get_current_scene() == this) get_tree()->set_auto_accept_quit(false);
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
	if (session_->view().quit_requested) get_tree()->quit(0);
}

void EditorApp::drain_requests() {
#if OPENNOVA_EDITOR_UI
	EditorRequest request;
	while (windows_->take_request(request)) {
		if (request.kind == EditorRequestKind::Play) {
			// The port is the shell's to allocate: a fresh loopback port per run.
			session_->set_launcher(make_launcher(session_->view().play_retail ? 0 : allocate_mcp_port()));
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
		case PickPurpose::RetailDirectory:
			picker_->set_title("Choose the Joint Operations install folder");
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
	ensure_session();
	EditorRequest request = opennova::editor::make_request(EditorRequestKind::PreviewImport);
	for (int i = 0; i < p_files.size(); ++i) request.paths.push_back(opennova::to_std(p_files[i]));
	session_->handle(request);
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
	session_->set_launcher(make_launcher(session_->view().play_retail ? 0 : allocate_mcp_port()));
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


namespace {
opennova::editor::CatalogAddress catalog_address(const opennova::editor::EditableDocument &document, int64_t id) {
	using namespace opennova::def;
	for (const auto &row : document.rows()) {
		if (row->id == uint64_t(id)) return {row->id, row->kind, 0};
		for (auto child : row->children) if (child == uint64_t(id))
			return {row->id, row->kind == DefRecordKind::Item ? DefRecordKind::Attachment :
				row->kind == DefRecordKind::Weapon ? DefRecordKind::Action : DefRecordKind::Effect, child};
		for (auto child : row->sights) if (child == uint64_t(id)) return {row->id, DefRecordKind::Sight, child};
	}
	return {};
}
bool catalog_set(ProjectSession &session, int64_t id, const String &field, opennova::def::DefValue value) {
	auto *document = session.document_for();
	if (!document) return false;
	const auto before = document->revision();
	auto request = opennova::editor::make_request(EditorRequestKind::EditRecord, document->path());
	request.catalog_edit.address = catalog_address(*document, id);
	request.catalog_edit.field = opennova::to_std(field); request.catalog_edit.value = std::move(value);
	session.handle(request);
	return document->revision() != before;
}
}
bool EditorApp::create_catalog(const String &p_path) {
    ensure_session();
    session_->handle(opennova::editor::make_request(EditorRequestKind::CreateCatalog, opennova::to_std(p_path)));
    return session_->document_for(opennova::to_std(p_path)) != nullptr;
}
bool EditorApp::open_catalog(const String &p_path) {
	ensure_session();
	session_->handle(opennova::editor::make_request(EditorRequestKind::OpenDocument, opennova::to_std(p_path)));
	return session_->document_for(opennova::to_std(p_path)) != nullptr;
}
int EditorApp::get_catalog_row_count() const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document ? int(document->rows().size()) : 0;
}
int64_t EditorApp::get_catalog_row_id(int p_index) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document && p_index >= 0 && size_t(p_index) < document->rows().size() ? int64_t(document->rows()[p_index]->id) : 0;
}
String EditorApp::get_catalog_row_name(int p_index) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document && p_index >= 0 && size_t(p_index) < document->rows().size() ? opennova::to_gd(document->rows()[p_index]->name()) : String();
}
int64_t EditorApp::add_catalog_record(const String &p_kind, int64_t p_parent) {
	ensure_session();
	auto *document = session_->document_for(); if (!document) return 0;
	const char *kinds[] = {"item", "weapon", "ammo", "action", "sight", "attachment", "effect", "carry"};
	int type = -1;
	for (int i = 0; i < 8; ++i) if (p_kind == kinds[i]) type = i;
	if (type < 0) return 0;
	const auto revision = document->revision();
	auto request = opennova::editor::make_request(EditorRequestKind::EditRecord);
	request.catalog_edit.operation = opennova::editor::CatalogOperation::Add;
	request.catalog_edit.address = {uint64_t(p_parent), static_cast<opennova::def::DefRecordKind>(type), 0};
	session_->handle(request);
	return document->revision() != revision ? int64_t(document->last_added()) : 0;
}
bool EditorApp::set_catalog_text(int64_t p_id, const String &p_field, const String &p_value) {
	ensure_session(); return catalog_set(*session_, p_id, p_field, opennova::to_std(p_value));
}
bool EditorApp::set_catalog_integer(int64_t p_id, const String &p_field, int64_t p_value) {
	ensure_session(); return catalog_set(*session_, p_id, p_field, p_value);
}
bool EditorApp::set_catalog_real(int64_t p_id, const String &p_field, double p_value) {
	ensure_session(); return catalog_set(*session_, p_id, p_field, p_value);
}
String EditorApp::get_catalog_text(int64_t p_id, const String &p_field) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	if (!document) return {};
	const auto address = catalog_address(*document, p_id);
	const auto *field = opennova::def::def_field(address.kind, opennova::to_std(p_field));
	const void *record = document->record(address);
	if (!field || !record || field->type != opennova::def::DefFieldType::Text) return {};
	return opennova::to_gd(std::get<std::string>(opennova::def::def_get(record, *field)));
}
int64_t EditorApp::get_catalog_integer(int64_t p_id, const String &p_field) const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	if (!document) return 0;
	const auto address = catalog_address(*document, p_id);
	const auto *field = opennova::def::def_field(address.kind, opennova::to_std(p_field));
	const void *record = document->record(address);
	if (!field || !record) return 0;
	const auto value = opennova::def::def_get(record, *field);
	return std::holds_alternative<int64_t>(value) ? std::get<int64_t>(value) : 0;
}
bool EditorApp::remove_catalog_record(int64_t p_id) {
	ensure_session(); auto *document = session_->document_for(); if (!document) return false;
	const auto revision = document->revision();
	auto request = opennova::editor::make_request(EditorRequestKind::EditRecord);
	request.catalog_edit.operation = opennova::editor::CatalogOperation::Remove;
	request.catalog_edit.address = catalog_address(*document, p_id);
	session_->handle(request); return document->revision() != revision;
}
bool EditorApp::save_catalogs() {
	ensure_session(); session_->handle(opennova::editor::make_request(EditorRequestKind::SaveAll));
	return !session_->documents_dirty();
}
void EditorApp::catalog_undo() { ensure_session(); session_->handle(opennova::editor::make_request(EditorRequestKind::Undo)); }
void EditorApp::catalog_redo() { ensure_session(); session_->handle(opennova::editor::make_request(EditorRequestKind::Redo)); }
bool EditorApp::is_catalog_dirty() const {
	const auto *document = session_ ? session_->document_for() : nullptr;
	return document && document->dirty();
}
bool EditorApp::has_unsaved_prompt() const { return session_ && session_->view().unsaved_prompt; }
void EditorApp::resolve_unsaved(int p_choice) {
	if (p_choice < 0 || p_choice > 2) return;
	ensure_session();
	auto request = opennova::editor::make_request(EditorRequestKind::ResolveUnsaved);
	request.unsaved_choice = static_cast<opennova::editor::UnsavedChoice>(p_choice);
	session_->handle(request);
}
int EditorApp::get_play_mcp_port() const { return session_ ? session_->launcher().mcp_port : 0; }
void EditorApp::_notification(int p_what) {
	if (p_what == NOTIFICATION_WM_CLOSE_REQUEST) {
		ensure_session(); session_->handle(opennova::editor::make_request(EditorRequestKind::Quit));
	}
}

} // namespace godot
