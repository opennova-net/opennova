// The joiner role: the non-authority client of a remote host. It owns the
// joiner's ClientRuntime and runs the joiner's per-frame world<->net frame
// over the kernel it binds. The retail client frame is ONE function run on
// every machine [orig: Game_ProcessMainFrame @0x5263f0;
// Client_ProcessNetworkFrame @0x42c180]; its non-authority half — recv-fold +
// decoded-consequence application to the local world BEFORE this frame's
// local World tick, then the local motor tick and the post-tick attachment
// recompose — is the sequence run_tick owns. The replication
// ClientReplicaPipeline stays the ONE decoded-entity fold and
// ClientWorldMaterializer the ONE registry materializer (ADR 0026): this class
// sequences and consumes them, it reduces nothing itself.
//
// ADR 0043 d3 (slice E8b): the frame's shell legs are engine legs of the role
// (the socket seam, the decoded-row asset resolution through the kernel, the
// local-player pumps, the charattr words); the shell keeps only the loadout
// profile seams (kit_seams: the profile-page composition it owns) and reads
// two observer facts after a tick (the diagnostic sample, the world-sync
// serial its registry-derived caches re-derive from).
#pragma once

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/replica_query.h>
#include <runtime/inmatch/replica_track.h>
#include <runtime/inmatch/server_files.h> // ServerLogs (the held server's stop lines)
#include <runtime/inmatch/session.h>

#include <runtime/replication/client_state.h>
#include <runtime/replication/client_world_materializer.h>

#include <runtime/world/collision.h> // CollisionWorld::ResolveState (per-replica resolver state)
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h> // PlayerSpawn (stamp_mission_start_identity)
#include <runtime/world/world.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::inmatch {

class JoinerRole final : public Role, public world::VehicleOccupancySource {
public:
	RoleKind kind() const override { return RoleKind::Joiner; }

	// The joiner's replica runtime; the shell installs its tables on it.
	std::unique_ptr<ClientRuntime> runtime;

	// The loadout profile seams the shell keeps (the weapon.sav page
	// composition is the shell's; every other frame leg is the role's):
	struct KitSeams {
		// The S2C 0x5A authoritative-loadout fold (the grant -> kit rows).
		std::function<void()> apply_authoritative;
		// Re-copy the resident kit page when the side selector moved; true =
		// the resident buffer changed (the frame then re-arms the 0x2F seam).
		std::function<bool()> reseed_on_side_change;
		// Re-arm the joiner 0x2F loadout-submission seam.
		std::function<void()> push;
	};
	KitSeams kit_seams;

	JoinerRole() = default;
	// The embedder's form: constructed with the loadout profile seams it keeps.
	explicit JoinerRole(KitSeams seams);
	// Unbinds the vehicle occupancy source bind() installed on the kernel's world.
	~JoinerRole() override;

	// (Re)build the runtime for a join; the request is retained so a load that
	// rebuilds an as-yet unstarted joiner (bring_up) rebuilds it the same way.
	// The shell installs its tables on the new runtime after. player_name is the
	// callsign as the game's code-page bytes, which the 0x42's NA carries and the
	// host's roster keeps (D-NET-661).
	// [orig: CNapiNPConnection_SendClientJoin @0x61fe20 - the NA write @0x620162]
	ClientRuntime &create_runtime(const std::string &player_name, JoinRole join_role,
			const std::string &spectator_password, const std::string &server_password,
			const std::string &join_password = {});
	void bind(mission::MissionKernel &kernel) override;
	bool bring_up() override;

