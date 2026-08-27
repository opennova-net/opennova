#pragma once

#include <net/npruntime/join_session_policy.h>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Thin wrapper over np::JoinSessionPolicy + np::decide_join_expansion — the
// joiner session-drive policy (the two 0xEA60 reachable-analog windows, the
// S2C 0x7B promote validation, the admission/deploy/loss edge machine, the
// D-NET-178 expansion reconcile decision). One instance per NetSessionDrive;
// the drive reads simulation state, forwards it here, and executes exactly
// what the returned flags say (signal emission, the settle call, the mount
// switch). Every decision, window, latch, and reason text lives in
// engine/net/npruntime/join_session_policy.cpp.
class NetSessionPolicy : public RefCounted {
	GDCLASS(NetSessionPolicy, RefCounted)

public:
	// np::JoinExpansionAction.
	enum { ACTION_KEEP = 0, ACTION_REMOUNT = 1, ACTION_FAIL = 2 };
	// np::JoinPreloadStep.
	enum { STEP_WAIT = 0, STEP_FAIL = 1 };
	// The admission-frame edge flags (np::kAdmission*).
	enum {
		FRAME_DONE = 1 << 0,
		EMIT_SESSION_LOST = 1 << 1,
		LOAD_FAILED = 1 << 2,
		SETTLE_REQUIRED = 1 << 3,
		SETTLE_FAILED = 1 << 4,
		EMIT_DEPLOY_PICK = 1 << 5,
		EMIT_ADMISSION_READY = 1 << 6,
	};

	int decide_expansion(const String &p_host_expansion,
			const String &p_mounted_expansion, const PackedStringArray &p_installed);
	String decided_expansion() const;
	String decision_error() const;
	// The installed set as it reads in an abort reason ("none — base game
	// only" when empty), shared with the decision leg (one impl).
	static String describe_installed(const PackedStringArray &p_installed);

	void arm_preload(int64_t p_now_ms);
	void disarm_preload();
	int preload_step(const String &p_join_error, int64_t p_now_ms);

	bool validate_promote_mission_file(const String &p_mission_file);
	String promoted_mission_file() const;
	bool validate_promote_header(int64_t p_header_size);

	void arm_admission_watch(int64_t p_now_ms);
	void disarm_admission_watch();
	bool is_admission_watch_active() const;
	bool request_admission_abort();
	int begin_admission_frame(const String &p_session_loss_reason,
			bool p_deploy_pending, bool p_initial_admission_complete,
			const String &p_join_error, const String &p_admission_stage,
			int64_t p_now_ms);
	int finish_admission_frame(bool p_settle_ok, bool p_wire_present_drained);

	String fail_reason() const;
	String session_loss_reason() const;
	void reset_for_join();
	void reset();

protected:
	static void _bind_methods();

private:
	opennova::np::JoinSessionPolicy policy_;
	opennova::np::JoinExpansionDecision decision_;
};

} // namespace godot
