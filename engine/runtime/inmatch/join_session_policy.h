#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The joiner session-drive POLICY: the decisions the shell's net-session drive
// (godot/src/world/session_drive.cpp) executes around a LAN join: the
// preload's failure edge, the S2C 0x7B promote validation, the post-load
// admission/deploy edge machine with its session-loss latch, and the
// D-NET-178 expansion reconcile decision. Pure state + text: no sockets, no
// clock, no filesystem: the drive keeps every shell action (the preload sim
// pump, the mount switch, the settle call, signal emission) and executes
// exactly what these decisions say.
//
// No join wait has a deadline of its own, as retail's have none. The join
// screen waits in the host's queue (the WAIT* line off the S2C 0x03 record)
// until the S2C 0x05, a lost session or Cancel: its only clock reads are the
// NovaWorld gate ping's and the queue line's elapsed time. The mission start's
// waits after the 0x05 return on the round's end, the disconnect flag, ESC or
// their own record, never on time. A dead host still ends the join, through the
// connection's own legs: the 0x42's 30 s connect window (D-NET-231) and the
// connected state's silence reap (D-NET-177). Retail's one 60 s window is the
// NovaWorld gate's play / host poll (engine/net/napi/session.h), which no LAN join
// runs. (D-NET-1070: ours reused that window for every join wait, so a join
// the host held in its queue failed at 60 s with its goodbyes where retail's waits
// on.)
// [orig: MultiPlayer_JoinSessionStateMachine @0x56A320 - states 5 / 6 leave on
//  CNapiNetwork_IsSessionActive @0x56A5E7 / @0x56A6DF / @0x56A758 or the abort
//  @0x56A59C / @0x56A67F, GetTickCount only @0x56A4E8 (the gate ping) and @0x56A7A2
//  (the queue line's WAITTIMEDISPLAY); NapiClient_WaitForGameStart @0x42CC10 - its
//  GetTickCount results @0x42CC4A / @0x42CC5D discarded, the returns on g_RoundOver
//  @0x42CCA7, g_NetDisconnectedFlag @0x42CCA9 (the disconnect's flag,
//  CNapiNetwork_OnDisconnectedFromServer @0x4C64A4), g_LoadingCancelFlag @0x42CCB1 and ESC
//  @0x42CC77; Game_StartMission @0x524360 - the final wait @0x526295..0x5262E7 leaves on the
//  world state @0x5262DF, the cancel flag @0x526295, ESC @0x5262AB or the disconnect flag
//  @0x5262CD, with no clock read; SaveFile_SendAndWaitForServerAck @0x5204B0 (@0x5205B7) and
//  NapiClient_SendPlayerSyncAndWaitForSyncTail @0x42CB20 (@0x42CBB5) read the clock for their
//  50 / 100 ms send cadence alone; CNapiGameSession_InitRandomSeedOrRequest @0x51E8F0 - the
//  mission-data wait @0x51EA18..0x51EA62, no clock read; CNapiGameSession_ConnectOrHost
//  @0x4D4F10 - `cmp eax, 0EA60h` @0x4D54CE / `ja` @0x4D54D3, the gate's play poll]

namespace opennova::inmatch {

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
// Expansion_SwitchTo @0x5688c0 -> PFF_CloseAllOpenArchives @0x4a4380 /
// PFF_OpenAllArchives @0x4a4310, no second mount object], then IGNORES the
// switch result — a missing expansion .pff fails the @0x568914 gate and
// retail connects on its own data
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
// session drive; the drive arms the admission watch at its load's end and
// forwards the state it read from the simulation each frame.

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
	// -- the pre-load waits ----------------------------------------------------
	// One not-yet-ready preload frame, AFTER the drive polled the sim: a non-empty
	// join error fails it; anything else waits on, however long the host holds the
	// join (no deadline: see the witness at the top of this file).
	JoinPreloadStep preload_step(std::string_view join_error);