	// The dialed socket and the host it reaches; null = socketless (every send
	// dropped, nothing received — the headless tests).
	void set_socket(opennova::IDatagramSocket *socket, const PeerAddr &host);
	// The title identity every runtime this role creates speaks (ClientRuntime::set_title):
	// configure before create_runtime. A runtime with none refuses to start.
	void set_title(const TitleIdentity *title) { title_ = title; }
	// The process-lived client words every runtime this role creates steps
	// (ClientRuntime::use_process_state; null: each runtime its own): the 0x4C report
	// count runs free across joins and missions as retail's global does (D-NET-602).
	// [orig: g_NetQualityReportTimer @0xA85B84 - Client_ProcessNetworkFrame @0x42C239 /
	//  @0x42C246, its only writers]
	void use_process_state(ClientProcessState *state) {
		client_process_ = state;
		if (runtime) runtime->use_process_state(state);
	}
	// The process's log devices, whose protocol _inout.txt takes a held server's stop lines at
	// a session destroy (destroy_session). Default: off.
	void set_server_logs(const ServerLogs &logs) { server_logs_ = logs; }
	// The address the join's socket is bound on (its bind address, 0 on every interface, and
	// the port it bound), which a held server's HOST STOPPED prints, as retail formats the
	// manager's socket there; unset prints the no-socket placeholder.
	// [orig: CNapiNPConnection_LogHostStopped @0x61E780 - the manager's socket read @0x61E7A6,
	//  Napi_FormatAddress of its +8 / +12 @0x61E7C2, the placeholder @0x61E7DD;
	//  NapiUdpSocket_CreateAndBind @0x62D2A0 - bindAddress @0x62D4C1, boundPort @0x62D4C4]
	void set_local_address(const PeerAddr &address) {
		local_address_ = address;
		local_address_known_ = true;
	}
	// The pre-mission subset of the frame: the same socket and runtime advance
	// the retail connect exchange; no World exists yet to tick, and the load
	// waits for the S2C 0x1A's mission start (D-NET-NEW-L2-19). The hello
	// latch and the per-frame clock are shared with the in-mission frame.
	void poll_preload();
	// The first leg of a mission start whose load runs after the S2C 0x1A, once its
	// definition and model loads are in (ClientRuntime::push_mission_start_first_loadout):
	// sends what its pumps build, the first 0x2F when the countdown is spent there.
	void push_mission_start_first_loadout();
	// The end of a mission start whose load ran after the S2C 0x1A
	// (ClientRuntime::set_mission_start_loading): sends what its pumps build, the two
	// 0x2F and the 0x0B, then runs the final wait's first pass for the S2C 0x0F, which
	// ships what the push's countdown held; nothing before the release.
	void push_mission_start_end();
	// The mission start's call, from the game-start wait's pass whose receive takes the S2C
	// 0x1A to the final wait's first pass, with no frame between its legs (D-NET-NEW-L2-35):
	// the wait's pass returns on the 0x1A's flag straight into the mission start's load, whose
	// end pushes the two 0x2F and the 0x0B and enters the final wait, none of it rendering. The
	// embedder's legs: `after_pass`, its own checks on the wait's pass (its window, the join
	// error; false stops ahead of the load); `load`, its load (the first leg's pumps inside it,
	// push_mission_start_first_loadout); `settle`, the streamed world's settle
	// (materialize_world_stream, then the statics' placement and the loaded-model warm). A leg
	// that returns false stops the call, its embedder reporting why. A mission start the 0x1A
	// already released goes straight to the load. Waiting: the pass took no 0x1A.
	// [orig: NapiClient_WaitForGameStart @0x42CC10 - the receive @0x42CC88, the send @0x42CC92,
	//  the flag test @0x42CCB9..0x42CCBF, the return @0x42CCC1; Game_StartMission @0x524360 -
	//  the call @0x524E29, the return-code tests @0x524E54..0x524E63, the 0x0B @0x52624B, the
	//  send @0x526255, the final wait @0x526295..0x5262E5]
	struct MissionStartLegs {
		std::function<bool()> after_pass;
		std::function<bool()> load;
		std::function<bool()> settle;
	};
	enum class MissionStartPass : uint8_t { Waiting, Stopped, Started };
	MissionStartPass poll_mission_start(const MissionStartLegs &legs);
	// The streamed pools into the World for that mission start's settle (D-NET-NEW-L2-38):
	// the frame phase that materializes them and stamps the statics' placed identity
	// (materialize_replica_world) runs only in a session update, and the settle runs in the
	// mission start's call, ahead of any (poll_mission_start), so it runs it ahead of placing
	// the streamed statics; the next update finds the World in step. Retail's client writes each
	// streamed row into its pool slot in its page's receive (a pool-1 0x0D's, a pool-2 0x10's),
	// and the mission start's load initializes every pool's rows from their models ahead of its
	// 0x0B.
	// [orig: NapiNPClientMsg_0x00D @0x432C40 - Pool_UpdateMaxUsedFromSlotId called @0x4333CD;
	//  NapiNPClientMsg_0x010 @0x433400 - Pool_UpdateMaxUsed called @0x433771;
	//  Game_StartMission @0x524360 - Entity_InitAllFromModels called @0x52567F]
	void materialize_world_stream();
	// The mission data's rules word into the world (bring_up and the S2C 0x64 copy's step).
	void stamp_mission_rules();
	// The witnessed key handlers queue one C2S 0x1D with the action id; it leaves
	// inside the next open send boundary [orig: cases 169/170/172 @0x4e0d77/@0x4e0df3/
	// @0x4e0e3e -> CNapiNetwork_QueueReliableMessage @0x4e0de7].
	bool queue_stance_change(uint16_t action_id);
	// A stance select on a non-authority client past the dispatcher's head
	// (Role::request_stance): the key's own refusals, then the C2S 0x1D alone --
	// every press sends, the selected stance included, and the latch waits for the
	// authority's 0x0A echo (LocalPlayer::latch_stance).
	// 0 stand / 1 crouch / 2 prone -> action 172 / 169 / 170.
	// [orig: Input_HandleActionBinding_0 cases 169/170/172 @0x4e0d77..0x4e0e87,
	//  no latch write; the latch @0x430562 / @0x430570]
	bool send_stance_request(int stance) override;
	// Choose the use-item seat request without mutating the local body.
	// The caller applies the equipped weapon's busy gate before this action.
	bool queue_mount_toggle();
	bool queue_numbered_seat(int seat_index);
	world::VehicleSeatOccupancy seat_occupancy(const world::Entity &carrier,
			const world::Seat &seat, world::EntityHandle requester) const override;
	void collect_hostile_mounts(const world::Entity &requester,
			std::vector<world::EntityHandle> &out) const override;
	bool remote_claimant(const world::Entity &carrier, world::Entity &out) const override;

