#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <npwire/ingame_decode.h> // EntityClass + WeaponReload (a per-family decode-header split candidate)

#include "netsim/client_state.h"
#include "netsim/session_transport.h"

namespace opennova::world {
class IRootMotionSource;
} // namespace opennova::world

namespace opennova::terrain {
struct TerrainHeightField;
} // namespace opennova::terrain

namespace opennova::netsim {

// The local client's decode pump: drains S2C datagrams off the loopback and folds
// them into a ClientState via the witnessed ingame_decode codec. This is the "local
// client decodes them via libs/novaworld/ingame_decode" half of the SP in-process
// listen server (ADR 0011). The same ClientState feeds the Godot present pass.
class ClientReplicaPipeline {
public:
	using ItemClassResolution = std::optional<EntityClass>;
	using ItemClassResolver =
			std::function<ItemClassResolution(uint16_t)>;

	ClientReplicaPipeline();
	explicit ClientReplicaPipeline(std::function<EntityClass(uint16_t)> resolver);

	// Drain every pending S2C datagram and apply it to the held ClientState. Used for the
	// host-as-client loopback path (inner {tag,body} datagrams, no handshake).
	void pump(ISessionTransport &channel);

	// Fold ONE already-decoded inner S2C body (tag + body, no NWU/SCRK framing) into the
	// ClientState. The remote-joiner path calls this for each body JoinerConnection surfaces off
	// its 0x83 SESSION decode (inbound_0a / inbound_world); pump() is the thin loopback loop over
	// it. Exactly ONE of {pump, apply-per-body} drives a given ClientState per frame (one fold
	// path per role) so frames_applied / seen_this_frame stay coherent.
	void apply(uint8_t tag, const std::vector<uint8_t> &body);

	// Advance the remote lean integrator one body tick (called once per client
	// frame). [orig: decay @0x4b5c97, then the ramp @0x4b7dbf/@0x4b7dd6]
	void tick_lean();
	void tick_arms_dip();
	// Wire handles whose compact record was dropped because its carrier could
	// not be resolved (the retail bail queues a C2S 0x0F entity request); the
	// embedding runtime drains and sends them once per frame.
	std::vector<uint16_t> drain_carrier_repair_requests();
	void tick_recoil();

	// JOINER role only (net-re §5.38e, D-NET-196): switch the 0x0A fold from
	// live-pose snap to smooth-target STAGING, and enable tick_remote_motion.
	// The host/SP roles keep the snap fold — their loopback view refreshes at
	// full rate and the authority never interpolates (D-NET-89).
	void set_remote_motion_mode(bool enabled) { remote_motion_mode_ = enabled; }
	bool remote_motion_mode() const { return remote_motion_mode_; }

	// The per-class between-update mover, one call per 62.5 Hz logic tick after
	// the recv fold (retail order: Client_ProcessNetworkFrame first, entity
	// movers after). Chases each armed row's live pose/heading toward its staged
	// wire target with the witnessed per-class math:
	//  - Player rows: the org2 body-pass chase [orig: Entity_UpdateInfantryPlayerBody
	//    @ 0x4B40E0, chase @ 0x4B4470..0x4B46C0] — incl. the own-player soft
	//    position reconciliation (self_handle; bucket 48 moving / 512 still).
	//  - Infantry rows: the org1 motor fall-through [orig: Entity_UpdateInfantryAI
	//    @ 0x4b9a8c] + the promoted-heading quarter-step body chase [orig: @ 0x4be8fd].
	//  - Vehicle rows: the family chase template [orig: Entity_UpdateWatercraftPhysics
	//    @ 0x48D480 et al.]. Rows the embedding sim flags net_world_mover are
	//    instead predicted by the world-side family movers (world/vehicle_motor,
	//    all four families landed) and skipped here; the row-side chase remains
	//    for traitless vehicles and lib-only embedders.
	// No-op unless remote-motion mode is enabled.
	void tick_remote_motion(uint16_t self_handle);

