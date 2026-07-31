#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <npwire/ingame_decode.h> // EntityClass + WeaponReload (a per-family decode-header split candidate)

#include "netsim/client_state.h"
#include "netsim/session_transport.h"

namespace opennova::netsim {

// The local client's decode pump: drains S2C datagrams off the loopback and folds
// them into a ClientState via the witnessed ingame_decode codec. This is the "local
// client decodes them via libs/novaworld/ingame_decode" half of the SP in-process
// listen server (ADR 0011). The same ClientState feeds the Godot present pass.
class NetClientView {
public:
	NetClientView();
	explicit NetClientView(std::function<EntityClass(uint16_t)> resolver);

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
	void tick_recoil();

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
	std::size_t unknown_tags() const { return unknown_tags_; }

	// One-shot gameplay notifications surfaced by apply(). Draining keeps the
	// decoded ClientState persistent while preventing event replay on later frames.
	std::vector<ClientRoundEvent> drain_round_events();
	std::vector<WeaponReload> drain_weapon_reloads();

	// Install the items.def-derived per-type classifier — the table the retail client
	// itself dispatches 0x0A records through (each type's serialize callback, seeded
	// from the *_function class tag at items.def load [orig: itemDef+356 dispatch
	// @0x50f2e2 / ItemList_FindIndexByTypeId]). When it resolves a type (non-Unknown)
	// it OUTRANKS the learned/heuristic chain in classify(): the 0x0D pool blanket
	// brands every pool-1 type Vehicle, which mis-sizes a no-callback item's
	// header-only record (e.g. an `ewep` emplacement) and desyncs the rest of the
	// frame. Unknown falls through to the learned map, then the phase-1 resolver.
	void set_item_class_resolver(std::function<EntityClass(uint16_t)> resolver);

	// The phase-3 0x0A objective block has no on-wire discriminator. apply()
	// learns the shared g_GameType from S2C 0x08 field 3 / 0x7B `extra`;
	// replay/bootstrap callers may also seed it explicitly before a midstream 0x0A.
	void set_game_type(uint32_t game_type) { game_type_ = game_type; }
	uint32_t game_type() const { return game_type_; }

private:
	void apply_frame_update(const std::vector<uint8_t> &body);
	// Load-time world-stream spawn/static batches (§5.2a) -> ClientState upsert. Each carries
	// ABSOLUTE world positions (no anchor) + the entity identity/type, so spawn-only entities
	// (statics/markers) and not-yet-moving organics are present before any 0x0A motion arrives.
	void apply_organic_spawn(const std::vector<uint8_t> &body); // 0x0C pool-0
	void apply_pool_spawn(const std::vector<uint8_t> &body);    // 0x0D pool-1
	void apply_static_batch(const std::vector<uint8_t> &body);  // 0x10 pool-2
	void apply_pool3_batch(const std::vector<uint8_t> &body);   // 0x20 pool-3
	void refresh_parented_pool_entities();
	void erase_entity_tree(uint16_t root_handle);

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
	std::function<EntityClass(uint16_t)> item_resolver_; // items.def table (authoritative)
	std::function<EntityClass(uint16_t)> resolver_;      // phase-1 heuristic fallback
	std::unordered_map<uint16_t, EntityClass> learned_classes_;
	std::vector<ClientRoundEvent> pending_round_events_;
	std::vector<WeaponReload> pending_weapon_reloads_;
	std::size_t unknown_tags_ = 0;
	uint32_t game_type_ = 0;
	// BSS-zero PRNG_Next16 stand-in shared by every decoded row in this view. The
	// body consumes one draw per person per tick even when recoil is zero. Retail
	// also has unrelated process-global consumers that this decoded seam cannot
	// honestly order against; algorithm and local call history remain exact.
	uint32_t prng16_ = 0;
};

} // namespace opennova::netsim
