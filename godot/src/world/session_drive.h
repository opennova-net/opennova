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

#include <net/novaworld/lobby_vars.h> // HostPlayerSlot (the hosting bind's own slot)

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
// (D-NET-178), the post-load admission watch + per-frame
// admission/deploy/loss observer, the session-loss latch, the ESC aborts, and
// NovaWorld gate registration for a browsable listen host.
//
// The POLICY of all of that -- the preload's failure edge (no join wait has a
// deadline, D-NET-NEW-L2-44), the promote validation, the admission/deploy/loss
// edge machine with its latches, and the expansion reconcile DECISION -- is native
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
	// preload is pending), with no deadline: a join error or the session's loss
	// ends it (the policy owns the edge). Synchronous on purpose.
	void step_preload();
	bool is_preload_pending() const { return join_preload_sim_.is_valid(); }
	// ESC/abort for the only interruptible load leg: the joiner's pre-load
	// connect/session wait (the SP/host load remains one synchronous call the
	// SceneTree cannot interrupt). Returns true when an in-flight preload was
	// aborted; the ordinary load-failure leg reports it to the shell (engine: runtime/inmatch/join_session_policy.cpp)
	// (docs/interface/loading-screen-re.md D-LOADSCR-7).
	bool cancel_preload();
	// ESC in the mission start's final wait for the S2C 0x0F: it leaves the
	// wait for the game loop and aborts nothing (engine
	// inmatch::JoinerConnection::cancel_final_wait). Returns true when the
	// final wait took it; false anywhere else, the shell's cold-spawn hold
	// after the 0x0F included, where the game loop already runs
	// (D-NET-NEW-L2-39).
	bool cancel_admission_wait();
	// True while the staged host request is a DEDICATED serve (no local-player
	// spawn, ADR 0015 serve mode); read by the world's playable gate before
	// stage_runtime_options consumes the request.
	bool pending_dedicated() const;
	// True while the load is a session's: a staged host or join request, a
	// joiner's preload, or a kept session's next map; false for single
	// player (the session start's class clamp runs only in a session,
	// engine inmatch::join_character_profile).
	bool pending_session() const;
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
	// joiner's admission watch arms inside this drive's own load entries,
	// not here.)
	void on_runtime_started(const Ref<MissionSetupOptions> &p_opts, const String &p_bms_name);
	// Per-frame observer, called from the world's tick after the runtime
	// ticked: the admission/deploy/session-loss edges plus the gate's
	// advertised occupancy.
	void observe_tick(MissionRoot *p_runtime);
	// One teardown for everything this drive staged or stood up, called from
	// the world's unload(): the preload sim/root, the policy's windows +
	// notification latches, the typed request staging, and the NovaWorld
	// session unless the shell took it back first or a kept session rides
	// through the reload with it.
	void reset();
	// The map change (D-NET-331, inmatch/map_change.h): the live session's
	// simulation, taken out of the departing runtime (keep_session) and across
	// the world's unload into the next load. load_next_host_mission boots the
	// rotation's next map inside it; reload_as_joiner re-runs the joiner's
	// preload over its kept connection, with no window: a stock joiner's reload
	// legs have no deadline (net-re §5.70.7).
	bool keep_session(MissionRoot *p_runtime);
	bool has_kept_session() const { return kept_sim_.is_valid(); }
	int load_next_host_mission(const String &p_bms_name);
	// That load's head ahead of the world's effect system, the head's window past it, and its
	// stage pumps at the terrain and ahead of the objects, on the kept session
	// (Simulation::begin_next_host_mission_load, open_host_load_window, pump_host_load_stage);
	// no-ops on any other load.
	void begin_next_host_mission_load(const Ref<MissionData> &p_mission, const String &p_bms_name,
			const Ref<ResourceRoot> &p_resource_root);
	void open_host_load_window();
	void pump_host_load_stage();
	// The join's simulation ahead of the runtime's adoption (stage_runtime_options), null
	// otherwise: a join's load steps pump its session there (GameWorld::pump_join_load_step).
	Ref<Simulation> join_preload_simulation() const { return join_preload_sim_; }
	int reload_as_joiner();
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
	// The return to the main menu, ahead of the menu's entry: when a join's switch left an
	// expansion, `p_root` mounts it again and the cfg block takes its weapon table, reading
	// and saving nothing, so the menu entry's game.cfg read and profile load run over the
	// menu's own expansion (D-NET-622). A host or single-player start switches nothing, so
	// its return restores nothing (engine inmatch/game_config_block.h switch_expansion
	// carries the witness).
	void restore_kept_expansion(const Ref<ResourceRoot> &p_root);

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
	// r_avatar_db, when given, takes the Avatars.def the profile was built over (null
	// when none loaded).
	Ref<CharacterJoinProfile> build_join_character_profile(
			const Ref<ResourceRoot> &p_resource_root, const Ref<PlayerSpawnLoadout> &p_loadout,
			Ref<class AvatarDatabase> *r_avatar_db = nullptr);
	// Point the joiner's resource root at the HOST's expansion (S2C 0x7B
	// field 7, net-re 5.32) before any of the host's data is resolved through
	// it. Returns false when the join has been failed and the driver must
	// stop.
	bool reconcile_join_expansion();
	// The S2C 0x11's identification of the joined session, before the world stream: the
	// expansion reconcile, the promoted map name and header checks, the stream's item
	// catalog, the identified signal. False when the join has been failed and the driver
	// must stop.
	bool identify_join_session();
	// The S2C 0x11's load after the identification: the mission start's head and terrain
	// off the 0x0B header, the declared load's end (the 0x0A follows), the stream wait's
	// window. False when the join has been failed and the driver must stop.
	bool load_join_terrain();
	// The host's S2C 0x0B header as the join's mission document; null after the join's
	// failure.
	Ref<MissionData> open_join_mission_header(const String &p_bms);
	// The per-frame admission/deploy/loss observer.
	void update_joiner_admission_signals();
	void fail_join_preload(const String &p_reason);
	void cancel_join_preload();
	// Bind a hosting session to the live hosted match: the GSID/AppId, the
	// join-ticket arm, the host's own roster slot and the mission start's
	// update (both lists whole). unbind_nw_host undoes the in-match half (the
	// GSID, the join-ticket hook, the roster mirror).
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
	// A listen host's own PlayerList slot as the hosting bind published it;
	// the roster sync keeps its identity and follows its PlayerTeam / PlayerType.
	opennova::HostPlayerSlot nw_self_slot_;
	bool nw_self_slot_set_ = false;
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
	// The S2C 0x11's identification and load ran (identify_join_session,
	// load_join_terrain): the world stream runs until the S2C 0x1A's load.
	bool join_identified_ = false;
	// The map change's kept session and the join it came from.
	Ref<Simulation> kept_sim_;
	Ref<JoinTarget> last_join_target_;
	bool reloading_join_ = false;
	Ref<ConnectionError> last_connection_error_;
	// The expansion the first switch of a join left (byte_24D22A8), kept for the return.
	bool expansion_kept_ = false;
	String kept_expansion_;
};

} // namespace godot