	// Recompose every carried row after its carrier's mover has completed. The
	// ordinary library-only tick_remote_motion path calls this as its final
	// phase. Embedders with a later world-side mover (the joiner vehicle
	// prediction seam) call it once more after mirroring those final carrier
	// poses back into ClientState. Keeping this phase explicit prevents pool
	// iteration order from leaving an earlier child one mover tick behind a
	// later carrier.
	// tick_sweep is true only from the per-tick mover call: it advances the
	// pure-client 128-tick stale-carrier sweep (C2S 0x0F for carrier + child,
	// local child destroy pending authority re-spawn) — record applies must
	// not double-run the cadence [orig: the @0x440d41..0x440e2f sweep].
	void refresh_carried_entities(bool tick_sweep = false);

	// S2C 0x5D empty-slot sweep: retire one RAW pool-0 slot index and everything
	// attached to it. The decoded view is the client's entity pool, so
	// Entity_Destroy's effect here is removing the ROW (not flagging it) —
	// omission from an 0x0A is deliberately not a despawn signal in this view, so
	// a retained row would keep blocking projectiles as a person proxy forever.
	// [orig: NapiNPClientMsg_DestroyEntityList @0x429730 -> Pool_GetEntryUnchecked(0, idx)
	//  + Entity_Destroy]
	void destroy_pool0_slot(uint16_t pool0_index);

	// S2C 0x50 team assign leg 2: `entity->Team = team` for ANY pool 0..4 entity on
	// a non-authority client. Upserts so an assignment that precedes the entity's
	// spawn record is not lost (an unresolved row keeps type_id 0, which the render
	// and collision passes both skip).
	// [orig: NapiNPClientMsg_0x050 @0x431910 — the team store @0x4319ee]
	void apply_team_assign(uint16_t handle, uint8_t team);

	const ClientState &state() const { return state_; }
	ClientState &state() { return state_; }
	std::uint32_t frames_applied() const { return state_.frames_applied; }
	std::uint64_t revision() const { return state_.revision; }
	std::uint64_t topology_revision() const {
		return state_.topology_revision;
	}
	std::size_t unknown_tags() const { return unknown_tags_; }

	// One-shot gameplay notifications surfaced by apply(). Draining keeps the
	// decoded ClientState persistent while preventing event replay on later frames.
	std::vector<ClientRoundEvent> drain_round_events();
	std::vector<WeaponReload> drain_weapon_reloads();
	// S2C 0x13 entity-death notifies folded by apply(): the row's health drops to
	// zero and the record is surfaced once so the embedding sim can run the
	// class death callback on its world twin (a destructible's husk/explosion
	// chain — reason 4, the net kill).
	// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 — Health = 0 @0x42ebd6,
	//  deathAnimStateId = killerSource @0x42ebdf, deathCallback(entity, 4, 0)
	//  @0x42ebf5]
	std::vector<EntityDeathRecord> drain_entity_deaths();

	// Install the items.def-derived per-type classifier — the table the retail client
	// itself dispatches 0x0A records through (each type's serialize callback, seeded
	// from the *_function class tag at items.def load [orig: itemDef+356 dispatch
	// @0x50f2e2 / ItemList_FindIndexByTypeId]). Every present catalog entry
	// OUTRANKS the learned/heuristic chain in classify(): the 0x0D pool blanket
	// brands every pool-1 type Vehicle, which mis-sizes a no-callback item's
	// header-only record (e.g. an `ewep` emplacement) and desyncs the rest of the
	// frame. nullopt falls through to the learned map, then the phase-1 resolver;
	// a present Unknown is a known unresolved definition and fails closed.
	void set_item_class_resolver(ItemClassResolver resolver);

	// Inject the embedder's .adm root-motion source (JOINER role): armed rows
	// (rm_adm_id >= 0, stamped by the embedder) advance their own AnimMap
	// primary channel each tick and integrate the rotated root delta after the
	// chase — retail's remote-body dead reckoning [orig: the class movers run
	// the full body pass; root integration @0x4B7CB4 / @0x4BF684]. Null (the
	// lib-only embedders) = the truthful chase-only degradation.
	void set_root_motion_source(world::IRootMotionSource *source) {
		root_motion_ = source;
	}

	// Install the joiner's terrain column for the bounded remote-person
	// post-motion probe. The pipeline owns neither field nor backing buffers.
	// [orig: Entity_UpdateInfantryPlayerBody resolver call @0x4B7CF4;
	// Entity_UpdateInfantryAI caller @0x4BF7FA;
	// Entity_MovementCollisionResolver @0x4B2BD0]
	void set_remote_motion_terrain(
			const terrain::TerrainHeightField *terrain) {
		remote_motion_terrain_ = terrain;
	}

