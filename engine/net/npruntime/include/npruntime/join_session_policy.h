#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The joiner session-drive POLICY: the decisions the shell's net-session drive
// (godot/src/world/net_session_drive.gd) executes around a LAN join — the
// two ConnectOrHost wait windows, the S2C 0x7B promote validation, the
// post-load admission/deploy edge machine with its session-loss latch, and the
// D-NET-178 expansion reconcile decision. Pure state + text: no sockets, no
// clock (callers pass now_ms), no filesystem — the drive keeps every shell
// action (the preload sim pump, the mount switch, the settle call, signal
// emission) and executes exactly what these decisions say.

namespace opennova::np {

// The retail ConnectOrHost wait window, 0xEA60 = 60000 ms [orig:
// CNapiGameSession_ConnectOrHost @0x4d4f10 — both GetTickCount loops break on
// elapsed > 0xEA60 (playing @0x4d54d3) with an ESC(27) escape]. Retail applies
// it to the NovaWorld gate connect/host poll (engine/net/napi/session.h
// SESSION_CONNECT_TIMEOUT_MS is the same witnessed constant on that path);
// the two windows HERE are its documented reachable-analog reuse for the LAN
// join waits, which retail leaves untimed: NapiClient_WaitForGameStart
// @0x42cc10 discards its GetTickCount results and returns only on
// externally-set flags, and UI_JoinSelectedSession's post-connect poll
// @0x569e70 is state-machine-driven with no deadline
// (docs/net/novaworld-net-re.md §5.2).
inline constexpr uint32_t kJoinConnectWindowMs = 60000u;

// The exact S2C 0x0B mission-header size a joiner promotes into a world: the
// server copies its 0x268-byte header verbatim (D-NET-194,
// docs/net/novaworld-net-re.md §5.32).
inline constexpr std::size_t kJoinWireMissionHeaderBytes = 616u;

// ---------------------------------------------------------------------------
// The D-NET-178 expansion reconcile DECISION: host expansion x mounted
// expansion x installed set -> keep / remount / fail. Retail decides the same
// thing one step earlier, off the browse row's session record: it copies that
// record's expansion over the pending name and switches THE ONE global mount
// in place before it connects [orig: UI_JoinSelectedSession @0x5699d0
// (expansion copy @0x569afa, switch @0x569b02, connect @0x569ded) ->
// Expansion_SwitchTo @0x5688c0], then IGNORES the switch result — a missing
// expansion .pff fails the @0x568914 gate and retail connects on its own data
// set anyway, reproducing the ADM index-space corruption D-NET-178 records.
// We refuse that join instead (tracked divergence, deliberate). A LAN joiner
// has no browse row, so the drive runs this on S2C 0x7B field 7. All
// comparisons are case-insensitive: the wire carries the host's spelling and
// the installed set carries the local filesystem's; a remount target keeps
// the ON-DISK spelling.

enum class JoinExpansionAction : uint8_t {
	// The mounted root already holds the host's expansion.
	kKeep = 0,
	// Re-point the mounted root at `expansion` ("" = base game), in place —
	// retail switches the one global mount; there is no second mount object.
	kRemount = 1,
	// The host's expansion is not installed here; `error` says so and the
	// join must abort.
	kFail = 2,
};

struct JoinExpansionDecision {
	JoinExpansionAction action = JoinExpansionAction::kKeep;
	// The expansion to mount, in its on-disk spelling ("" = base game).
	std::string expansion;
	// Populated only for kFail: the join-failure reason shown to the player.
	std::string error;
};

// The installed set as it appears in a failure reason ("none — base game
// only" when empty). Kept beside the reason it feeds.
std::string describe_installed_expansions(const std::vector<std::string> &installed);

JoinExpansionDecision decide_join_expansion(std::string_view host_expansion,
		std::string_view mounted_expansion,
		const std::vector<std::string> &installed);

// ---------------------------------------------------------------------------
// The per-frame decisions of the two joiner waits. One instance per net
// session drive; the drive arms/disarms around its load legs and forwards the
// state it read from the simulation each frame.

enum class JoinPreloadStep : uint8_t {
	kWait = 0,  // keep pumping the preload
	kFail = 1,  // fail_reason() carries the load-failure text
};

// Admission-frame edge flags (begin_admission_frame / finish_admission_frame
// results). The drive maps each flag 1:1 onto a shell action or a GameWorld
// signal emission, in this order.
enum : uint32_t {
	// Stop this frame after begin: the loss latch suppresses every later
	// admission/deploy edge, or a terminal failure was decided.
	kAdmissionFrameDone = 1u << 0,
	// Emit session_lost(session_loss_reason()) — set exactly once per join.
	kAdmissionEmitSessionLost = 1u << 1,
	// Emit load_failed(fail_reason()) and stop.
	kAdmissionLoadFailed = 1u << 2,
	// begin: the frame needs the shell's settle_join_wire_assets() result
	// before finish_admission_frame.
	kAdmissionSettleRequired = 1u << 3,
	// finish: the settle failed; report fail_reason() through the wire-asset
	// failure leg and stop.
	kAdmissionSettleFailed = 1u << 4,
	// finish: emit join_deploy_pick_required (rising edge only; the latch
	// re-arms when the pending pick clears so a later death can reopen DEATH).
	kAdmissionEmitDeployPick = 1u << 5,
	// finish: emit join_admission_ready (once per join).
	kAdmissionEmitReady = 1u << 6,
};

class JoinSessionPolicy {
public:
	// -- the pre-load connect/session window ---------------------------------
	// Armed by the joiner load entry; the drive pumps the preload sim while
	// preload_step says kWait. The deadline is the retail ConnectOrHost
	// window (kJoinConnectWindowMs) from arm time.
	void arm_preload(uint64_t now_ms);
	void disarm_preload();
	bool preload_armed() const { return preload_armed_; }
	// One not-yet-ready preload frame, AFTER the drive polled the sim:
	// a non-empty join error fails first, then the window.
	JoinPreloadStep preload_step(std::string_view join_error, uint64_t now_ms);

