// The ADR 0011 §4 loopback identity guard.
//
// The SP listen server serializes REAL World entity state to the wire and the
// host's own local client decodes it. This test drives that whole path with one
// static entity:
//
//   World entity -> EntityWireBridge::snapshot_of -> build_tag_0a_world_reference
//     (encode) -> LoopbackChannel -> NetClientView::pump (decode) -> ClientState
//
// and asserts the client's decoded view is field-identical to what the host's
// encoder + the lossy position codec produced (a pipeline identity, not equality to
// the pre-compression value — the codec is intentionally lossy). This guard must
// stay green through every phase.

#include "netsim/entity_wire_bridge.h"
#include "netsim/loopback_channel.h"
#include "netsim/net_client_view.h"
#include "netsim/net_system.h"
#include "netsim/serializing_sink.h"

#include <novaworld/ingame_encode.h> // network_compress_fixedpoint
#include <world/entity.h>
#include <world/geom.h>
#include <world/world.h>

#include <cstdint>
#include <cstdio>

namespace {

namespace nw = opennova;
namespace ns = opennova::netsim;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// The exact value the host's encoder + codec produce for one axis: compress the
// (wire - anchor) delta, decompress it, add the anchor back. The client must land
// on precisely this.
int32_t codec_recon(int32_t wire, int32_t anchor) {
	return anchor + nw::network_decompress_fixedpoint(
	                        nw::network_compress_fixedpoint(wire - anchor));
}

bool run() {
	// --- a World with one static AI-infantry entity in pool 0 ---
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity seed;
	seed.kind = w::EntityKind::Organic; // -> EntityClass::Infantry
	seed.item_id = 0x2000;              // non-player template
	seed.position = {10.0f, 20.0f, -5.0f};
	seed.yaw = 0x0140;
	seed.team = 1;
	const w::EntityHandle h = world.registry.spawn(0, seed);
	if (!expect(h.valid(), "entity spawned")) return false;

	// --- the SP serializing sink replaces LocalSink (the seam is wired) ---
	ns::LoopbackChannel channel;
	ns::SerializingSink sink(channel);
	world.net = &sink;
	if (!expect(world.net->is_authority(h), "SP host is authority")) return false;

	// --- frame anchor = the subject (local player) reference position ---
	nw::PlayerReplicationState anchor;
	anchor.spawn_x = static_cast<uint32_t>(w::to_fixed(8.0));
	anchor.spawn_y = static_cast<uint32_t>(w::to_fixed(18.0));
	anchor.spawn_z = static_cast<uint32_t>(w::to_fixed(-5.0));

	// --- NetSystem integrates as an ISystem (runs ahead of gameplay) ---
	ns::NetSystem net(channel);
	world.add_system(&net);
	world.load_systems();
	const uint32_t t0 = world.logic_tick;
	world.run_logic_tick(); // NetSystem::tick (no-op C2S drain) runs under authority
	if (!expect(world.logic_tick == t0 + 1, "logic tick advanced")) return false;

	// --- host emits the post-logic S2C frame; the local client decodes it ---
	net.emit_s2c(world, anchor);
	if (!expect(channel.s2c_pending() == 1, "one 0x0A frame on the loopback")) return false;

	ns::NetClientView view;
	view.pump(channel);
	if (!expect(view.frames_applied() == 1 && view.unknown_tags() == 0,
	            "client applied exactly one frame, no unknown tags")) return false;

	const ns::ClientState &cs = view.state();
	if (!expect(cs.entities.size() == 1, "one decoded entity")) return false;
	const ns::ClientEntityState &es = cs.entities[0];

	// Handle / class / type round-trip exactly.
	if (!expect(es.handle == static_cast<uint16_t>(
	                    (h.pool() << 12) | (h.slot() & 0x0FFF)),
	            "handle round-trips")) return false;
	if (!expect(es.cls == nw::EntityClass::Infantry, "decoded as infantry")) return false;
	if (!expect(es.type_id == 0x2000, "type id round-trips")) return false;

	// Anchor stored verbatim.
	if (!expect(cs.anchor_x == static_cast<int32_t>(anchor.spawn_x) &&
	            cs.anchor_y == static_cast<int32_t>(anchor.spawn_y) &&
	            cs.anchor_z == static_cast<int32_t>(anchor.spawn_z),
	            "frame anchor stored")) return false;

	// Position is the codec's exact reconstruction of the entity's wire position.
	const int32_t wx = w::to_fixed(seed.position.x);
	const int32_t wy = w::to_fixed(seed.position.y);
	const int32_t wz = w::to_fixed(seed.position.z);
	if (!expect(es.x == codec_recon(wx, static_cast<int32_t>(anchor.spawn_x)),
	            "x is the codec's exact reconstruction")) return false;
	if (!expect(es.y == codec_recon(wy, static_cast<int32_t>(anchor.spawn_y)),
	            "y is the codec's exact reconstruction")) return false;
	if (!expect(es.z == codec_recon(wz, static_cast<int32_t>(anchor.spawn_z)),
	            "z is the codec's exact reconstruction")) return false;

	// Coarse heading round-trips: the high byte of the 32-bit engine BAM.
	const uint8_t want_yaw = static_cast<uint8_t>(
			(static_cast<uint32_t>(static_cast<int32_t>(seed.yaw) << 16) + 0x00800000u) >> 24);
	if (!expect(es.yaw_byte == want_yaw, "coarse yaw byte round-trips")) return false;

	// A second emit/pump applies cleanly (frame counter advances, entity reused).
	net.emit_s2c(world, anchor);
	view.pump(channel);
	if (!expect(view.frames_applied() == 2 && view.state().entities.size() == 1,
	            "second frame re-applies to the same entity")) return false;

	return true;
}

} // namespace

int main() {
	const bool ok = run();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
