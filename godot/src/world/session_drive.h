#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "mission/mission_setup_options.h"
#include "network/host_session_options.h"
#include "network/join_target.h"
#include "network/net_session_policy.h"
#include "object/character_join_profile.h"
#include "player/player_spawn_loadout.h"
#include "network/join_screen_status.h"
#include "world/post_mission_route.h"
#include "resource_index/resource_root.h"
#include "simulation/simulation.h"

#include <map>
#include <string>

namespace opennova {
class NwuHostRole;
} // namespace opennova

namespace godot {

class GameWorld;
class MissionRoot;
class NovaWorldClient;

// The drive between a typed session request and an admitted world -- the
// engine side of a net-session load (the former net_session_drive.gd, ADR
// 0043 slice G10), a plain member of the C++ GameWorld: request staging
// (HostSessionOptions / JoinTarget, ADR 0017), the authenticated-before-load
// joiner preload sim, the S2C 0x7B promote, the expansion reconcile
// (D-NET-178), the post-load admission watchdog + per-frame
// admission/deploy/loss observer, the session-loss latch, the ESC aborts, and
// NovaWorld gate registration for a browsable listen host.
//
// The POLICY of all of that -- the two ConnectOrHost windows (0xEA60), the
// promote validation, the admission/deploy/loss edge machine with its latches,
// and the expansion reconcile DECISION -- is native
// (engine/runtime/inmatch/join_session_policy.cpp, bound as
// NetSessionPolicy). This drive keeps the lifetime: it reads simulation state,
// forwards it to the policy each frame, and executes exactly what the returned
// edges say -- signal emission, the settle call, the preload sim pump, and the
// in-place mount switch.
//
// No menu/env knowledge lives here (the shell's NetSessionController owns
// the entries; MainGame owns presentation). The session signals live on the
// world -- the shell contract pins them there -- so this drive emits them
// through its world. Both waits are synchronous state machines: the world's
// own _process (switched on ONLY while a joiner preload is pending) steps
// pre-load admission, and the world tick steps post-load admission.
class SessionDrive {
public:
	explicit SessionDrive(GameWorld *p_world);

	// Load a mission as a LAN co-op HOST. Same load path as the world's
	// load_mission, but the runtime starts the in-process listen server (ADR
	// 0011) bound to a real socket transport and, on the NovaWorld channel,
	// hosted by the NovaWorld session the shell handed over. `options` is the sim-shaped session request
	// every host producer projects (ADR 0017): the mp.mnu host screen, the
	// NovaWorld panel, and the --lan-host launch flag. Returns the same codes
	// as load_mission.
	int load_as_host(const Ref<HostSessionOptions> &p_options);
	// Load as a LAN co-op JOINER (a non-authority client). Every endpoint
	// authenticates first, learns map_file from S2C 0x7B, and builds from the
	// host's exact S2C 0x0B header + world stream on the SAME socket.
	// `target.mission` is browse/debug display metadata only; it can never
	// bypass the retail wire-driven load (D-NET-194). `target.player_name`
	// rides ClientAuth and is echoed in our organic-spawn record.
	int load_as_joiner(const Ref<JoinTarget> &p_target);
	// Drive one frame of the witnessed pre-world connect/session exchange
	// while the loading screen is visible (the world's _process while a
	// preload is pending). The deadline is the retail ConnectOrHost window
	// (0xEA60; the policy owns it). Synchronous on purpose.
	void step_preload();
	bool is_preload_pending() const { return join_preload_sim_.is_valid(); }
	// ESC/abort for the only interruptible load leg: the joiner's pre-load
	// connect/session wait (the SP/host load remains one synchronous call the
	// SceneTree cannot interrupt). Returns true when an in-flight preload was
	// aborted; the ordinary load-failure leg reports it to the shell [orig:
	// the "Mission loading aborted" early return of
	// Client_CheckDisconnectOrEscDuringLoad @ 0x520270]
	// (docs/interface/loading-screen-re.md D-LOADSCR-7).
	bool cancel_preload();
	// ESC/abort for the SECOND interruptible joiner wait: the post-load
	// admission tail, where the map is loaded but the world stays hidden until
	// the host drives the join to its deploy pick or in-match edge.
	// D-LOADSCR-7's "single synchronous operation.call()" reasoning covers the
	// host/SP map load, NOT this one -- the admission state machine is polled
	// once per world tick, so the ESC window is as reachable here as it is in
	// the pre-load connect wait. Without this a player who joins a host that
	// stalls after the wire-header world load has no way out for the full
	// ConnectOrHost window. Returns true when a live admission wait was told
	// to abort; the watchdog reports it through the ordinary load-failure leg.
	bool cancel_admission_wait();
	// True while the staged host request is a DEDICATED serve (no local-player
	// spawn, ADR 0015 serve mode); read by the world's playable gate before
	// stage_runtime_options consumes the request.
	bool pending_dedicated() const;
	// Stamp the staged session request onto the runtime's typed options
	// record, then clear the staging: the typed record
	// (host_session/join_target), the net-transport/gate fields derived from
	// it, and the preload-sim surrender -- the sim reference moves into
	// opts.simulation and is cleared here so MissionRoot remains the one
	// adopter (ADR 0011/0012 ownership stays singular). Called once per load
	// by the world's start_runtime; opts must already carry resource_root.
	void stage_runtime_options(const Ref<MissionSetupOptions> &p_opts);
	// Post-runtime-start hook, called by the world once its runtime is live:
	// a hosting NovaWorld session meets its match (bind_nw_host). (The
	// joiner's admission watchdog arms inside this drive's own load entries,
	// not here.)
	void on_runtime_started(const Ref<MissionSetupOptions> &p_opts, const String &p_bms_name);
	// Per-frame observer, called from the world's tick after the runtime
	// ticked: the admission/deploy/session-loss edges plus the gate's
	// advertised occupancy.
	void observe_tick(MissionRoot *p_runtime);
	// One teardown for everything this drive staged or stood up, called from
	// the world's unload(): the preload sim/root, the policy's windows +
	// notification latches, the typed request staging, and the NovaWorld
	// session unless the shell took it back first.
	void reset();
	// The NovaWorld session (a NovaWorldClient), handed over by the shell with a
	// joiner or host load: retail keeps the NWU session playing or hosting
	// through the match (the N icon's input, the NovaWorld exit, the hosted
	// match's registration), so the node moves under the world and lives until
	// the shell takes it back (release_nw_client) or reset() stops it.
	void adopt_nw_client(NovaWorldClient *p_client);
	// The shell's normal exit back to the NovaWorld menu takes the session back
	// out of the world, still connected (null when none was adopted).
	NovaWorldClient *release_nw_client();
	// The post-mission router's verdict for an exit reason (inmatch::route_mission_exit) on
	// this world's session; a NovaWorld network type rides an adopted NovaWorld session.
	Ref<PostMissionRoute> post_mission_route(int p_reason) const;
	// The error record of the join that last failed (null until one did): the preload's
	// refusal or timeout, or the admission tail's loss.
	Ref<ConnectionError> last_connection_error() const { return last_connection_error_; }
	// Where the join under the join screen stands: the preload's status from the
	// dial until the session is identified (null when no join preload runs).
	Ref<JoinScreenStatus> join_screen_status() const;
	// Switch `p_root` in place to the host's expansion (keep / remount / fail,
	// inmatch::decide_join_expansion): the post-auth reconcile and the pre-dial
	// leg (GameWorld::mount_join_expansion) share it. Returns the failure text,
	// empty on success.
	String switch_join_expansion(const Ref<ResourceRoot> &p_root, const String &p_host_expansion);