	bool send_medic_request() override { return runtime && runtime->queue_medic_request(); }
	bool send_respawn_request() override { return runtime && runtime->queue_respawn_request(); }
	void run_tick(const TickInput &input) override;
	// The update whose receive ended the mission start's final wait (the S2C 0x0F, or the
	// ESC that leaves it), once: the Game Loop mode's initialize returns there.
	bool take_final_wait_end() override {
		const bool ended = final_wait_end_;
		final_wait_end_ = false;
		return ended;
	}
	bool session_lost(SessionError &error) const override;
	bool reset_to_baseline(SessionError &error) override;
	void close() override;
	// Input action 3 on a joiner, the quit to the main menu (the in-game menu's confirmed
	// exit, the admin's GOTO MENUSTATE): exit reason 1, then the connection's user leave runs
	// inside the action: the "I.C:CIDEMIS" record latched and its goodbye burst written at
	// once, the connection torn down (ClientRuntime::session_lost), so no later frame sends a
	// C2S and the next frame's session_lost leaves the mission (the shell's teardown sends
	// nothing more). [orig: Input_HandleActionBinding case 3 @0x49AF26 (g_MissionExitReason = 1),
	//  @0x49AF38..0x49AF42 -> CNapiNetwork_DisconnectActiveConnection @0x4C9140 (the record
	//  @0x4C91EF..0x4C923E, the request @0x4C924A) ->
	//  CNapiNPConnection_RequestDisconnect @0x61E0F0 (state 5: SetState(6) @0x61E0FA) ->
	//  CNapiNPConnection_TeardownActiveConnection @0x6253C0]
	void quit_to_menu();
	// The pre-game join screen's ABORT (the shell's cancel of a join ahead of the mission
	// start): the join state machine's cleanup, FullDestroy(-1), or FullDestroy(-2) in the wait
	// for the JOIN's S2C 0x00 (JoinerConnection::pre_game_cancel_reason), whose leave latches
	// "N.C:SWWAD[1]:<reason>" and writes the goodbye burst at once. A cancel inside the mission
	// start is the wait's abort, exit reason 1, which close() routes.
	// [orig: PreMenu_OnAbortCancel @0x568CA0 (g_PreMenuState = 9 @0x568CB4);
	//  MultiPlayer_JoinSessionStateMachine @0x56A320 - state 9's
	//  CNapiGameSession_FullDestroy(-1) @0x56A3E3; CNapiGameSession_FullDestroy @0x4C96A0 -
	//  the sprintf @0x4C96CC, the leave @0x4C96DB]
	void cancel_join();
	// The join's network type is NovaWorld (the menu's connect type 1): the session destroy keeps
	// its transport, so no NET TEARING DOWN SOCKET (ClientRuntime::tear_down_transport). Set by the
	// embedder from the join record; it outlives a runtime rebuild.
	void set_join_novaworld(bool novaworld) { join_novaworld_ = novaworld; }
	// The embedder's drain of the process enumerator's records (the browse the join went on
	// from), each writing its REMOVED line: the session reset's connection destroy runs it
	// (ClientRuntime::drain_enumerator). Set by the embedder; it outlives a runtime rebuild.
	void set_enumerator_drain(std::function<void()> drain) { enumerator_drain_ = std::move(drain); }
	void store_mission_exit(int32_t reason) override {
		if (runtime) runtime->set_mission_exit_reason(reason);
	}
	ClientRuntime *client_runtime() override { return runtime.get(); }
	int64_t last_net_us() const override { return last_net_us_; }
	// The frame statistics the C2S 0x0C's two stat bytes carry; the frame rate
	// also reaches the replica runtime's quality window through the base.
	// [orig: NetPacket_SerializePlayerState case 3 @0x4C1BA2 / @0x4C1BBC]
	void observe_frame_rate(int32_t fps) override {
		Role::observe_frame_rate(fps);
		uplink_avg_fps_ = fps;
	}
	void observe_cpu_share(int32_t cpu_percent) override { uplink_cpu_percent_ = cpu_percent; }

