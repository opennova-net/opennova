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

#include <net/novaworld/proxy_rendezvous.h> // ProxyRendezvousConfig (the proxy-assisted join)
#include <net/npwire/idatagram_socket.h>
#include <net/npwire/peer_addr.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/replica_query.h>
#include <runtime/inmatch/session.h>

#include <runtime/replication/client_state.h>
#include <runtime/replication/client_world_materializer.h>

#include <runtime/world/collision.h> // CollisionWorld::ResolveState (per-replica resolver state)
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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
		// L revived on an ACK-qualified redeploy release: rebuild the respawn
		// loadout (Player_InitPlayer's weapon leg + its view/map resets).
		std::function<void()> respawn;
	};
	KitSeams kit_seams;

	JoinerRole() = default;
	// The embedder's form: constructed with the loadout profile seams it keeps.
	explicit JoinerRole(KitSeams seams);
	// Unbinds the vehicle occupancy source bind() installed on the kernel's world.
	~JoinerRole() override;

	// (Re)build the runtime for a join; the request is retained so a load that
	// rebuilds an as-yet unstarted joiner (bring_up) rebuilds it the same way.
	// The shell installs its tables on the new runtime after.
	ClientRuntime &create_runtime(const std::string &player_name, JoinRole join_role,
			const std::string &spectator_password, const std::string &server_password,
			const std::string &join_password = {});
	void bind(mission::MissionKernel &kernel) override;
	bool bring_up() override;

	// The dialed socket and the host it reaches; null = socketless (every send
	// dropped, nothing received — the headless tests).
	void set_socket(opennova::IDatagramSocket *socket, const PeerAddr &host);
	// The proxy-assisted NovaWorld join (the .joi NI/NP/BK next to NK): the game
	// node the 48-byte rendezvous targets, the relay cookie and the NK relay
	// endpoint the ordinary dial uses. All six fields set = the rendezvous is
	// sent to the node every 3000 ms ahead of each announce while the hello is
	// outstanding; any field missing = no proxy (LAN, or a .joi without them).
	// The shell fills these from JoinTarget.proxy_node / proxy_relay /
	// proxy_cookie. [orig: CNapiGameSession_InitTransportConnection @0x4c9e10
	//  install @0x4ca051..0x4ca0c7; the all-six gate
	//  CNapiNPConnection_PumpEnumeratorAndSend @0x62914f..0x629159]
	struct JoinProxyOptions {
		std::string proxy_node_ip;    // NI, a dotted quad
		uint32_t proxy_node_port = 0; // NP
		uint32_t proxy_cookie = 0;    // atol(BK)
		std::string proxy_relay_ip;   // the NK-decoded host, a dotted quad
		uint32_t proxy_relay_port = 0; // the NK-decoded port
	};
	void set_join_proxy(const JoinProxyOptions &options);
	// The pre-mission subset of the frame: the same socket and runtime advance
	// the retail connect exchange, but no World exists yet to tick and the
	// runtime's world-ready gate suppresses the load/spawn drive. The hello
	// latch and the per-frame clock are shared with the in-mission frame.
	void poll_preload();
	// The witnessed key handlers queue one C2S 0x1D with the action id; it leaves
	// inside the next open send boundary [orig: cases 169/170/172 @0x4e0d77/@0x4e0df3/
	// @0x4e0e3e -> CNapiNetwork_QueueReliableMessage @0x4e0de7].
	bool queue_stance_change(uint16_t action_id);
	// A stance key on a non-authority client: the key's own refusals, then the
	// C2S 0x1D alone -- every press sends, the selected stance included, and
	// the latch waits for the authority's 0x0A echo (LocalPlayer::latch_stance).
	// 0 stand / 1 crouch / 2 prone -> action 172 / 169 / 170.
	// [orig: Input_HandleActionBinding_0 cases 169/170/172 @0x4e0d77..0x4e0e87,
	//  no latch write; the latch @0x430562 / @0x430570]
	bool request_stance(int stance);
	// Choose the use-item seat request without mutating the local body.
	// The caller applies the equipped weapon's busy gate before this action.
	bool queue_mount_toggle();
	bool queue_numbered_seat(int seat_index);
	world::VehicleSeatOccupancy seat_occupancy(const world::Entity &carrier,
			const world::Seat &seat, world::EntityHandle requester) const override;
	void collect_hostile_mounts(const world::Entity &requester,
			std::vector<world::EntityHandle> &out) const override;
	bool remote_claimant(const world::Entity &carrier, world::Entity &out) const override;

	bool spectator() const override { return runtime && runtime->is_spectator(); }
	bool send_medic_request() override { return runtime && runtime->queue_medic_request(); }
	void run_tick(const TickInput &input) override;
	bool session_lost(SessionError &error) const override;
	bool reset_to_baseline(SessionError &error) override;
	void close() override;
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
		weather_revision_seen_ = 0;
		weapon_availability_revision_seen_ = 0;
		mounted_ammo_revision_seen_ = 0;
		world_state_revision_seen_ = 0;
		replica_peer_scratch_src_ = nullptr;
		replica_peer_scratch_count_ = 0;
		replica_peer_scratch_tick_ = 0;
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
	// Deposit received framed datagrams for this frame's recv pump (socket
	// drain -> runtime.receive).
	void deposit_inbound();
	// ClientHello once (Idle -> Hello) on the first armed frame.
	void send_hello_once();
	// The proxy rendezvous datagram to the game node, on the enumerator's
	// 3000 ms interval, ahead of the announce it precedes.
	void pump_proxy_rendezvous();
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
	void materialize_replica_world();
	void spawn_and_arm_local_player();
	// The S2C 0x0F landing on L: the authoritative pose (+ look yaw, the
	// un-hide) and, for a waypoint gametype, the host-filtered route.
	void apply_world_state_load();
	void apply_authoritative_health();
	void sync_authoritative_mount();
	void apply_mounted_ammo_update();
	void mirror_mission_entities();
	void mirror_predicted_vehicles();
	void refresh_wire_collision_proxies();
	void apply_gameplay_events();
    void apply_round_event(const replication::ClientRoundEvent &);

	opennova::IDatagramSocket *socket_ = nullptr;
	PeerAddr host_addr_{};
	// The proxy rendezvous: the installed six fields, the node endpoint and
	// the enumerator's last-send clock (0 = never sent, the first pump sends).
	// [orig: conn+1464..+1484; enum_info+52 @0x629108/@0x62911f]
	ProxyRendezvousConfig proxy_{};
	PeerAddr proxy_node_addr_{};
	uint64_t proxy_last_send_ms_ = 0;
	int64_t last_net_us_ = 0;
	// g_StatsAvgFps / g_StatsCpuPercent as the session last handed them over.
	int32_t uplink_avg_fps_ = 0;
	int32_t uplink_cpu_percent_ = 0;
	// The last folded 0x0A tail whose stance echo L has latched.
	uint32_t stance_echo_seen_ = 0;
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
	bool local_spawned_ = false; // L spawned at reached_in_match (one-shot guard)
	bool wire_world_static_initialized_ = false;
	uint64_t wire_world_topology_revision_seen_ = ~uint64_t{0};
	uint64_t wire_world_stream_revision_seen_ = ~uint64_t{0};
	replication::ClientWorldMaterializer materializer_;
	// ClientRuntime raises a monotonic edge for each gameplay/deployment release.
	// The role uses it to snap L to the host-selected post-pick pose; health
	// remains separately gated by authoritative_spawn_released(), so C2S 0x0E
	// cannot kill L and a stale positive tail cannot revive a genuinely dead L.
	uint64_t deployment_release_revision_seen_ = 0;
	// S2C 0x50 re-latched OUR OWN team (the second byte_A85B48 writer). The
	// join-time team arrives through the spawn, so only later edges apply here.
	// [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — the latch @0x4319db]
	uint64_t self_team_revision_seen_ = 0;
	// Receive-once cursor for the conditional flags2&0x0f==8 mounted-ammo
	// record. A stale net sample must not refill a locally firing gun each tick.
	uint32_t mounted_ammo_revision_seen_ = 0;
	// The last S2C 0x0F landing applied to L (ClientState::world_state.revision).
	uint32_t world_state_revision_seen_ = 0;
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
