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

#include "netsim/connection_fan.h"
#include "netsim/entity_wire_bridge.h"
#include "netsim/loopback_channel.h"
#include "netsim/net_client_view.h"
#include "netsim/serializing_sink.h"

#include "conn_fan_test_util.h"

#include <novaworld/ingame_decode.h> // EntityPacketSubHeader / PlayerExtendedUplink
#include <novaworld/ingame_encode.h> // network_compress_fixedpoint, encode_* uplink
#include <world/ai.h>                 // AiSystem / AiEntity (engine-frame mirror)
#include <world/entity.h>
#include <world/geom.h>
#include <world/world.h>

#include <cstdint>
#include <cstdio>
#include <vector>

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

	// --- one loopback connection in the host's table (no owned entity -> rides the fallback anchor) ---
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, {}, 0});
	world.load_systems();
	const uint32_t t0 = world.logic_tick;
	world.run_logic_tick(); // advances under authority (the C2S drain is host-driven, not an ISystem)
	if (!expect(world.logic_tick == t0 + 1, "logic tick advanced")) return false;

	// --- host emits the post-logic S2C frame; the local client decodes it ---
	ns::test::emit_all(world, conns, anchor);
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

	// Coarse heading round-trips: the high byte of the 32-bit engine-frame BAM that
	// snapshot_of builds = (90 - mission_yaw) * kBamPerDegree (entity_wire_bridge.cpp), rounded.
	constexpr int64_t kBamPerDegree = 11930464; // 2^32 / 360 (matches the bridge)
	const uint32_t engine_bam = static_cast<uint32_t>(
			static_cast<int32_t>(static_cast<int64_t>(90 - seed.yaw) * kBamPerDegree));
	const uint8_t want_yaw = static_cast<uint8_t>((engine_bam + 0x00800000u) >> 24);
	if (!expect(es.yaw_byte == want_yaw, "coarse yaw byte round-trips (engine-frame BAM)")) return false;

	// A second emit/pump applies cleanly (frame counter advances, entity reused).
	ns::test::emit_all(world, conns, anchor);
	view.pump(channel);
	if (!expect(view.frames_applied() == 2 && view.state().entities.size() == 1,
	            "second frame re-applies to the same entity")) return false;

	return true;
}

bool run_header_only_records_are_ignored_by_client_view() {
	nw::FrameUpdate fu;
	fu.flags2 = 0x02;
	fu.env.fog_dist = 0x1111;
	fu.env.fog_accel = 0x2222;
	fu.env.tod_fixed = 0x3333;
	fu.mount_handle = 0xFFFF;
	fu.health = 100;

	nw::FrameUpdateRecord r;
	r.handle = 0x2007;
	r.type_id = 0x0465;
	r.cls = nw::class_from_tag("bldg");
	fu.records.push_back(r);

	ns::LoopbackChannel channel;
	channel.host_send(ns::kTag0aFrameUpdate, nw::encode_frame_update(fu));
	ns::NetClientView view([](uint16_t t) {
		return t == 0x0465 ? nw::class_from_tag("bldg") : nw::EntityClass::Unknown;
	});
	view.pump(channel);

	if (!expect(view.frames_applied() == 1, "header-only frame applied")) return false;
	if (!expect(view.state().entities.empty(),
	            "header-only null-callback record does not create a client entity")) return false;
	return true;
}

// Build a 48-byte C2S 0x0C extended uplink (5-B sub-header + 43-B body) from the encoders.
std::vector<uint8_t> make_0c_uplink(uint16_t handle, int32_t x, int32_t y, int32_t z,
                                    int16_t heading, int16_t pitch) {
	nw::EntityPacketSubHeader hdr;
	hdr.handle = handle;
	hdr.item_type_id = 0x14B9; // player infantry
	hdr.sub_op = 0x0A;         // extended (type 10)
	nw::PlayerExtendedUplink up;
	up.carrier_handle = 0xFFFF; // unmounted
	up.pos_x = x;
	up.pos_y = y;
	up.pos_z = z;
	up.heading = heading;
	up.pitch = pitch;
	std::vector<uint8_t> body = nw::encode_entity_packet_sub_header(hdr);
	const std::vector<uint8_t> tail = nw::encode_player_extended_uplink(up);
	body.insert(body.end(), tail.begin(), tail.end());
	return body;
}