	// The exact-handle materialized world twin of a decoded wire row (null when
	// the handle has no truthful local carrier). Header-only joins read the
	// materializer's owned rows; full-BMS joins read the promoted registry row
	// (spawn-origin gated).
	world::Entity *replica_world_entity(world::World &world, world::EntityHandle handle);
	// The World's pool-1 slot leg (World::pool1_slot_leg, installed by the pump): the
	// slot's decoded row takes its visit there, then its twin's pose or its twin the
	// row's, then its proxy's live words (D-NET-196; joiner_role_replica_world.cpp).
	void visit_replica_pool1_slot(world::EntityHandle handle);

	// The JOINER's per-frame clock (the host uses HostOwner::now_tick). The
	// preload frame shares it, so it advances there too.
	uint32_t now_tick() const { return now_tick_; }

	bool started() const { return started_; }
	bool local_spawned() const { return local_spawned_; }
	// H, the host-assigned wire identity, once L is spawned and the runtime
	// still binds it. Unbound reads kInvalid, never 0: zero is a live pool-0
	// handle (the listen host's own player), and every consumer that excludes
	// "self" by handle would otherwise misclassify that remote row while the
	// binding is retired (an OpenNova-only state; retail caches the pointer
	// once at Player_InitPlayer @0x4E15F0 and never re-resolves it).
	bool has_self_wire_handle() const {
		return local_spawned_ && runtime && runtime->has_self_handle();
	}
	uint16_t self_wire_handle() const {
		return has_self_wire_handle() ? runtime->self_handle() : world::EntityHandle::kInvalid;
	}
	int flat_seconds() const { return flat_seconds_; }
	bool freeze_suspected() const { return freeze_suspected_; }
	// The ~1 Hz tripwire sampled this tick (one-shot: the observer's print).
	bool take_diagnostic_sample() {
		const bool sampled = diagnostic_sample_pending_;
		diagnostic_sample_pending_ = false;
		return sampled;
	}
	// Bumps on every streamed topology/world change the materializer applied:
	// the shell's registry-derived caches (the deploy-zone registry) re-derive
	// when it moves.
	uint64_t world_sync_serial() const { return world_sync_serial_; }

	// The exact-packed-row materializer of a wire-header world (the kernel's
	// wire_header_world flag names the mission form; the joiner frame owns
	// the fold, the embedder reads the placed rows it stamped).
	replication::ClientWorldMaterializer &materializer() { return materializer_; }
	const replication::ClientWorldMaterializer &materializer() const {
		return materializer_;
	}

