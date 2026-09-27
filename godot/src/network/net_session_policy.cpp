#include "network/net_session_policy.h"
#include "util/string_convert.h"

#include <string>
#include <vector>

namespace godot {

using opennova::to_std;

int NetSessionPolicy::decide_expansion(const String &p_host_expansion,
		const String &p_mounted_expansion, const PackedStringArray &p_installed) {
	std::vector<std::string> installed;
	installed.reserve(static_cast<size_t>(p_installed.size()));
	for (int i = 0; i < p_installed.size(); ++i) {
		installed.push_back(opennova::to_std(p_installed[i]));
	}
	decision_ = opennova::inmatch::decide_join_expansion(
			opennova::to_std(p_host_expansion), opennova::to_std(p_mounted_expansion), installed);
	return static_cast<int>(decision_.action);
}

String NetSessionPolicy::decided_expansion() const {
	return opennova::to_gd(decision_.expansion);
}

String NetSessionPolicy::decision_error() const {
	return opennova::to_gd(decision_.error);
}

String NetSessionPolicy::describe_installed(const PackedStringArray &p_installed) {
	std::vector<std::string> installed;
	installed.reserve(static_cast<size_t>(p_installed.size()));
	for (int i = 0; i < p_installed.size(); ++i) {
		installed.push_back(opennova::to_std(p_installed[i]));
	}
	return opennova::to_gd(opennova::inmatch::describe_installed_expansions(installed));
}

void NetSessionPolicy::arm_preload(int64_t p_now_ms) {
	policy_.arm_preload(static_cast<uint64_t>(p_now_ms));
}

void NetSessionPolicy::disarm_preload() {
	policy_.disarm_preload();
}

int NetSessionPolicy::preload_step(const String &p_join_error, int64_t p_now_ms) {
	return static_cast<int>(policy_.preload_step(
			opennova::to_std(p_join_error), static_cast<uint64_t>(p_now_ms)));
}

bool NetSessionPolicy::validate_promote_mission_file(const String &p_mission_file) {
	return policy_.validate_promote_mission_file(opennova::to_std(p_mission_file));
}

String NetSessionPolicy::promoted_mission_file() const {
	return opennova::to_gd(policy_.promoted_mission_file());
}

bool NetSessionPolicy::validate_promote_header(int64_t p_header_size) {
	return policy_.validate_promote_header(
			p_header_size < 0 ? 0u : static_cast<std::size_t>(p_header_size));
}

void NetSessionPolicy::arm_admission_watch(int64_t p_now_ms) {
	policy_.arm_admission_watch(static_cast<uint64_t>(p_now_ms));
}

void NetSessionPolicy::disarm_admission_watch() {
	policy_.disarm_admission_watch();
}

bool NetSessionPolicy::is_admission_watch_active() const {
	return policy_.admission_watch_active();
}

bool NetSessionPolicy::request_admission_abort() {
	return policy_.request_admission_abort();
}

int NetSessionPolicy::begin_admission_frame(const String &p_session_loss_reason,
		bool p_deploy_pending, bool p_initial_admission_complete,
		const String &p_join_error, const String &p_admission_stage,
		int64_t p_now_ms) {
	return static_cast<int>(policy_.begin_admission_frame(
			opennova::to_std(p_session_loss_reason), p_deploy_pending,
			p_initial_admission_complete, opennova::to_std(p_join_error),
			opennova::to_std(p_admission_stage), static_cast<uint64_t>(p_now_ms)));
}

int NetSessionPolicy::finish_admission_frame(bool p_settle_ok,
		bool p_wire_present_drained) {
	return static_cast<int>(
			policy_.finish_admission_frame(p_settle_ok, p_wire_present_drained));
}

String NetSessionPolicy::fail_reason() const {
	return opennova::to_gd(policy_.fail_reason());
}

String NetSessionPolicy::session_loss_reason() const {
	return opennova::to_gd(policy_.session_loss_reason());
}

void NetSessionPolicy::reset_for_join() {
	policy_.reset_for_join();
}

void NetSessionPolicy::reset() {
	policy_.reset();
}

void NetSessionPolicy::_bind_methods() {
	ClassDB::bind_method(D_METHOD("decide_expansion", "host_expansion", "mounted_expansion", "installed"),
			&NetSessionPolicy::decide_expansion);
	ClassDB::bind_method(D_METHOD("decided_expansion"), &NetSessionPolicy::decided_expansion);
	ClassDB::bind_method(D_METHOD("decision_error"), &NetSessionPolicy::decision_error);
	ClassDB::bind_static_method("NetSessionPolicy",
			D_METHOD("describe_installed", "installed"),
			&NetSessionPolicy::describe_installed);
	ClassDB::bind_method(D_METHOD("arm_preload", "now_ms"), &NetSessionPolicy::arm_preload);
	ClassDB::bind_method(D_METHOD("preload_step", "join_error", "now_ms"),
			&NetSessionPolicy::preload_step);
	ClassDB::bind_method(D_METHOD("validate_promote_mission_file", "mission_file"),
			&NetSessionPolicy::validate_promote_mission_file);
	ClassDB::bind_method(D_METHOD("promoted_mission_file"),
			&NetSessionPolicy::promoted_mission_file);
	ClassDB::bind_method(D_METHOD("validate_promote_header", "header_size"),
			&NetSessionPolicy::validate_promote_header);
	ClassDB::bind_method(D_METHOD("arm_admission_watch", "now_ms"),
			&NetSessionPolicy::arm_admission_watch);
	ClassDB::bind_method(D_METHOD("is_admission_watch_active"),
			&NetSessionPolicy::is_admission_watch_active);
	ClassDB::bind_method(D_METHOD("begin_admission_frame", "session_loss_reason",
								 "deploy_pending", "initial_admission_complete",
								 "join_error", "admission_stage", "now_ms"),
			&NetSessionPolicy::begin_admission_frame);
	ClassDB::bind_method(D_METHOD("finish_admission_frame", "settle_ok", "wire_present_drained"),
			&NetSessionPolicy::finish_admission_frame);
	ClassDB::bind_method(D_METHOD("fail_reason"), &NetSessionPolicy::fail_reason);
	ClassDB::bind_method(D_METHOD("session_loss_reason"),
			&NetSessionPolicy::session_loss_reason);
	ClassDB::bind_method(D_METHOD("reset_for_join"), &NetSessionPolicy::reset_for_join);
	ClassDB::bind_method(D_METHOD("reset"), &NetSessionPolicy::reset);

	BIND_CONSTANT(ACTION_KEEP);
	BIND_CONSTANT(ACTION_REMOUNT);
	BIND_CONSTANT(ACTION_FAIL);
	BIND_CONSTANT(STEP_WAIT);
	BIND_CONSTANT(STEP_FAIL);
	BIND_CONSTANT(FRAME_DONE);
	BIND_CONSTANT(EMIT_SESSION_LOST);
	BIND_CONSTANT(LOAD_FAILED);
	BIND_CONSTANT(SETTLE_REQUIRED);
	BIND_CONSTANT(SETTLE_FAILED);
	BIND_CONSTANT(EMIT_DEPLOY_PICK);
	BIND_CONSTANT(EMIT_ADMISSION_READY);
}

} // namespace godot