	// The phase-3 0x0A objective block has no on-wire discriminator. apply()
	// learns the shared g_GameType from S2C 0x08 field 3 / 0x7B `extra`;
	// replay/bootstrap callers may also seed it explicitly before a midstream 0x0A.
	void set_game_type(uint32_t game_type) { game_type_ = game_type; }
	uint32_t game_type() const { return game_type_; }

private:
	void queue_carrier_repair(uint16_t handle);
	std::vector<uint16_t> carrier_repair_requests_;
	// Shared S2C 0x13 / 0x26 death fold (retail gates + row health + the
	// surfaced record). [orig: NapiNPClientMsg_EntityDeath @0x42EB50 /
	// Entity_KillBySlotId @0x42BCE0]
	void apply_entity_death(uint16_t handle_packed, int16_t killer_source);
	void apply_frame_update(const std::vector<uint8_t> &body);
	// Load-time world-stream spawn/static batches (§5.2a) -> ClientState upsert. Each carries
	// ABSOLUTE world positions (no anchor) + the entity identity/type, so spawn-only entities
	// (statics/markers) and not-yet-moving organics are present before any 0x0A motion arrives.
	void apply_organic_spawn(const std::vector<uint8_t> &body); // 0x0C pool-0
	void apply_pool_spawn(const std::vector<uint8_t> &body);    // 0x0D pool-1
	void apply_static_batch(const std::vector<uint8_t> &body);  // 0x10 pool-2
	void apply_pool3_batch(const std::vector<uint8_t> &body);   // 0x20 pool-3
	void erase_entity_tree(uint16_t root_handle);
	// Land one decoded compact world sample on a row: live snap in snap mode /
	// on the forced edges (respawn, vehicle dead-pose); smooth-target staging +
	// interpProgress reset in remote-motion mode (§5.38e stage-only reads).
	void land_compact_pose(ClientEntityState &es, int32_t wx, int32_t wy,
	                       int32_t wz, bool has_heading, int32_t heading_bam,
	                       bool force_live_snap);

	// Effective record classifier for the 0x0A event loop: the items.def table (when
	// installed) wins, then the class LEARNED from the world spawn stream, then the
	// injected resolver. The retail client classifies via each type's items.def
	// serialize callback [orig: itemDef+356 dispatch @0x50f2e2 /
	// ItemList_FindIndexByTypeId] — that is item_resolver_. Without an items table,
	// a 0x0D pool-1 spawn is the witnessed signal that a type replicates as a VEHICLE
	// (pool 1 = the vehicle pool), so the view records type->Vehicle there and decodes
	// the 15/21-B vehicle compact body for those types (a Player/Infantry misparse
	// would desync the whole record chain).
	EntityClass classify(uint16_t type_id) const;

	ClientState state_;
	world::IRootMotionSource *root_motion_ = nullptr;
	const terrain::TerrainHeightField *remote_motion_terrain_ = nullptr;
	uint32_t rm_tick_counter_ = 0; // the leg re-plant window clock [orig: tick&63]
	bool remote_motion_mode_ = false;
	ItemClassResolver item_resolver_;                    // items.def table (authoritative)
	std::function<EntityClass(uint16_t)> resolver_;      // phase-1 heuristic fallback
	std::unordered_map<uint16_t, EntityClass> learned_classes_;
	std::vector<ClientRoundEvent> pending_round_events_;
	std::vector<WeaponReload> pending_weapon_reloads_;
	std::vector<EntityDeathRecord> pending_entity_deaths_;
	std::size_t unknown_tags_ = 0;
	uint32_t game_type_ = 0;
	// Mission-seeded PRNG_Next16 stand-in shared by every decoded row in this
	// view. The body consumes one draw per person per tick even when recoil is
	// zero. Retail also has unrelated process-global consumers that this decoded
	// seam cannot honestly order against; algorithm and local call history after
	// the exact Game_StartMission seed remain bounded here.
	uint32_t prng16_ = 0;
};

} // namespace opennova::netsim