// The host read-applies a remote peer's C2S 0x0C uplink: the authority drain (drain_connection_c2s)
// reads it and EntityWireBridge::apply_player_intent SNAPS the registry Entity (the store the S2C 0x0A
// frame re-broadcasts), mirrors the engine-frame AiEntity, and stages the smooth-target.
// [orig: dispatch_entity_packet_callback @0x4D6A80 -> NetPacket_SerializePlayerState case 4
// @0x4c2042-0x4c20a9; §5.10/§5.38]
bool run_apply_player_intent_stages_remote_peer() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity peer;
	peer.kind = w::EntityKind::Organic;
	peer.item_id = 0x14B9; // player infantry template
	peer.position = {0.0f, 0.0f, 0.0f};
	peer.yaw = 0;
	const w::EntityHandle ph = world.registry.spawn(0, peer);
	if (!expect(ph.valid(), "peer spawned")) return false;

	// The engine-frame mirror. AiSystem is wired (so apply mirrors it) but NOT ticked —
	// this isolates the host read-apply from the motor.
	w::AiSystem ai;
	world.ai = &ai;
	ai.attach(ph);

	// A DIFFERENT handle is the local player, so the read-apply guard does not reject the peer.
	world.cached.local_player = w::EntityHandle::make(0, 7);

	const int32_t wx = w::to_fixed(100.0);
	const int32_t wy = w::to_fixed(200.0);
	const int32_t wz = w::to_fixed(-50.0);
	const int16_t wheading = 0x2000; // BAM-high i16 -> mission yaw 45 deg
	const int16_t wpitch = 0x0100;

	ns::LoopbackChannel channel;
	channel.client_send(0x0C, make_0c_uplink(ph.packed, wx, wy, wz, wheading, wpitch));
	if (!expect(channel.c2s_pending() == 1, "one C2S 0x0C queued")) return false;

	// Drain directly (the authority host's top-of-tick C2S drain). The connection owns peer ph — the
	// owner gate (D-NET-119) requires the uplink handle to match conn.owned_entity for the apply.
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, ph, 0});
	ns::test::drain_all(world, conns, /*is_authority=*/true);
	if (!expect(channel.c2s_pending() == 0, "C2S drained by the tick")) return false;

	// Registry Entity SNAPPED (absolute world pos; no map-origin add on the extended wire).
	const w::Entity *pe = world.registry.get(ph);
	if (!expect(pe != nullptr, "peer still present")) return false;
	if (!expect(pe->position.x == static_cast<float>(w::from_fixed(wx)) &&
	            pe->position.y == static_cast<float>(w::from_fixed(wy)) &&
	            pe->position.z == static_cast<float>(w::from_fixed(wz)),
	            "registry Entity position snapped to the wire pose")) return false;
	if (!expect(pe->yaw == 45, "registry yaw = 90 - BAM/deg (== 45)")) return false;

	// Engine-frame AiEntity mirrored + smooth-target staged + net-snapped.
	const w::AiEntity *ae = ai.for_handle(ph);
	if (!expect(ae != nullptr, "peer has an AiEntity")) return false;
	if (!expect(ae->net_is_remote_peer, "peer marked net-snapped")) return false;
	const int32_t heading_bam = static_cast<int32_t>(wheading) << 16;
	const int32_t pitch_bam = static_cast<int32_t>(wpitch) << 16;
	if (!expect(ae->pos[0] == wx && ae->pos[1] == wy && ae->pos[2] == wz,
	            "AiEntity live pos = wire pose")) return false;
	if (!expect(ae->heading == heading_bam && ae->pitch == pitch_bam,
	            "AiEntity heading/pitch = i16<<16 (pure widen, no 90-offset)")) return false;
	if (!expect(ae->net_smooth_target[0] == wx && ae->net_smooth_target[1] == wy &&
	            ae->net_smooth_target[2] == wz,
	            "smooth-target staged (+0x234/238/23C)")) return false;
	if (!expect(ae->net_smooth_heading == heading_bam && ae->net_smooth_pitch == pitch_bam,
	            "smooth heading/pitch staged (+0x240/244)")) return false;
	if (!expect(ae->net_interp_progress == 0, "interp progress reset (+0x27C)")) return false;
	return true;
}

