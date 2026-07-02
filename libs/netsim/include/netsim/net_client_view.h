#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <novaworld/ingame_decode.h> // EntityClass

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

	const ClientState &state() const { return state_; }
	ClientState &state() { return state_; }
	std::uint32_t frames_applied() const { return state_.frames_applied; }
	std::size_t unknown_tags() const { return unknown_tags_; }

private:
	void apply_frame_update(const std::vector<uint8_t> &body);
	// Load-time world-stream spawn/static batches (§5.2a) -> ClientState upsert. Each carries
	// ABSOLUTE world positions (no anchor) + the entity identity/type, so spawn-only entities
	// (statics/markers) and not-yet-moving organics are present before any 0x0A motion arrives.
	void apply_organic_spawn(const std::vector<uint8_t> &body); // 0x0C pool-0
	void apply_pool_spawn(const std::vector<uint8_t> &body);    // 0x0D pool-1
	void apply_static_batch(const std::vector<uint8_t> &body);  // 0x10 pool-2
	void apply_pool3_batch(const std::vector<uint8_t> &body);   // 0x20 pool-3

	// Effective record classifier for the 0x0A event loop: the class LEARNED from the world
	// spawn stream wins, then the injected resolver. The retail client classifies via each
	// type's items.def serialize callback [orig: itemDef+356 dispatch @0x50f2e2 /
	// ItemList_FindIndexByTypeId]; without an items table, a 0x0D pool-1 spawn is the
	// witnessed signal that a type replicates as a VEHICLE (pool 1 = the vehicle pool), so
	// the view records type->Vehicle there and decodes the 15/21-B vehicle compact body for
	// those types (a Player/Infantry misparse would desync the whole record chain).
	EntityClass classify(uint16_t type_id) const;

	ClientState state_;
	std::function<EntityClass(uint16_t)> resolver_;
	std::unordered_map<uint16_t, EntityClass> learned_classes_;
	std::size_t unknown_tags_ = 0;
};

} // namespace opennova::netsim