	// enable_join: a fresh join resets every per-session latch (the runtime is
	// rebuilt beside it).
	void reset_for_join();
	// finish_load: a (re)load respawns L and re-baselines the release cursor
	// to the live runtime edge (a retail-style menu join keeps its session);
	// the tripwire samplers deliberately persist across loads.
	void reset_for_load(uint64_t deployment_release_revision_baseline) {
		local_spawned_ = false;
		deployment_release_revision_seen_ = deployment_release_revision_baseline;
		weapon_availability_revision_seen_ = 0;
		redeploy_release_pending_ = false;
		redeploy_health_updates_at_release_ = 0;
	}
	// finish_load's direct-load branch rebuilt a fresh ClientRuntime: the hello
	// re-arms and the receive-side cursors restart with it.
	void reset_for_runtime_rebuild() {
		started_ = false;
		local_resubmit_revision_seen_ = 0;
		weather_revision_seen_ = 0;
		weapon_availability_revision_seen_ = 0;
		mounted_ammo_revision_seen_ = 0;
		world_state_revision_seen_ = 0;
		track_seen_ = ReplicaTrackSeen{};
		replica_peer_scratch_src_ = nullptr;
		replica_peer_scratch_count_ = 0;
		replica_peer_scratch_tick_ = 0;
	}
	// The in-place reload into the host's next mission (ClientRuntime::
	// begin_mission_reload): the connection and this role stay, the replica
	// starts over, so the receive-side cursors re-baseline with it (the hello
	// latch kept) and L respawns in the next world. False when the runtime is
	// not in a session.
	bool begin_mission_reload() {
		if (!runtime || !runtime->begin_mission_reload()) return false;
		local_spawned_ = false;
		post_start_identity_pending_ = false;
		weather_revision_seen_ = 0;
		weapon_availability_revision_seen_ = 0;
		mounted_ammo_revision_seen_ = 0;
		world_state_revision_seen_ = 0;
		track_seen_ = ReplicaTrackSeen{};
		replica_peer_scratch_src_ = nullptr;
		replica_peer_scratch_count_ = 0;
		replica_peer_scratch_tick_ = 0;
		return true;
	}
	// Stop/Start restart with a live wire-header world: force one exact
	// rematerialization fold; the portal tables persist by design (their
	// handles remain identical and the weld records are one-shot mutable).
	void reset_materialization();
	// reset_world: the whole streamed-world image dies with the world.
	void reset_world_stream();

private:
	// What this frame's client net pump decoded (drives the later phases).
	struct FrameSignals {
		bool health = false;     // authoritative 0x0A health tail applied
		bool objectives = false; // objective sync applied
	};

	// --- the frame's own legs (ex the shell's hook table) ------------------
	// Ship one framed datagram to the dialed host (the socket send).
	void send(const std::vector<uint8_t> &dg);
	// FullDestroy(reason): the leave burst on the wire, then the session reset's stop of a held
	// server over the same socket (stop_held_server), then the transport's teardown line
	// (ClientRuntime::tear_down_transport; joiner_role.cpp).
	void destroy_session(int32_t reason);
	void stop_held_server();
	// Deposit received framed datagrams for this frame's recv pump (socket
	// drain -> runtime.receive).
	void deposit_inbound();
	// ClientHello once (Idle -> Hello) on the first armed frame.
	void send_hello_once();
	// Resolve each decoded organic row's adm id once its type is known.
	void resolve_row_adm_ids();
	uint64_t infantry_adm_revision_seen_ = 0;
	// A streamed topology/world change materialized: rebuild the asset-backed
	// caches (collision/occlusion instances, item traits, seat specs) for the
	// retired/spawned/updated rows.
	void on_replica_world_changed(const replication::ClientWorldSyncResult &sync);
	// The authoritative mount relation changed: the view effects (binocular
	// latch) and the mounted input heading follow.
	void on_mount_changed();
	// The equipped-slot FSM pump with the joiner's wire seams: the predicted
	// round becomes the C2S 0x06 descriptor, a reload the C2S 0x25 request.
	void tick_local_weapon();
	// The World's per-class ATTRIBUTES words follow the runtime's charattr
	// table the same frame an S2C 0x41 mutated it.
	void sync_class_attribute_flags();