	// The bound signal targets of the hosting session (the world forwards).
	// A ServerCommand from the NovaWorld service: run it on the in-match host,
	// then the two shell legs of its outcome -- leave the hosting when the
	// service punted the host's own slot, republish the changed server name /
	// message columns on the next refresh.
	void on_nw_host_server_command(const String &p_verb, const String &p_target,
			const PackedStringArray &p_args);
	// The service's answer for a joiner the in-match host announced: success
	// releases it to the spawn pump, a failure punts it with the MsgCode.
	void on_nw_host_player_enter_result(int64_t p_connection_id, int p_success, int p_msg_code,
			const String &p_player_ticket, const String &p_access_code_list);

private:
	// One reset for the typed request staging, used by every session
	// load-failure leg and reset().
	void clear_pending_session();
	Ref<CharacterJoinProfile> build_join_character_profile(
			const Ref<ResourceRoot> &p_resource_root, const Ref<PlayerSpawnLoadout> &p_loadout);
	// Point the joiner's resource root at the HOST's expansion (S2C 0x7B
	// field 7, net-re 5.32) before any of the host's data is resolved through
	// it. Returns false when the join has been failed and the driver must
	// stop.
	bool reconcile_join_expansion();
	// The per-frame admission/deploy/loss observer.
	void update_joiner_admission_signals();
	void fail_join_preload(const String &p_reason);
	void cancel_join_preload();
	// Bind a hosting session to the live hosted match: the GSID/AppId, the
	// join-ticket arm and the host's own roster slot. unbind_nw_host undoes the
	// in-match half (the GSID, the join-ticket hook, the roster mirror).
	void bind_nw_host(const Ref<MissionSetupOptions> &p_opts);
	void unbind_nw_host();
	// Mirror the admitted joiners onto the hosting session's per-slot roster
	// (the PlayerList + ClientHostPlayerAdded/Removed).
	void sync_nw_host_roster(opennova::NwuHostRole &p_host, const Ref<Simulation> &p_sim);
	NovaWorldClient *nw_client() const;
	void stop_nw_client();
	// Hand the match the NovaWorld session's facts once a tick. The feed starts
	// at the session's first hosting/playing word: the joiner is already
	// playing at the handoff and the host already hosting, both before the
	// mission starts. A node gone mid-match reads as a reset session (the word
	// 0, the gate's NWU address still known), which the 62-frame block exits on.
	void sync_nwu_session(const Ref<Simulation> &p_sim);

	GameWorld *world_ = nullptr;
	// The native session policy: windows, latches, edge ordering, reason text.
	Ref<NetSessionPolicy> policy_;
	// The adopted NovaWorldClient (the joiner's or host's NWU session), held by
	// identity.
	ObjectID nw_client_id_;
	// Set while a hosting session is bound to the live hosted match.
	bool nw_host_bound_ = false;
	// Set once the feed starts (sync_nwu_session); cleared by reset().
	bool nwu_feed_live_ = false;
	// The roster slots last mirrored onto the hosting session: slot -> the
	// per-slot signature (name|ip:port|team), so only a changed slot re-sends.
	std::map<int, std::string> nw_roster_sent_;
	// The typed session request at the shell seam (ADR 0017): exactly one is
	// non-null during a net load -- the host screen's HostSessionOptions or
	// the joiner's dial JoinTarget -- stamped onto MissionSetupOptions as
	// host_session/join_target.
	Ref<HostSessionOptions> pending_host_;
	Ref<JoinTarget> pending_join_;
	// A retail LAN join authenticates before the wire-header world load. This
	// preload simulation owns that one live socket/session while S2C 0x7B
	// supplies map_file; stage_runtime_options surrenders it so the connection
	// is never restarted.
	Ref<Simulation> join_preload_sim_;
	Ref<ResourceRoot> join_preload_root_;
	Ref<ConnectionError> last_connection_error_;
};

} // namespace godot