	// -- the S2C 0x7B -> world promote validation ----------------------------
	// The advertised map_file: trimmed, must be non-empty, ".bms" appended
	// case-insensitively when absent (promoted_mission_file() carries the
	// result). The joined mission never opens the local .bms (D-NET-194);
	// the name keys mission/text-table naming only.
	bool validate_promote_mission_file(std::string_view mission_file);
	const std::string &promoted_mission_file() const { return promoted_mission_file_; }
	// The wire header must be exactly kJoinWireMissionHeaderBytes.
	bool validate_promote_header(std::size_t header_size);

	// -- the post-load admission watch + admission/deploy/loss edges ---------
	// Post-load joiner watch: the admission tail (the mission start's loadout and
	// status -> the grants -> the S2C 0x0F) is server-driven, and a join error the
	// tail latches fails the join. It has no deadline, as retail's waits have none
	// (the witness at the top of this file): a stalled host is ended by the session's
	// loss legs, else the player waits. It covers only SERVER-owed transitions: once
	// the join reaches the player-paced deployment pick the watch ends (retail's
	// DEATH screen simply waits; net-re §5.61), while deploy/loss observation
	// continues for the session.
	void arm_admission_watch();
	void disarm_admission_watch();
	bool admission_watch_active() const { return watch_active_; }
	// The watch takes no ESC. The load's ESC legs run inside the mission start's one
	// synchronous call, where no key reaches the drive; the final wait's ESC only leaves
	// that wait (SessionDrive::cancel_admission_wait); after the S2C 0x0F the game loop
	// runs, and its ESC is the game loop's, while the shell's cold-spawn hold still shows
	// the loading screen (D-NET-1030).
	// [orig: Client_CheckDisconnectOrEscDuringLoad @0x520270 - the ESC's reason 1
	//  @0x5202A8, its calls in Game_StartMission @0x52555B / @0x525910 / @0x525A1B; the
	//  final wait's ESC @0x5262AB -> @0x5262F3, "Mission loading complete" @0x5262FD]

	// One observed frame, split so the shell's settle call happens between the
	// two halves at exactly the witnessed point in the ordering: terminal
	// state first (the loss latch wins over every admission edge — the
	// initial-admission predicate is monotonic and reading it first could
	// reveal the world in the same tick the loss leg tears it down), then
	// settle, then the watch's join error, then the edges.
	uint32_t begin_admission_frame(std::string_view session_loss_reason,
			bool deploy_pending, bool initial_admission_complete,
			std::string_view join_error);
	// finish is only called when begin did not set kAdmissionFrameDone;
	// settle_ok is meaningful when begin set kAdmissionSettleRequired.
	uint32_t finish_admission_frame(bool settle_ok, bool wire_present_drained);

	// -- latches + composed text --------------------------------------------
	// One reset per join attempt (the drive's joiner load entry): the
	// admission-ready / deploy-signal / session-loss emission latches.
	void reset_for_join();
	// Full teardown (the drive's reset()).
	void reset();

	// The failure this join reported (empty while none; cleared by reset_for_join). A join
	// that failed left its mission start: a failing leg stores exit 1 and leaves
	// Game_StartMission ahead of its final wait, so the drive has no final wait for a later
	// ESC to leave (SessionDrive::cancel_admission_wait).
	// [orig: Client_CheckDisconnectOrEscDuringLoad @0x520270 - reason 1 @0x5202A8;
	//  Game_StartMission @0x524360 - the final wait @0x526295]
	const std::string &fail_reason() const { return fail_reason_; }
	const std::string &session_loss_reason() const { return session_loss_reason_; }

private:
	// admission watch
	bool watch_active_ = false;
	// begin -> finish frame carry
	bool frame_deploy_pending_ = false;
	bool frame_initial_complete_ = false;
	std::string frame_join_error_;
	// emission latches (edges, not per-frame state reports)
	bool ready_emitted_ = false;
	bool deploy_signal_active_ = false;
	bool session_lost_emitted_ = false;
	std::string session_loss_reason_;
	std::string fail_reason_;
	std::string promoted_mission_file_;
};

} // namespace opennova::inmatch