// Parity-critical negative: the host NEVER read-applies its OWN player (§5.38 / ADR-0012).
bool run_apply_rejects_own_player() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity peer;
	peer.kind = w::EntityKind::Organic;
	peer.item_id = 0x14B9;
	peer.position = {1.0f, 2.0f, 3.0f};
	const w::EntityHandle ph = world.registry.spawn(0, peer);
	w::AiSystem ai;
	world.ai = &ai;
	ai.attach(ph);

	// Make the peer the local player -> the read-apply must reject it.
	world.cached.local_player = ph;

	ns::LoopbackChannel channel;
	channel.client_send(0x0C, make_0c_uplink(ph.packed, w::to_fixed(999.0), 0, 0, 0x4000, 0));
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, ph, 0}); // owner gate passes
	                                     // (handle == owner); the §5.38 local-player refusal is what
	                                     // must reject this self-uplink [D-NET-119]
	ns::test::drain_all(world, conns, /*is_authority=*/true);
	if (!expect(channel.c2s_pending() == 0, "C2S drained even when rejected")) return false;

	const w::Entity *pe = world.registry.get(ph);
	if (!expect(pe->position.x == 1.0f && pe->position.y == 2.0f && pe->position.z == 3.0f,
	            "own-player pose NOT overwritten by a read-apply")) return false;
	const w::AiEntity *ae = ai.for_handle(ph);
	if (!expect(ae != nullptr && !ae->net_is_remote_peer,
	            "own player not marked net-snapped")) return false;
	return true;
}

// The infantry motor SKIPS a net-snapped remote peer entirely — its pose is host-snapped,
// never re-simulated. [orig: Entity_UpdateInfantryAI @0x4b9a03 entity+0x24 bit0 -> full exit.]
bool run_motor_skips_net_peer() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity peer;
	peer.kind = w::EntityKind::Organic;
	peer.item_id = 0x14B9;
	const w::EntityHandle ph = world.registry.spawn(0, peer);
	w::AiSystem ai;
	world.ai = &ai;
	ai.attach(ph);
	w::AiEntity *ae = ai.for_handle(ph);
	if (!expect(ae != nullptr, "peer AiEntity")) return false;

	// Seed state the motor would otherwise change; mark net-snapped so it skips entirely.
	ae->inf.active = true;
	ae->inf.body_heading = 12345;
	ae->inf.target_heading = 9999999;
	ae->pos[0] = 111;
	ae->pos[1] = 222;
	ae->pos[2] = 333;
	ae->heading = 4444;
	ae->net_is_remote_peer = true;
	ai.tick_infantry(*ae, world, 0);

	if (!expect(ae->inf.body_heading == 12345 && ae->inf.target_heading == 9999999,
	            "skip-guard: heading state untouched")) return false;
	if (!expect(ae->pos[0] == 111 && ae->pos[1] == 222 && ae->pos[2] == 333,
	            "skip-guard: live pos untouched")) return false;
	if (!expect(ae->heading == 4444, "skip-guard: engine heading untouched")) return false;
	return true;
}

} // namespace

int main() {
	const bool ok = run() &&
	                run_header_only_records_are_ignored_by_client_view() &&
	                run_apply_player_intent_stages_remote_peer() &&
	                run_apply_rejects_own_player() &&
	                run_motor_skips_net_peer();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
