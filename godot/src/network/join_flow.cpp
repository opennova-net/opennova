#include "network/join_flow.h"

#include "network/lan_server_row.h"
#include "network/lan_session.h" // destroy_pre_dial_session
#include "resource_index/launch_flags.h"
#include "util/string_convert.h"

#include <vector>

namespace godot {

const Ref<JoinTarget> &JoinFlow::current_target() {
	if (current_.is_null())
		current_.instantiate();
	current_->set_record(flow_.target());
	return current_;
}

void JoinFlow::join(const Ref<JoinTarget> &p_target) {
	if (p_target.is_null())
		return;
	current_ = p_target;
	flow_.join(p_target->record());
}

void JoinFlow::refuse(const String &p_text) {
	current_.unref();
	flow_.refuse(opennova::to_std(p_text));
}

void JoinFlow::join_from_novaworld(const Ref<JoinTarget> &p_target, int64_t p_rid) {
	if (p_target.is_null())
		return;
	current_ = p_target;
	flow_.join_from_novaworld(p_target->record(), static_cast<uint32_t>(p_rid));
}

bool JoinFlow::launch_from_flags() {
	return flow_.launch(LaunchFlags::parsed());
}

void JoinFlow::browse_rows(const Array &p_rows, int p_serial) {
	std::vector<opennova::inmatch::DiscoveredSessionRow> rows;
	rows.reserve(static_cast<size_t>(p_rows.size()));
	for (int64_t i = 0; i < p_rows.size(); ++i) {
		const Ref<LanServerRow> row = p_rows[i];
		if (row.is_valid())
			rows.push_back(JoinTarget::row_facts(row));
	}
	flow_.browse_rows(static_cast<uint32_t>(p_serial), rows);
}

void JoinFlow::browse_finished(int p_serial) {
	flow_.browse_finished(static_cast<uint32_t>(p_serial));
}

void JoinFlow::commit_choice(
		const String &p_game_password, bool p_spectate, const String &p_team_password) {
	flow_.commit_choice(
			opennova::to_std(p_game_password), p_spectate, opennova::to_std(p_team_password));
}

int64_t JoinFlow::join_failed(const Ref<ConnectionError> &p_error) {
	const std::optional<uint32_t> rid =
			flow_.join_failed(p_error.is_valid() ? p_error->record_or_null() : nullptr);
	return rid ? static_cast<int64_t>(*rid) : 0;
}

// The device: each leg one hook.

bool JoinFlow::Device::begin_browse(
		const opennova::inmatch::JoinTargetRecord *target, uint32_t serial) {
	const Ref<JoinTarget> crossing =
			target != nullptr ? owner->current_target() : Ref<JoinTarget>();
	bool started = false;
	GDVIRTUAL_CALL_PTR(owner, _begin_browse, crossing, static_cast<int>(serial), started);
	return started;
}

void JoinFlow::Device::adopt_browse() {
	// The launch search's row starts a target of its own; the enumerate's is the join's.
	if (owner->flow_.state() == opennova::inmatch::JoinFlowState::LaunchSearch)
		owner->current_.instantiate();
	GDVIRTUAL_CALL_PTR(owner, _adopt_browse, owner->current_target());
}

void JoinFlow::Device::end_browse() {
	GDVIRTUAL_CALL_PTR(owner, _end_browse);
}
void JoinFlow::Device::drain_browse() {
	GDVIRTUAL_CALL_PTR(owner, _drain_browse);
}
void JoinFlow::Device::drain_enumerator() {
	GDVIRTUAL_CALL_PTR(owner, _drain_enumerator);
}

bool JoinFlow::Device::show_enumerating() {
	bool shown = false;
	GDVIRTUAL_CALL_PTR(owner, _show_enumerating, shown);
	return shown;
}

bool JoinFlow::Device::show_choice(uint32_t server_flags) {
	bool shown = false;
	GDVIRTUAL_CALL_PTR(owner, _show_choice, static_cast<int>(server_flags), shown);
	return shown;
}

bool JoinFlow::Device::show_refusal(const std::string &text) {
	bool shown = false;
	GDVIRTUAL_CALL_PTR(owner, _show_refusal, opennova::to_gd(text), shown);
	return shown;
}

void JoinFlow::Device::close_join_screen() {
	GDVIRTUAL_CALL_PTR(owner, _close_join_screen);
}

std::string JoinFlow::Device::callsign() {
	String name;
	GDVIRTUAL_CALL_PTR(owner, _callsign, name);
	return opennova::to_std(name);
}

std::string JoinFlow::Device::switch_expansion(const std::string &expansion) {
	String error;
	GDVIRTUAL_CALL_PTR(owner, _switch_expansion, opennova::to_gd(expansion), error);
	return opennova::to_std(error);
}

void JoinFlow::Device::save_profile() {
	GDVIRTUAL_CALL_PTR(owner, _save_profile);
}
void JoinFlow::Device::reload_profile() {
	GDVIRTUAL_CALL_PTR(owner, _reload_profile);
}
void JoinFlow::Device::save_config() {
	GDVIRTUAL_CALL_PTR(owner, _save_config);
}

void JoinFlow::Device::start_join(const opennova::inmatch::JoinTargetRecord &target) {
	(void)target; // the flow's own, which current_target() carries
	GDVIRTUAL_CALL_PTR(owner, _start_join, owner->current_target());
}

void JoinFlow::Device::start_launch_host(const opennova::inmatch::LaunchHost &host) {
	PackedStringArray rotation;
	for (const std::string &mission : host.rotation)
		rotation.push_back(opennova::to_gd(mission));
	GDVIRTUAL_CALL_PTR(owner, _start_launch_host, opennova::to_gd(host.mission), rotation,
			host.max_players, host.port, host.game_type, host.lan_mode);
}

// The pre-dial session's destroy is the process transport's (LanSession).
void JoinFlow::Device::destroy_pre_dial_session(bool novaworld) {
	LanSession::destroy_pre_dial_session(novaworld);
}

void JoinFlow::Device::cancel_pre_game() {
	GDVIRTUAL_CALL_PTR(owner, _cancel_pre_game);
}
void JoinFlow::Device::show_main_menu() {
	GDVIRTUAL_CALL_PTR(owner, _show_main_menu);
}

void JoinFlow::Device::route_novaworld_session(bool reenter) {
	GDVIRTUAL_CALL_PTR(owner, _route_novaworld_session, reenter);
}

void JoinFlow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("join", "target"), &JoinFlow::join);
	ClassDB::bind_method(D_METHOD("refuse", "text"), &JoinFlow::refuse);
	ClassDB::bind_method(
			D_METHOD("join_from_novaworld", "target", "rid"), &JoinFlow::join_from_novaworld);
	ClassDB::bind_method(D_METHOD("host_from_novaworld"), &JoinFlow::host_from_novaworld);
	ClassDB::bind_method(D_METHOD("launch_from_flags"), &JoinFlow::launch_from_flags);
	ClassDB::bind_method(D_METHOD("browse_rows", "rows", "serial"), &JoinFlow::browse_rows);
	ClassDB::bind_method(D_METHOD("browse_finished", "serial"), &JoinFlow::browse_finished);
	ClassDB::bind_method(D_METHOD("commit_choice", "game_password", "spectate", "team_password"),
			&JoinFlow::commit_choice);
	ClassDB::bind_method(D_METHOD("cancel"), &JoinFlow::cancel);
	ClassDB::bind_method(D_METHOD("join_failed", "error"), &JoinFlow::join_failed);
	ClassDB::bind_method(D_METHOD("take_return_to_novaworld"), &JoinFlow::take_return_to_novaworld);
	ClassDB::bind_method(D_METHOD("release_novaworld"), &JoinFlow::release_novaworld);
	ClassDB::bind_method(D_METHOD("is_join_choice_pending"), &JoinFlow::is_join_choice_pending);
	GDVIRTUAL_BIND(_begin_browse, "target", "serial");
	GDVIRTUAL_BIND(_adopt_browse, "target");
	GDVIRTUAL_BIND(_end_browse);
	GDVIRTUAL_BIND(_drain_browse);
	GDVIRTUAL_BIND(_drain_enumerator);
	GDVIRTUAL_BIND(_show_enumerating);
	GDVIRTUAL_BIND(_show_choice, "server_flags");
	GDVIRTUAL_BIND(_show_refusal, "text");
	GDVIRTUAL_BIND(_close_join_screen);
	GDVIRTUAL_BIND(_callsign);
	GDVIRTUAL_BIND(_switch_expansion, "expansion");
	GDVIRTUAL_BIND(_save_profile);
	GDVIRTUAL_BIND(_reload_profile);
	GDVIRTUAL_BIND(_save_config);
	GDVIRTUAL_BIND(_start_join, "target");
	GDVIRTUAL_BIND(_start_launch_host, "mission", "rotation", "max_players", "port", "game_type",
			"lan_mode");
	GDVIRTUAL_BIND(_cancel_pre_game);
	GDVIRTUAL_BIND(_show_main_menu);
	GDVIRTUAL_BIND(_route_novaworld_session, "reenter");
}

} // namespace godot