	// --- the frame's phases ----------------------------------------------------
	void pump();
	FrameSignals run_client_net_frame();
	void wire_frame_providers();
	void apply_weather_sample();
	// The replica-world mirror and the authoritative appliers, through
	// refresh_wire_collision_proxies (joiner_role_replica_world.cpp).
	void materialize_replica_world();
	void spawn_and_arm_local_player();
	// The S2C 0x0F landing on L: the authoritative pose (+ look yaw, the
	// un-hide) and, for a waypoint gametype, the host-filtered route.
	void apply_world_state_load();
	// The self player record onto L, once per folded record: its Flags under 0xE1, then
	// the mount edge's position snap (D-NET-NEW-L2-13).
	void fold_local_record();
	void apply_authoritative_health();
	// The tail's carried word onto L's carried link (D-NET-728).
	void apply_local_carried_object(world::World &world, world::Entity &local);
	void sync_authoritative_mount();
	void apply_mounted_ammo_update();
	void mirror_mission_entities();
	void mirror_mission_entity(replication::ClientEntityState &es);
	void mirror_predicted_vehicles();
	void mirror_predicted_vehicle(replication::ClientEntityState &es);
	// The decoded rows' collision proxies, rebuilt at the World's proximity build
	// (World::before_proximity_build), and their live words between builds.
	void refresh_wire_collision_proxies();
	void sync_wire_collision_proxies_live();
	void sync_wire_collision_proxy_live(const replication::ClientEntityState &row);
	// The live Flags bits a row's proxy carries for the traces' skip (0x2000001).
	uint32_t wire_proxy_live_flags(const replication::ClientEntityState &row);
	void apply_gameplay_events();
    void apply_round_event(const replication::ClientRoundEvent &);

	opennova::IDatagramSocket *socket_ = nullptr;
	PeerAddr host_addr_{};
	const TitleIdentity *title_ = nullptr; // set_title
	ClientProcessState *client_process_ = nullptr; // use_process_state
	ServerLogs server_logs_{};                     // set_server_logs
	PeerAddr local_address_{};                     // set_local_address
	bool local_address_known_ = false;
	int64_t last_net_us_ = 0;
	// g_StatsAvgFps / g_StatsCpuPercent as the session last handed them over.
	int32_t uplink_avg_fps_ = 0;
	int32_t uplink_cpu_percent_ = 0;
	// The last folded 0x0A tail whose stance echo L has latched.
	uint32_t stance_echo_seen_ = 0;
	// The objective syncs whose subgoals the world holds: a receive outside the net frame (the
	// final wait's first pass, inside the push) folds them too, so the next net frame copies
	// them over (the copy is the state's, so a re-baselined count only copies it again).
	uint32_t objective_updates_seen_ = 0;
	// The self row's compact revision whose Flags byte L has folded.
	uint32_t local_flags_revision_seen_ = 0;
	// The wire-leg clock: the frame's start, stamped where the uplink ships
	// (before the decoded-state folds) so the stats board measures exactly the
	// wire leg.
	int64_t wire_leg_start_us_ = 0;
	bool diagnostic_sample_pending_ = false;
	uint64_t world_sync_serial_ = 0;

	// The world the current frame runs against, latched at frame entry: the
	// provider closures installed on the runtime's pipeline persist across
	// frames and must never capture a per-call reference (a reload rebuilds
	// the World before it rebuilds the runtime).
	world::World *world_ = nullptr;

