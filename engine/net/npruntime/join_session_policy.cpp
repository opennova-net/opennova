#include "npruntime/join_session_policy.h"

#include <io/strutil.h>

namespace opennova::np {

std::string describe_installed_expansions(const std::vector<std::string> &installed) {
	if (installed.empty()) {
		// "none <em dash> base game only" — the em dash spelled as UTF-8 bytes so
		// the literal survives every source-charset configuration.
		return "none \xE2\x80\x94 base game only";
	}
	std::string out;
	for (const std::string &name : installed) {
		if (!out.empty()) out += ", ";
		out += name;
	}
	return out;
}

JoinExpansionDecision decide_join_expansion(std::string_view host_expansion,
		std::string_view mounted_expansion,
		const std::vector<std::string> &installed) {
	JoinExpansionDecision plan;
	const std::string host = strutil::trim(host_expansion);
	const std::string mounted = strutil::trim(mounted_expansion);
	if (strutil::iequals(host, mounted)) {
		plan.action = JoinExpansionAction::kKeep;
		plan.expansion = mounted;
		return plan;
	}
	// A BASE-GAME host while an expansion is mounted locally is exactly as
	// corrupting as the reverse, and base game is always mountable — no
	// installed-set lookup applies.
	if (host.empty()) {
		plan.action = JoinExpansionAction::kRemount;
		return plan;
	}
	for (const std::string &name : installed) {
		if (strutil::iequals(strutil::trim(name), host)) {
			plan.action = JoinExpansionAction::kRemount;
			plan.expansion = name;
			return plan;
		}
	}
	plan.action = JoinExpansionAction::kFail;
	plan.error = "join: host runs expansion '" + host +
			"' which is not installed (installed: " +
			describe_installed_expansions(installed) + ")";
	return plan;
}

// ---------------------------------------------------------------------------

void JoinSessionPolicy::arm_preload(uint64_t now_ms) {
	preload_armed_ = true;
	preload_deadline_ms_ = now_ms + kJoinConnectWindowMs;
}

void JoinSessionPolicy::disarm_preload() {
	preload_armed_ = false;
	preload_deadline_ms_ = 0;
}

JoinPreloadStep JoinSessionPolicy::preload_step(std::string_view join_error,
		uint64_t now_ms) {
	if (!join_error.empty()) {
		fail_reason_ = "join failed: ";
		fail_reason_ += join_error;
		return JoinPreloadStep::kFail;
	}
	if (preload_armed_ && now_ms >= preload_deadline_ms_) {
		fail_reason_ = "join timed out before the host completed preload admission";
		return JoinPreloadStep::kFail;
	}
	return JoinPreloadStep::kWait;
}

bool JoinSessionPolicy::validate_promote_mission_file(std::string_view mission_file) {
	std::string bms = strutil::trim(mission_file);
	if (bms.empty()) {
		fail_reason_ = "join: host sent an empty map_file in S2C 0x7B";
		return false;
	}
	if (!strutil::ends_with_icase(bms, ".bms")) bms += ".bms";
	promoted_mission_file_ = bms;
	return true;
}

bool JoinSessionPolicy::validate_promote_header(std::size_t header_size) {
	if (header_size != kJoinWireMissionHeaderBytes) {
		fail_reason_ =
				"join: host did not provide an exact 616-byte S2C 0x0B mission header";
		return false;
	}
	return true;
}

void JoinSessionPolicy::arm_admission_watch(uint64_t now_ms) {
	watch_active_ = true;
	watch_abort_ = false;
	admission_deadline_ms_ = now_ms + kJoinConnectWindowMs;
}

void JoinSessionPolicy::disarm_admission_watch() {
	watch_active_ = false;
	watch_abort_ = false;
	admission_deadline_ms_ = 0;
}

bool JoinSessionPolicy::request_admission_abort() {
	if (!watch_active_) return false;
	watch_abort_ = true;
	return true;
}

uint32_t JoinSessionPolicy::begin_admission_frame(std::string_view session_loss_reason,
		bool deploy_pending, bool initial_admission_complete,
		std::string_view join_error, std::string_view admission_stage,
		uint64_t now_ms) {
	// Terminal state wins over every admission edge: the initial-admission
	// predicate is monotonic, so reading it first could reveal the world in
	// the same tick the session-loss leg tears it down. One notification per
	// session; the latched reason keeps suppressing every later edge.
	if (!session_loss_reason.empty()) {
		uint32_t flags = kAdmissionFrameDone;
		if (!session_lost_emitted_) {
			disarm_admission_watch();
			session_lost_emitted_ = true;
			session_loss_reason_ = std::string(session_loss_reason);
			flags |= kAdmissionEmitSessionLost;
		}
		return flags;
	}
	frame_deploy_pending_ = deploy_pending;
	frame_initial_complete_ = initial_admission_complete;
	frame_join_error_ = std::string(join_error);
	frame_admission_stage_ = std::string(admission_stage);
	frame_now_ms_ = now_ms;
	if (watch_active_ && watch_abort_) {
		disarm_admission_watch();
		// The reachable ESC return of the retail wait loop [orig:
		// NapiClient_WaitForGameStart @0x42cc10 return 3;
		// Client_CheckDisconnectOrEscDuringLoad @0x520270].
		fail_reason_ = "Mission loading aborted";
		return kAdmissionFrameDone | kAdmissionLoadFailed;
	}
	// The revealed world must settle the host's streamed assets first; the
	// shell performs the settle between the two frame halves.
	if (frame_deploy_pending_ || frame_initial_complete_) return kAdmissionSettleRequired;
	return 0;
}

uint32_t JoinSessionPolicy::finish_admission_frame(bool settle_ok,
		bool wire_present_drained) {
	if ((frame_deploy_pending_ || frame_initial_complete_) && !settle_ok) {
		disarm_admission_watch();
		fail_reason_ = "join: failed to settle the host's streamed mission assets";
		return kAdmissionSettleFailed;
	}
	uint32_t flags = 0;
	// The no-pick reveal holds — with its watchdog deadline still armed —
	// until the presenter's cold wire drain empties behind the loading hold.
	// A deploy pick disarms immediately; the DEATH screen is itself the hold.
	if (frame_deploy_pending_ || (frame_initial_complete_ && wire_present_drained)) {
		disarm_admission_watch();
	} else if (watch_active_) {
		if (!frame_join_error_.empty()) {
			disarm_admission_watch();
			fail_reason_ = "join failed: " + frame_join_error_;
			return kAdmissionLoadFailed;
		}
		if (frame_now_ms_ >= admission_deadline_ms_) {
			fail_reason_ =
					"join stalled waiting for the host (" + frame_admission_stage_ + ")";
			disarm_admission_watch();
			return kAdmissionLoadFailed;
		}
	}
	// Admission and deploy notifications are edges, not per-frame state
	// reports. The deploy latch releases when pending clears so a later death
	// can reopen DEATH.
	if (frame_deploy_pending_) {
		if (!deploy_signal_active_) {
			deploy_signal_active_ = true;
			flags |= kAdmissionEmitDeployPick;
		}
	} else {
		deploy_signal_active_ = false;
	}
	// A player-paced pick replaces loading with DEATH; it is deliberately not
	// also an admitted-world edge. After the pick releases, the monotonic
	// initial predicate emits admission-ready once with deploy clear — and
	// only once the cold wire drain has emptied behind the hold.
	if (frame_initial_complete_ && !frame_deploy_pending_ && wire_present_drained) {
		if (!ready_emitted_) {
			ready_emitted_ = true;
			flags |= kAdmissionEmitReady;
		}
	}
	return flags;
}

void JoinSessionPolicy::reset_for_join() {
	ready_emitted_ = false;
	deploy_signal_active_ = false;
	session_lost_emitted_ = false;
	session_loss_reason_.clear();
	fail_reason_.clear();
}

void JoinSessionPolicy::reset() {
	disarm_preload();
	disarm_admission_watch();
	reset_for_join();
	frame_deploy_pending_ = false;
	frame_initial_complete_ = false;
	frame_join_error_.clear();
	frame_admission_stage_.clear();
	frame_now_ms_ = 0;
	promoted_mission_file_.clear();
}

} // namespace opennova::np
