#include "devtools/oned_ui.h"
#include "util/string_convert.h"

namespace godot {

using opennova::to_gd;
using opennova::to_std;

void OnedUiRequest::assign(const opennova::devtools::OnedRequest &p_request) {
	action_ = static_cast<int>(p_request.action);
	index_ = p_request.index;
}

void OnedUiRequest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_action"), &OnedUiRequest::get_action);
	ClassDB::bind_method(D_METHOD("get_index"), &OnedUiRequest::get_index);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "action"), "", "get_action");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "index"), "", "get_index");
}

OnedUi::OnedUi() : ui_(std::make_unique<opennova::devtools::OnedUi>()) {}

OnedUi::~OnedUi() = default;

void OnedUi::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resource_dir", "value"), &OnedUi::set_resource_dir);
	ClassDB::bind_method(D_METHOD("get_resource_dir"), &OnedUi::get_resource_dir);
	ClassDB::bind_method(D_METHOD("set_game_code", "value"), &OnedUi::set_game_code);
	ClassDB::bind_method(D_METHOD("get_game_code"), &OnedUi::get_game_code);
	ClassDB::bind_method(D_METHOD("set_expansion", "value"), &OnedUi::set_expansion);
	ClassDB::bind_method(D_METHOD("get_expansion"), &OnedUi::get_expansion);
	ClassDB::bind_method(D_METHOD("set_retail_dir", "value"), &OnedUi::set_retail_dir);
	ClassDB::bind_method(D_METHOD("get_retail_dir"), &OnedUi::get_retail_dir);
	ClassDB::bind_method(D_METHOD("set_recent_dirs", "dirs"), &OnedUi::set_recent_dirs);
	ClassDB::bind_method(D_METHOD("get_recent_dirs"), &OnedUi::get_recent_dirs);
	ClassDB::bind_method(D_METHOD("set_readiness", "opennova_block", "retail_block", "running"),
			&OnedUi::set_readiness);
	ClassDB::bind_method(D_METHOD("get_opennova_block"), &OnedUi::get_opennova_block);
	ClassDB::bind_method(D_METHOD("get_retail_block"), &OnedUi::get_retail_block);
	ClassDB::bind_method(D_METHOD("is_running"), &OnedUi::is_running);
	ClassDB::bind_method(D_METHOD("set_status", "text", "kind"), &OnedUi::set_status);
	ClassDB::bind_method(D_METHOD("get_status_text"), &OnedUi::get_status_text);
	ClassDB::bind_method(D_METHOD("get_status_kind"), &OnedUi::get_status_kind);
	ClassDB::bind_method(D_METHOD("take_request"), &OnedUi::take_request);
	ClassDB::bind_method(D_METHOD("has_requests"), &OnedUi::has_requests);
	ClassDB::bind_method(D_METHOD("push_request", "action", "index"), &OnedUi::push_request, DEFVAL(-1));
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "resource_dir"), "set_resource_dir", "get_resource_dir");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "game_code"), "set_game_code", "get_game_code");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "expansion"), "set_expansion", "get_expansion");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "retail_dir"), "set_retail_dir", "get_retail_dir");
	BIND_ENUM_CONSTANT(NONE);
	BIND_ENUM_CONSTANT(EDIT_RESOURCE_DIR);
	BIND_ENUM_CONSTANT(APPLY_RESOURCE_DIR);
	BIND_ENUM_CONSTANT(APPLY_RETAIL_DIR);
	BIND_ENUM_CONSTANT(COMMIT_PROFILE);
	BIND_ENUM_CONSTANT(BROWSE_RESOURCE_DIR);
	BIND_ENUM_CONSTANT(BROWSE_RETAIL_DIR);
	BIND_ENUM_CONSTANT(SELECT_RECENT);
	BIND_ENUM_CONSTANT(CLEAR_RECENTS);
	BIND_ENUM_CONSTANT(RUN_OPENNOVA);
	BIND_ENUM_CONSTANT(RUN_RETAIL);
	BIND_ENUM_CONSTANT(STOP);
}

opennova::devtools::ImGuiPass *OnedUi::engine_pass() {
	return &ui_->pass();
}

void OnedUi::set_resource_dir(const String &p_value) {
	ui_->set_resource_dir(to_std(p_value));
}

String OnedUi::get_resource_dir() const {
	return to_gd(ui_->resource_dir());
}

void OnedUi::set_game_code(const String &p_value) {
	ui_->set_game_code(to_std(p_value));
}

String OnedUi::get_game_code() const {
	return to_gd(ui_->game_code());
}

void OnedUi::set_expansion(const String &p_value) {
	ui_->set_expansion(to_std(p_value));
}

String OnedUi::get_expansion() const {
	return to_gd(ui_->expansion());
}

void OnedUi::set_retail_dir(const String &p_value) {
	ui_->set_retail_dir(to_std(p_value));
}

String OnedUi::get_retail_dir() const {
	return to_gd(ui_->retail_dir());
}

void OnedUi::set_recent_dirs(const PackedStringArray &p_dirs) {
	std::vector<std::string> dirs;
	dirs.reserve(static_cast<size_t>(p_dirs.size()));
	for (int i = 0; i < p_dirs.size(); ++i) {
		dirs.push_back(to_std(p_dirs[i]));
	}
	ui_->set_recent_dirs(std::move(dirs));
}

PackedStringArray OnedUi::get_recent_dirs() const {
	PackedStringArray dirs;
	for (const std::string &dir : ui_->recent_dirs()) {
		dirs.push_back(to_gd(dir));
	}
	return dirs;
}

void OnedUi::set_readiness(const String &p_opennova_block, const String &p_retail_block, bool p_running) {
	ui_->set_readiness(to_std(p_opennova_block), to_std(p_retail_block), p_running);
}

String OnedUi::get_opennova_block() const {
	return to_gd(ui_->opennova_block());
}

String OnedUi::get_retail_block() const {
	return to_gd(ui_->retail_block());
}

bool OnedUi::is_running() const {
	return ui_->is_running();
}

void OnedUi::set_status(const String &p_text, const StringName &p_kind) {
	opennova::devtools::OnedStatusKind kind = opennova::devtools::OnedStatusKind::INFO;
	if (p_kind == StringName("error")) {
		kind = opennova::devtools::OnedStatusKind::ERROR;
	} else if (p_kind == StringName("warn")) {
		kind = opennova::devtools::OnedStatusKind::WARN;
	}
	ui_->set_status(to_std(p_text), kind);
}

String OnedUi::get_status_text() const {
	return to_gd(ui_->status_text());
}

StringName OnedUi::get_status_kind() const {
	switch (ui_->status_kind()) {
		case opennova::devtools::OnedStatusKind::ERROR:
			return StringName("error");
		case opennova::devtools::OnedStatusKind::WARN:
			return StringName("warn");
		case opennova::devtools::OnedStatusKind::INFO:
			break;
	}
	return StringName("info");
}

Ref<OnedUiRequest> OnedUi::take_request() {
	opennova::devtools::OnedRequest request;
	if (!ui_->take_request(request)) {
		return Ref<OnedUiRequest>();
	}
	Ref<OnedUiRequest> out;
	out.instantiate();
	out->assign(request);
	return out;
}

bool OnedUi::has_requests() const {
	return ui_->has_requests();
}

void OnedUi::push_request(int p_action, int p_index) {
	ui_->push_request(opennova::devtools::OnedRequest{static_cast<opennova::devtools::OnedAction>(p_action), p_index});
}

} // namespace godot