	// -- the S2C 0x7B -> world promote validation ----------------------------
	// The advertised map_file: trimmed, must be non-empty, ".bms" appended
	// case-insensitively when absent (promoted_mission_file() carries the
	// result). The joined mission never opens the local .bms (D-NET-194);
	// the name keys mission/text-table naming only.
	bool validate_promote_mission_file(std::string_view mission_file);
	const std::string &promoted_mission_file() const { return promoted_mission_file_; }
	// The wire header must be exactly kJoinWireMissionHeaderBytes.
	bool validate_promote_header(std::size_t header_size);

	// -- the post-load admission watchdog + admission/deploy/loss edges ------
	// Post-load joiner watchdog: the admission tail (C2S 0x0A -> world stream
	// -> loadout grants) is server-driven with no retail timeout — see the
	// kJoinConnectWindowMs witness note — so a stalled or incompatible host
	// would leave the player loaded but hidden forever. The window covers only
	// SERVER-owed transitions: once the join reaches the player-paced
	// deployment pick the watchdog ends (retail's DEATH screen simply waits;
	// net-re §5.61), while deploy/loss observation continues for the session.
	void arm_admission_watch(uint64_t now_ms);
	void disarm_admission_watch();
	bool admission_watch_active() const { return watch_active_; }
	// ESC/abort for the admission wait (the second interruptible joiner wait —
	// as reachable as retail's ESC leg in the wait loop [orig:
	// NapiClient_WaitForGameStart @0x42cc10 return 3;
	// Client_CheckDisconnectOrEscDuringLoad @0x520270]). Returns true when a
	// live watch was told to abort; the next frame reports the failure.
	bool request_admission_abort();

	// One observed frame, split so the shell's settle call happens between the
	// two halves at exactly the witnessed point in the ordering: terminal
	// state first (the loss latch wins over every admission edge — the
	// initial-admission predicate is monotonic and reading it first could
	// reveal the world in the same tick the loss leg tears it down), then the
	// abort, then settle, then the window, then the edges.
	uint32_t begin_admission_frame(std::string_view session_loss_reason,
			bool deploy_pending, bool initial_admission_complete,
			std::string_view join_error, std::string_view admission_stage,
			uint64_t now_ms);
	// finish is only called when begin did not set kAdmissionFrameDone;
	// settle_ok is meaningful when begin set kAdmissionSettleRequired.
	uint32_t finish_admission_frame(bool settle_ok, bool wire_present_drained);

	// -- latches + composed text --------------------------------------------
	// One reset per join attempt (the drive's joiner load entry): the
	// admission-ready / deploy-signal / session-loss emission latches.
	void reset_for_join();
	// Full teardown (the drive's reset()).
	void reset();

	const std::string &fail_reason() const { return fail_reason_; }
	const std::string &session_loss_reason() const { return session_loss_reason_; }

private:
	// pre-load window
	bool preload_armed_ = false;
	uint64_t preload_deadline_ms_ = 0;
	// admission watchdog
	bool watch_active_ = false;
	bool watch_abort_ = false;
	uint64_t admission_deadline_ms_ = 0;
	// begin -> finish frame carry
	bool frame_deploy_pending_ = false;
	bool frame_initial_complete_ = false;
	std::string frame_join_error_;
	std::string frame_admission_stage_;
	uint64_t frame_now_ms_ = 0;
	// emission latches (edges, not per-frame state reports)
	bool ready_emitted_ = false;
	bool deploy_signal_active_ = false;
	bool session_lost_emitted_ = false;
	std::string session_loss_reason_;
	std::string fail_reason_;
	std::string promoted_mission_file_;
};

} // namespace opennova::np