	// The join request create_runtime was last given (the rebuild's inputs).
	std::string player_name_;
	JoinRole join_role_ = JoinRole::Player;
	std::string spectator_password_;
	std::string server_password_;
	std::string join_password_;
	bool started_ = false;       // ClientHello emitted (Idle -> Hello)
	bool join_novaworld_ = false; // set_join_novaworld: FullDestroy keeps the transport
	std::function<void()> enumerator_drain_; // set_enumerator_drain
	bool local_spawned_ = false; // L spawned at reached_in_match (one-shot guard)
	bool final_wait_seen_ = false; // the last tick ended inside the final wait
	bool final_wait_end_ = false;  // take_final_wait_end's one-shot
	bool wire_world_static_initialized_ = false;
	uint64_t wire_world_topology_revision_seen_ = ~uint64_t{0};
	uint64_t wire_world_stream_revision_seen_ = ~uint64_t{0};
	replication::ClientWorldMaterializer materializer_;
	// ClientRuntime raises a monotonic edge for each gameplay/deployment release.
	// The role uses it to snap L to the host-selected post-pick pose; health
	// remains separately gated by authoritative_spawn_released(), so C2S 0x0E
	// cannot kill L and a stale positive tail cannot revive a genuinely dead L.
	uint64_t deployment_release_revision_seen_ = 0;
	uint32_t mp_attributes_revision_seen_ = 0; // stamp_mission_rules' last copy
	// S2C 0x50 re-latched OUR OWN team (the second g_LocalTeam writer). The
	// join-time team arrives through the spawn, so only later edges apply here.
	// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — the latch @0x4319db]
	uint64_t self_team_revision_seen_ = 0;
	// The S2C 0x50 local arms consumed (ClientRuntime::local_resubmit_revision): the
	// profile kit re-arm follows the arm, not every self 0x50 (D-NET-572).
	uint64_t local_resubmit_revision_seen_ = 0;
	// Receive-once cursor for the conditional flags2&0x0f==8 mounted-ammo
	// record. A stale net sample must not refill a locally firing gun each tick.
	uint32_t mounted_ammo_revision_seen_ = 0;
	// The last S2C 0x0F landing applied to L (ClientState::world_state.revision).
	uint32_t world_state_revision_seen_ = 0;
	// The 0x0F / local 0x50 track legs consumed (replica_track.h).
	ReplicaTrackSeen track_seen_;
	bool redeploy_release_pending_ = false;
	uint32_t redeploy_health_updates_at_release_ = 0;
	uint32_t now_tick_ = 0;
	// The staged replica-peer table's identity (source pointer, count, pump
	// tick): the resolver lambda re-copies it only when one of them changes.
	const void *replica_peer_scratch_src_ = nullptr;
	int32_t replica_peer_scratch_count_ = 0;
	uint32_t replica_peer_scratch_tick_ = 0;
	// The last S2C 0x0A phase-2 ENV revision folded into the weather home.
	uint32_t weather_revision_seen_ = 0;
	// The accepted S2C 0x1D headers already latched into the world's Match.
	uint64_t weapon_availability_revision_seen_ = 0;
	// The last 0x5A grant whose class re-resolved L's row (run_client_net_frame).
	uint64_t loadout_row_revision_seen_ = 0;
	// g_CharClassTeam1/2 as the joiner's 0x5A grants leave them: a grant stores its
	// class for L's side off the authority (0 = the session start's, the join vars').
	uint8_t granted_side_class_[2] = {0, 0};
	// Player_InitPlayer's class word on L: the side its Team names (team 1 or 3 the first),
	// stored as it is.
	void stamp_init_player_class(world::Entity &local) const;
	// A self S2C 0x50's re-run of Player_InitPlayer(1) on L, then its identity pair
	// (D-NET-570).
	void reinit_local_player_on_team_assign();
	// The mission start's Player_InitPlayer on the identity of the L about to spawn
	// (D-NET-570's pre-spawn half; joiner_role.cpp carries the witness).
	void stamp_mission_start_identity(world::PlayerSpawn &spawn);
	// A self 0x50's local arm ran after the mission start bound the local entity, before L
	// spawned: L takes that 0x50's animSlot at its spawn.
	bool post_start_identity_pending_ = false;

	// ~1 Hz frozen-session tripwire state.
	uint32_t last_frontier_seq_ = 0;
	uint32_t last_records_applied_ = 0;
	uint32_t last_outbound_seq_ = 0;
	bool diagnostic_sampled_ = false;
	int flat_seconds_ = 0;
	bool freeze_suspected_ = false;

	// Per-replica-row resolver state (the movement collision resolver's
	// prev-pose + idle skip counter) keyed by wire handle — the persistent
	// half the ClientState row cannot carry across the replication seam.
	// Entries for retired rows are benign: a reused handle's stale prev pose
	// triggers one displaced-detect full update and self-corrects.
	std::unordered_map<uint16_t, world::CollisionWorld::ResolveState>
			replica_resolve_states_;
	// Scratch for the replica contact resolver's replication->world peer copy
	// (cleared per call; grows once to the session's peer cap).
	std::vector<world::CollisionWorld::ReplicaPeer> replica_peer_scratch_;
};

} // namespace opennova::inmatch
