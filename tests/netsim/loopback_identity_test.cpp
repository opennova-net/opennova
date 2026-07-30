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

#include <npwire/ingame_decode.h> // EntityPacketSubHeader / PlayerExtendedUplink
#include <npwire/ingame_message_id.h>
#include <npwire/ingame_encode.h> // network_compress_fixedpoint, encode_* uplink
#include <world/ai.h>                 // AiSystem / AiEntity (engine-frame mirror)
#include <world/entity.h>
#include <world/geom.h>
#include <world/vehicle_attach.h>
#include <world/world.h>

#include <cstdint>
#include <cstdlib>
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

bool run_client_state_handle_lookup_contract() {
	ns::ClientState state;
	for (uint16_t handle = 0; handle < 1024; ++handle) {
		ns::ClientEntityState &entity = state.upsert(handle);
		entity.type_id = static_cast<uint16_t>(handle + 100);
	}

	if (!expect(state.entities.size() == 1024,
	            "upsert keeps one entity per handle")) return false;
	for (uint16_t handle = 0; handle < 1024; ++handle) {
		ns::ClientEntityState *entity = state.find(handle);
		if (!expect(entity != nullptr && entity->handle == handle &&
		                    entity->type_id == static_cast<uint16_t>(handle + 100),
		            "find resolves the original entity")) return false;
	}

	ns::ClientEntityState &existing = state.upsert(512);
	existing.x = 1234;
	if (!expect(state.entities.size() == 1024 && state.entities[512].x == 1234,
	            "duplicate upsert reuses the original insertion-order slot")) return false;
	if (!expect(state.find(0xFFFF) == nullptr,
	            "find preserves the missing-handle result")) return false;

	// `entities` is a public decoded-state surface. Lookup must follow direct
	// mutations rather than retaining stale indexes into the vector.
	state.entities.clear();
	if (!expect(state.find(512) == nullptr,
	            "find observes a direct vector clear")) return false;
	state.entities.push_back(ns::ClientEntityState{});
	state.entities.back().handle = 77;
	if (!expect(state.find(77) == &state.entities.front(),
	            "find observes a directly appended entity")) return false;
	state.entities.front().handle = 88;
	if (!expect(state.find(77) == nullptr &&
	                    state.find(88) == &state.entities.front(),
	            "find observes a direct handle mutation")) return false;
	state.entities.insert(state.entities.begin(), ns::ClientEntityState{});
	state.entities.front().handle = 99;
	if (!expect(state.find(88) == &state.entities[1] &&
	                    state.find(99) == &state.entities[0],
	            "find remains correct after direct reordering")) return false;
	return true;
}

// The load-time world stream writes entity+354 for all four pools and the
// pool-1 zone block writes entity+538/+350. These decoded values must survive
// in the same ClientEntityState rows that later S2C 0x50 team assignments
// mutate. Flag omission denotes zero in retail, so a later zero-valued record
// must clear a previously witnessed non-zero value rather than preserve it.
bool run_world_stream_team_and_zone_fields_survive_client_fold() {
	ns::NetClientView view;

	nw::PoolSpawnRecord pool1;
	pool1.slot_id = 0x1007;
	pool1.item_type_id = 0x054F;
	pool1.team_byte = 2;
	pool1.zone_number_rank = 0x22; // zone 2, chain rank 1
	pool1.zone_radius = 70;
	nw::PoolSpawnBatch pool1_batch;
	pool1_batch.records.push_back(pool1);
	view.apply(0x0D, nw::encode_pool_spawn_batch(pool1_batch));

	const ns::ClientEntityState *decoded = view.state().find(0x1007);
	if (!expect(decoded != nullptr && decoded->team == 2 &&
	                    decoded->zone_number_rank == 0x22 &&
	                    decoded->zone_radius == 70,
	            "pool-1 retains team and packed zone block")) return false;

	const std::size_t row_count = view.state().entities.size();
	view.apply_team_assign(0x1007, 1);
	decoded = view.state().find(0x1007);
	if (!expect(view.state().entities.size() == row_count && decoded != nullptr &&
	                    decoded->team == 1 &&
	                    decoded->zone_number_rank == 0x22 &&
	                    decoded->zone_radius == 70,
	            "S2C 0x50 mutates the same pool-1 row without disturbing zone state"))
		return false;

	nw::StaticEntityRecord pool2;
	pool2.item_type_id = 0x0600;
	pool2.team_byte = 1;
	nw::StaticEntityBatch pool2_batch;
	pool2_batch.start_index = 4;
	pool2_batch.records.push_back(pool2);
	view.apply(0x10, nw::encode_static_entity_batch(pool2_batch));
	decoded = view.state().find(0x2004);
	if (!expect(decoded != nullptr && decoded->team == 1,
	            "pool-2 retains team")) return false;

	nw::Pool3SyncRecord pool3;
	pool3.item_type_id = 0x0700;
	pool3.net_handle = 0x3005;
	pool3.team_byte = 2;
	nw::Pool3SyncBatch pool3_batch;
	pool3_batch.start_index = 5;
	pool3_batch.records.push_back(pool3);
	view.apply(0x20, nw::encode_pool3_sync_batch(pool3_batch));
	decoded = view.state().find(0x3005);
	if (!expect(decoded != nullptr && decoded->team == 2,
	            "pool-3 retains team")) return false;

	// Re-encode the same rows with all flag-gated values zero. The encoders omit
	// those fields and the decoders surface zero, which the client fold must apply.
	pool1.team_byte = 0;
	pool1.zone_number_rank = 0;
	pool1.zone_radius = 0;
	pool1_batch.records[0] = pool1;
	view.apply(0x0D, nw::encode_pool_spawn_batch(pool1_batch));
	decoded = view.state().find(0x1007);
	if (!expect(decoded != nullptr && decoded->team == 0 &&
	                    decoded->zone_number_rank == 0 &&
	                    decoded->zone_radius == 0,
	            "pool-1 applies omitted team and zone fields as zero")) return false;

	pool2.team_byte = 0;
	pool2_batch.records[0] = pool2;
	view.apply(0x10, nw::encode_static_entity_batch(pool2_batch));
	decoded = view.state().find(0x2004);
	if (!expect(decoded != nullptr && decoded->team == 0,
	            "pool-2 applies omitted team as zero")) return false;

	pool3.team_byte = 0;
	pool3_batch.records[0] = pool3;
	view.apply(0x20, nw::encode_pool3_sync_batch(pool3_batch));
	decoded = view.state().find(0x3005);
	if (!expect(decoded != nullptr && decoded->team == 0,
	            "pool-3 applies omitted team as zero")) return false;

	view.apply_team_assign(0x1007, 0);
	decoded = view.state().find(0x1007);
	if (!expect(decoded != nullptr && decoded->team == 0,
	            "S2C 0x50 applies a zero team to the existing row")) return false;
	return true;
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

// The present pass consumes the client view, never the authoritative world (ADR 0011).
// Keep the already-witnessed compact pose/mount bytes alive across that public fold so a
// remote organic can select the same mounted overlay as the sender. The normalized fields
// deliberately retain the raw wire values: carrier_handle is the class-specific parent,
// and the player-only/infantry-only bytes are cleared when the class does not carry them.
bool run_compact_pose_fields_survive_client_fold() {
	nw::FrameUpdate mounted;
	mounted.flags2 = 0;
	mounted.mount_handle = 0xFFFF;
	mounted.health = 100;

	nw::FrameUpdateRecord player;
	player.handle = 0x0001;
	player.type_id = 0x14B9;
	player.cls = nw::EntityClass::Player;
	player.player.vehicle_bone = 5;
	player.player.seat_type = 2;
	player.player.carrier_handle = 0x1007;
	player.player.pitch_byte = 0x21;
	player.player.anim_state_id = 62;
	player.player.anim_channel_ratio = 19;
	player.player.health_class_byte = 0x28;
	mounted.records.push_back(player);

	nw::FrameUpdateRecord infantry;
	infantry.handle = 0x0002;
	infantry.type_id = 0x2000;
	infantry.cls = nw::EntityClass::Infantry;
	infantry.infantry.seat_bone_idx = 3;
	infantry.infantry.vehicle_slot_handle = 0x1008;
	infantry.infantry.pitch_byte = 0x31;
	infantry.infantry.aim_yaw_byte = 0xF4;
	infantry.infantry.anim_byte = 47;
	mounted.records.push_back(infantry);

	ns::NetClientView view([](uint16_t type_id) {
		return type_id == 0x14B9 ? nw::EntityClass::Player : nw::EntityClass::Infantry;
	});
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(mounted));

	const ns::ClientEntityState *p = view.state().find(0x0001);
	if (!expect(p != nullptr && p->carrier_handle == 0x1007 && p->mount_bone == 5 &&
	                    p->seat_type == 2 && p->pitch_byte == 0x21 &&
	                    p->aim_yaw_byte == 0 && p->anim_state_id == 62 &&
	                    p->anim_channel_ratio == 19,
	            "player compact mount/pose bytes survive the client fold")) return false;
	const ns::ClientEntityState *i = view.state().find(0x0002);
	if (!expect(i != nullptr && i->carrier_handle == 0x1008 && i->mount_bone == 3 &&
	                    i->seat_type == 0 && i->pitch_byte == 0x31 &&
	                    i->aim_yaw_byte == 0xF4 && i->anim_state_id == 47 &&
	                    i->anim_channel_ratio == 0 &&
	                    i->pitch_bam == static_cast<int32_t>(0xFE800000u),
	            "infantry compact target advances live pitch by retail's one-eighth chase"))
		return false;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(mounted));
	i = view.state().find(0x0002);
	if (!expect(i != nullptr &&
	                    i->pitch_bam == static_cast<int32_t>(0xFD300000u),
	            "successive mounted frames continue the stateful pitch chase"))
		return false;

	// A subsequent free-standing record is the dismount signal. Overwrite every
	// normalized field; stale carrier/bone bytes must never select yesterday's mount.
	nw::FrameUpdate dismounted = mounted;
	dismounted.records.clear();
	player.player.vehicle_bone = 0;
	player.player.seat_type = 0;
	player.player.carrier_handle = 0xFFFF;
	dismounted.records.push_back(player);
	infantry.infantry.seat_bone_idx = 0;
	infantry.infantry.vehicle_slot_handle = 0xFFFF;
	dismounted.records.push_back(infantry);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(dismounted));
	p = view.state().find(0x0001);
	i = view.state().find(0x0002);
	if (!expect(p != nullptr && p->carrier_handle == 0xFFFF && p->mount_bone == 0 &&
	                    p->seat_type == 0,
	            "player dismount clears retained selector fields")) return false;
	if (!expect(i != nullptr && i->carrier_handle == 0xFFFF && i->mount_bone == 0,
	            "infantry dismount clears retained selector fields")) return false;
	if (!expect(i->pitch_bam == static_cast<int32_t>(0xFD300000u),
	            "infantry dismount retains the last reconstructed live pitch")) return false;
	return true;
}

// The other locally integrated remote field. Only move_input bits 6/7 ride the wire;
// each end integrates the angle itself, in the retail body pass's DECAY-then-RAMP
// order. Ramp-then-decay sheds a sixteenth of every ramp step on the same tick that
// applies it and settles a whole step short (~±0x2D000000 ≈ 4.2° flatter), so both the
// first tick and the equilibrium are pinned here.
// [orig: decay lean -= (lean+8)>>4 @0x4b5c97, then the on-foot ramp @0x4b7dbf/@0x4b7dd6
//  — one Entity_UpdateInfantryPlayerBody @0x4b40e0 pass]
bool run_remote_lean_integrator_decays_before_ramping() {
	nw::FrameUpdate frame;
	frame.flags2 = 0;
	frame.mount_handle = 0xFFFF;

	nw::FrameUpdateRecord left;
	left.handle = 0x0001;
	left.type_id = 0x14B9;
	left.cls = nw::EntityClass::Player;
	left.player.carrier_handle = 0xFFFF;
	left.player.anim_def_index = 0xFF;
	left.player.move_input_byte = 0x40; // lean LEFT held
	frame.records.push_back(left);
	nw::FrameUpdateRecord right = left;
	right.handle = 0x0002;
	right.player.move_input_byte = 0x80; // lean RIGHT held
	frame.records.push_back(right);

	ns::NetClientView view([](uint16_t) { return nw::EntityClass::Player; });
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(frame));
	const ns::ClientEntityState *l = view.state().find(0x0001);
	const ns::ClientEntityState *r = view.state().find(0x0002);
	if (!expect(l != nullptr && r != nullptr && l->move_input == 0x40 &&
	                    r->move_input == 0x80 && l->lean_angle == 0 && r->lean_angle == 0,
	            "lean bits decode off the compact record, angle unintegrated")) return false;

	// Tick 1 from a level angle: the decay takes (0+8)>>4 == 0, so the ramp step lands
	// whole. Ramp-first would already read ∓0x2D00000.
	view.tick_lean();
	if (!expect(l->lean_angle == -0x3000000 && r->lean_angle == 0x3000000,
	            "first tick decays the level angle, then applies one whole ramp step"))
		return false;

	// Held: the decay cancels exactly one ramp step at ~16x it, not ~15x.
	for (int tick = 1; tick < 400; ++tick) view.tick_lean();
	if (!expect(l->lean_angle < -0x2F000000 && l->lean_angle > -0x31000000 &&
	                    r->lean_angle > 0x2F000000 && r->lean_angle < 0x31000000,
	            "held lean settles on the witnessed ±0x30000000 equilibrium")) return false;

	// Bits cleared by the next frame: the decay keeps running for the row, so the angle
	// returns to level (the ramp is the only gated half).
	const int32_t held_left = l->lean_angle;
	frame.records[0].player.move_input_byte = 0;
	frame.records[1].player.move_input_byte = 0;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(frame));
	l = view.state().find(0x0001);
	r = view.state().find(0x0002);
	view.tick_lean();
	if (!expect(l != nullptr && r != nullptr && l->lean_angle > held_left &&
	                    l->lean_angle < 0 && r->lean_angle > 0,
	            "released lean decays back toward level")) return false;
	// The decay's +8 rounding parks the tail a few BAM units off zero rather than on it.
	for (int tick = 1; tick < 400; ++tick) view.tick_lean();
	return expect(l->lean_angle > -0x1000 && l->lean_angle <= 0 &&
	                      r->lean_angle >= 0 && r->lean_angle < 0x1000,
	              "released lean settles level");
}

nw::FrameUpdate compact_lifecycle_frame(uint8_t player_flags,
		uint8_t infantry_flags) {
	nw::FrameUpdate frame;
	frame.flags2 = 0;
	frame.mount_handle = 0xFFFF;
	frame.health = 100;

	nw::FrameUpdateRecord player;
	player.handle = 0x0001;
	player.type_id = 0x14B9;
	player.cls = nw::EntityClass::Player;
	player.player.carrier_handle = 0xFFFF;
	player.player.state_flags = player_flags;
	player.player.anim_def_index = 0xFF;
	frame.records.push_back(player);

	nw::FrameUpdateRecord infantry;
	infantry.handle = 0x0002;
	infantry.type_id = 0x2000;
	infantry.cls = nw::EntityClass::Infantry;
	infantry.infantry.vehicle_slot_handle = 0xFFFF;
	infantry.infantry.flags_byte = infantry_flags;
	frame.records.push_back(infantry);
	return frame;
}

// Compact organic flags are the remote lifecycle authority: bit 0 hides and
// bit 1 marks dead/undeployed. A render pass can trail the network pump, so the
// decoded state also carries a monotonic dead->alive epoch; final alive state
// alone would lose two complete respawns folded below by one pump().
bool run_compact_lifecycle_survives_multi_frame_pump() {
	ns::LoopbackChannel channel;
	ns::NetClientView view([](uint16_t type_id) {
		return type_id == 0x14B9 ? nw::EntityClass::Player :
				nw::EntityClass::Infantry;
	});

	// An initially witnessed dead record establishes the known state, but is not
	// itself a respawn edge. Retain high/raw bits rather than normalizing the byte.
	channel.host_send(nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(compact_lifecycle_frame(0x83, 0x42)));
	view.pump(channel);
	const ns::ClientEntityState *player = view.state().find(0x0001);
	const ns::ClientEntityState *infantry = view.state().find(0x0002);
	if (!expect(player != nullptr && player->state_flags_known &&
				player->state_flags == 0x83 && player->respawn_revision == 0,
			"initial player lifecycle sample is known without a false respawn"))
		return false;
	if (!expect(infantry != nullptr && infantry->state_flags_known &&
				infantry->state_flags == 0x42 && infantry->respawn_revision == 0,
			"initial infantry lifecycle sample retains its raw flag byte"))
		return false;

	// alive, dead, alive: two dead->alive edges for each organic, all folded by
	// one pump before presentation gets a chance to inspect final state.
	channel.host_send(nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(compact_lifecycle_frame(0x80, 0x41)));
	channel.host_send(nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(compact_lifecycle_frame(0x82, 0x43)));
	channel.host_send(nw::s2c::PER_FRAME_UPDATE,
			nw::encode_frame_update(compact_lifecycle_frame(0x81, 0x40)));
	view.pump(channel);
	player = view.state().find(0x0001);
	infantry = view.state().find(0x0002);
	if (!expect(view.frames_applied() == 4,
			"one pump folds every queued lifecycle frame")) return false;
	if (!expect(player != nullptr && player->state_flags == 0x81 &&
				(player->state_flags & 0x01u) != 0u &&
				(player->state_flags & 0x02u) == 0u &&
				player->respawn_revision == 2,
			"player final flags and both respawn edges survive the pump"))
		return false;
	if (!expect(infantry != nullptr && infantry->state_flags == 0x40 &&
				(infantry->state_flags & 0x01u) == 0u &&
				(infantry->state_flags & 0x02u) == 0u &&
				infantry->respawn_revision == 2,
			"infantry final flags and both respawn edges survive the pump"))
		return false;
	return true;
}

// The host's registry order emits pool-0 organics before their pool-1 carriers.
// A frame is one state sample, so a child record must lift through the carrier's
// pose from that same frame even when the parent record appears later. A truly
// absent carrier keeps the prior world pose (retail drops that pose sample).
bool run_carrier_local_pose_lifts_after_later_carrier_record() {
	nw::FrameUpdate frame;
	frame.flags2 = 0;
	frame.mount_handle = 0xFFFF;
	frame.health = 100;

	nw::FrameUpdateRecord child;
	child.handle = 0x0002;
	child.type_id = 0x2000;
	child.cls = nw::EntityClass::Infantry;
	child.infantry.seat_bone_idx = 3;
	child.infantry.vehicle_slot_handle = 0x1007;
	child.infantry.pos_x_compressed = nw::network_compress_fixedpoint(1 << 16);
	child.infantry.pos_y_compressed = nw::network_compress_fixedpoint(2 << 16);
	child.infantry.pos_z_compressed = nw::network_compress_fixedpoint(3 << 16);
	child.infantry.yaw_byte = 0x10;
	child.infantry.anim_byte = 47;
	frame.records.push_back(child); // deterministic production order: child first

	nw::FrameUpdateRecord carrier;
	carrier.handle = 0x1007;
	carrier.type_id = 0x1004;
	carrier.cls = nw::EntityClass::Vehicle;
	carrier.vehicle.parent_slot_handle = 0xFFFF;
	carrier.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(0);
	carrier.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(0);
	carrier.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(0);
	carrier.vehicle.euler_z = 0;
	carrier.vehicle.flags_byte = 0;
	carrier.vehicle.health_word = 3000;
	frame.records.push_back(carrier);

	auto classify = [](uint16_t type_id) {
		return type_id == 0x1004 ? nw::EntityClass::Vehicle : nw::EntityClass::Infantry;
	};
	ns::NetClientView view(classify);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(frame));
	const ns::ClientEntityState *decoded = view.state().find(0x0002);
	if (!expect(decoded != nullptr && decoded->x == (1 << 16) &&
	                    decoded->y == (2 << 16) && decoded->z == (3 << 16) &&
	                    decoded->yaw_byte == 0x10,
	            "child-first record lifts through later same-frame carrier")) return false;

	// Change the local pose/yaw but name a carrier that never appears. The client
	// keeps the last resolved world pose instead of treating local coordinates as world.
	nw::FrameUpdate missing;
	missing.flags2 = 0;
	missing.mount_handle = 0xFFFF;
	missing.health = 100;
	child.infantry.vehicle_slot_handle = 0x1008;
	child.infantry.pos_x_compressed = nw::network_compress_fixedpoint(9 << 16);
	child.infantry.pos_y_compressed = nw::network_compress_fixedpoint(8 << 16);
	child.infantry.pos_z_compressed = nw::network_compress_fixedpoint(7 << 16);
	child.infantry.yaw_byte = 0x44;
	missing.records.push_back(child);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(missing));
	decoded = view.state().find(0x0002);
	if (!expect(decoded != nullptr && decoded->x == (1 << 16) &&
	                    decoded->y == (2 << 16) && decoded->z == (3 << 16) &&
	                    decoded->yaw_byte == 0x10,
	            "missing carrier drops the local pose sample")) return false;
	return true;
}

// Vehicle live compacts omit pitch/roll; a remote seated body still needs the
// carrier's last authored/full orientation. Retain spawn Euler X/Y, update them
// when the existing dead-pose compact carries new high words, and do not zero
// them merely because a live compact has no such fields.
bool run_carrier_pitch_roll_persists_across_live_records() {
	nw::PoolSpawnBatch spawn;
	nw::PoolSpawnRecord vehicle;
	vehicle.slot_id = 0x1007;
	vehicle.item_type_id = 0x1004;
	vehicle.euler_z = 0x10000000;
	vehicle.euler_x = 0x23456789;
	vehicle.euler_y = static_cast<int32_t>(0xD1234567u);
	spawn.records.push_back(vehicle);

	ns::NetClientView view;
	view.apply(0x0D, nw::encode_pool_spawn_batch(spawn));
	const ns::ClientEntityState *carrier = view.state().find(0x1007);
	if (!expect(carrier != nullptr && carrier->pitch_bam == 0x23456789 &&
	                    carrier->roll_bam == static_cast<int32_t>(0xD1234567u),
	            "carrier spawn retains full pitch and roll BAM")) return false;

	nw::FrameUpdate live;
	live.flags2 = 0;
	live.mount_handle = 0xFFFF;
	live.health = 100;
	nw::FrameUpdateRecord live_vehicle;
	live_vehicle.handle = 0x1007;
	live_vehicle.type_id = 0x1004;
	live_vehicle.cls = nw::EntityClass::Vehicle;
	live_vehicle.vehicle.parent_slot_handle = 0xFFFF;
	live_vehicle.vehicle.euler_z = 0x3000;
	live_vehicle.vehicle.flags_byte = 0;
	live_vehicle.vehicle.health_word = 3000;
	live.records.push_back(live_vehicle);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(live));
	carrier = view.state().find(0x1007);
	if (!expect(carrier != nullptr && carrier->pitch_bam == 0x23456789 &&
	                    carrier->roll_bam == static_cast<int32_t>(0xD1234567u),
	            "live vehicle record preserves last full pitch and roll")) return false;

	nw::FrameUpdate dead = live;
	dead.records.clear();
	live_vehicle.vehicle.flags_byte = 0x04;
	live_vehicle.vehicle.euler_x = 0x1234;
	live_vehicle.vehicle.euler_y = static_cast<int16_t>(-0x2345);
	dead.records.push_back(live_vehicle);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(dead));
	carrier = view.state().find(0x1007);
	if (!expect(carrier != nullptr && carrier->pitch_bam == 0x12340000 &&
	                    carrier->roll_bam == static_cast<int32_t>(0xDCBB0000u),
	            "dead-pose vehicle record refreshes full pitch and roll")) return false;
	return true;
}

// An items.def addeweap child is a real pool-1 entity, but its ewep callback is
// intentionally NoNetworkCallback: retail carries the exact parent relation in
// the load-time 0x0D record and the client keeps the child attached locally.
// Prove that production 0x0D -> ClientState relationship drives later parent
// motion, and that the parent's replicated death retires the child rather than
// leaving its spawn pose in the presented client view forever.
bool run_parented_pool_spawn_follows_and_retires() {
	w::World world;
	world.registry.configure_pool(1, 8);

	w::Entity parent_seed;
	parent_seed.kind = w::EntityKind::Item;
	parent_seed.item_id = 0x1004;
	parent_seed.position = {10.0f, 20.0f, -5.0f};
	parent_seed.health = 3000;
	parent_seed.health_max = 3000;
	parent_seed.flags = 0x02u; // overloaded load/movement gate, not a death verdict
	parent_seed.net_class_code = static_cast<uint8_t>(nw::EntityClass::Vehicle);
	const w::EntityHandle parent_h = world.registry.spawn(1, parent_seed);
	if (!expect(parent_h.valid(), "attachment parent spawned")) return false;

	w::Entity child_seed;
	child_seed.kind = w::EntityKind::Item;
	child_seed.item_id = 0x0666;
	child_seed.position = {12.0f, 20.0f, -5.0f};
	child_seed.spawn_origin = 0xFFFFFFFFu;
	child_seed.net_class_code =
			static_cast<uint8_t>(nw::EntityClass::NoNetworkCallback);
	child_seed.emplacement_parent = parent_h;
	child_seed.emplacement_parent_spawn_id =
			world.registry.get(parent_h)->registry_spawn_id;
	child_seed.emplacement_local = {2.0f, 0.0f, 0.0f};
	const w::EntityHandle child_h = world.registry.spawn(1, child_seed);
	if (!expect(child_h.valid(), "attachment child spawned")) return false;

	ns::NetClientView view;
	view.set_item_class_resolver([](uint16_t type_id) {
		if (type_id == 0x1004) return nw::EntityClass::Vehicle;
		if (type_id == 0x0666) return nw::EntityClass::NoNetworkCallback;
		return nw::EntityClass::Unknown;
	});
	view.apply(0x0D, nw::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(world)));
	const ns::ClientEntityState *decoded_child = view.state().find(child_h.packed);
	const ns::ClientEntityState *decoded_parent = view.state().find(parent_h.packed);
	if (!expect(decoded_child != nullptr && decoded_parent != nullptr &&
	                    decoded_child->parent_handle == parent_h.packed,
	            "0x0D retains the synthetic child's exact parent handle")) return false;
	const int32_t initial_child_x = decoded_child->x;
	const int32_t initial_parent_delta_x = decoded_child->x - decoded_parent->x;

	// The authoritative world poses the attachment after its carrier moves. The
	// child still emits no 0x0A compact body; its decoded pose must follow through
	// the retained parent relation instead of freezing at the 0x0D spawn sample.
	w::Entity *parent = world.registry.get(parent_h);
	parent->position.x = 20.0f;
	world.run_logic_tick();
	const w::Entity *moved_child = world.registry.get(child_h);
	if (!expect(moved_child != nullptr && moved_child->position.x == 22.0f,
	            "authoritative attachment followed the moving parent")) return false;

	ns::LoopbackChannel channel;
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, {}, 0});
	nw::PlayerReplicationState anchor;
	ns::test::emit_all(world, conns, anchor);
	view.pump(channel);
	decoded_child = view.state().find(child_h.packed);
	decoded_parent = view.state().find(parent_h.packed);
	if (!expect(decoded_child != nullptr && decoded_parent != nullptr &&
	                    decoded_child->x != initial_child_x &&
	                    std::abs((decoded_child->x - decoded_parent->x) -
	                             initial_parent_delta_x) < 128,
	            "NoNetworkCallback child follows the decoded parent pose")) return false;

	// A replicated vehicle death carries a zero health word in its ordinary
	// compact. The authority retires the synthetic child, and the decoded parent
	// relationship lets the client retire the same subtree without inventing a
	// destroy packet for a message whose retail transaction is unrelated.
	parent->health = 0;
	parent->alive = false;
	world.run_logic_tick();
	if (!expect(world.registry.get(child_h) == nullptr,
	            "authoritative parent death despawned the attachment")) return false;
	ns::test::emit_all(world, conns, anchor);
	view.pump(channel);
	if (!expect(view.state().find(child_h.packed) == nullptr,
	            "decoded zero-health parent retires the attachment subtree")) return false;
	return true;
}

// Retail's InfantryCompactRecord already carries every remote mounted-selector input
// available on the wire. Exercise the production world -> snapshot -> connection fan ->
// codec -> client-view path: no collision-only or out-of-band replication heuristic.
bool run_mounted_infantry_pose_fields_round_trip() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	w::AiSystem ai;
	world.ai = &ai;

	w::Entity soldier;
	soldier.kind = w::EntityKind::Organic;
	soldier.item_id = 0x2000;
	soldier.position = {0.0f, 0.0f, 0.0f};
	soldier.yaw = 90; // engine heading 0: makes the witnessed target-heading clamp literal.
	const w::EntityHandle ih = world.registry.spawn(0, soldier);
	if (!expect(ih.valid(), "infantry spawned")) return false;
	ai.attach(ih);
	w::AiEntity *ae = ai.for_handle(ih);
	if (!expect(ae != nullptr, "infantry AI state attached")) return false;
	ae->inf.target_heading = 0x40000000; // clamps to +0x1FFFFFE0, rounds to byte 0x20.
	ae->inf.aim_pitch = static_cast<int32_t>(0xF0000000u); // rounded high byte 0xF0.
	w::Entity *infantry_entity = world.registry.get(ih);
	if (!expect(infantry_entity != nullptr, "infantry entity resolvable")) return false;
	infantry_entity->net_anim_state = 47;

	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 0x1004;
	vehicle.position = {100.0f, 200.0f, 10.0f};
	vehicle.yaw = 90; // identity engine-frame carrier rotation
	vehicle.pitch = 15;
	vehicle.roll = -10;
	vehicle.health = 3000;
	vehicle.health_max = 3000;
	vehicle.net_class_code = static_cast<uint8_t>(nw::EntityClass::Vehicle);
	w::Seat seat;
	seat.type = w::SeatType::Gunner;
	seat.bone_index = 3;
	vehicle.seats.push_back(seat); // zero local pose: occupant lands at carrier origin.
	const w::EntityHandle vh = world.registry.spawn_from(1, 0, vehicle);
	if (!expect(vh.valid(), "carrier spawned")) return false;
	if (!expect(w::entity_process_vehicle_attach(world, ih, vh, 3),
	            "infantry attaches to witnessed wire bone")) return false;
	if (!expect(ai.pose_if_mounted(*ae, world),
	            "mounted infantry synchronizes to the seat frame")) return false;

	ns::LoopbackChannel channel;
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, {}, 0});
	nw::PlayerReplicationState fallback;
	ns::test::emit_all(world, conns, fallback);
	ns::Datagram dg;
	if (!expect(channel.client_recv(dg), "mounted infantry frame dequeued")) return false;

	auto classify = [](uint16_t type_id) {
		if (type_id == 0x1004) return nw::EntityClass::Vehicle;
		return nw::EntityClass::Infantry;
	};
	nw::FrameUpdate frame;
	if (!expect(nw::decode_frame_update(dg.body.data(), dg.body.size(), classify, frame),
	            "mounted infantry frame decodes")) return false;
	const nw::FrameUpdateRecord *wire = nullptr;
	for (const nw::FrameUpdateRecord &record : frame.records)
		if (record.handle == ih.packed) wire = &record;
	if (!expect(wire != nullptr, "mounted infantry record present")) return false;
	if (!expect(wire->infantry.seat_bone_idx == 3 &&
	                    wire->infantry.vehicle_slot_handle == vh.packed,
	            "mounted infantry carries existing bone and carrier fields")) return false;
	if (!expect(wire->infantry.pos_x_compressed == 0 &&
	                    wire->infantry.pos_y_compressed == 0 &&
	                    wire->infantry.pos_z_compressed == 0,
	            "mounted infantry position is carrier-local")) return false;
	// UseGun is not a generic vehicle slot: retail clears 0xA000 but does not
	// set entity Flags 0x40. [orig: Entity_AttachToUseGunSlot @0x546c5c]
	if (!expect((wire->infantry.flags_byte & 0x40u) == 0,
	            "UseGun infantry leaves the generic vehicle-seat flag clear")) return false;
	if (!expect(wire->infantry.pitch_byte == 0x20 &&
	                    wire->infantry.aim_yaw_byte == 0xF0 &&
	                    wire->infantry.anim_byte == 47,
	            "mounted infantry carries witnessed aim and animation bytes")) return false;

	// Seed the carrier through its existing pool-1 spawn before applying the local
	// infantry sample, matching a real client that has completed load sync.
	ns::NetClientView view(classify);
	view.apply(0x0D, nw::encode_pool_spawn_batch(ns::build_pool1_spawn_batch(world)));
	const ns::ClientEntityState *decoded_carrier = view.state().find(vh.packed);
	if (!expect(decoded_carrier != nullptr && decoded_carrier->pitch_bam == 178956960 &&
	                    decoded_carrier->roll_bam == -119304640,
	            "production carrier spawn retains authored pitch and roll")) return false;
	view.apply(nw::s2c::PER_FRAME_UPDATE, dg.body);
	const ns::ClientEntityState *decoded = view.state().find(ih.packed);
	if (!expect(decoded != nullptr && decoded->carrier_handle == vh.packed &&
	                    decoded->mount_bone == 3 && decoded->pitch_byte == 0x20 &&
	                    decoded->aim_yaw_byte == 0xF0 && decoded->anim_state_id == 47 &&
	                    decoded->pitch_bam == static_cast<int32_t>(0xFE000000u),
	            "mounted infantry selector and chased live pitch survive the production fold"))
		return false;
	decoded_carrier = view.state().find(vh.packed);
	if (!expect(decoded_carrier != nullptr && decoded->x == decoded_carrier->x &&
	                    decoded->y == decoded_carrier->y && decoded->z == decoded_carrier->z,
	            "client lifts carrier-local infantry pose through current decoded carrier"))
		return false;

	if (!expect(w::entity_detach_from_vehicle(world, ih), "infantry detaches")) return false;
	ns::test::emit_all(world, conns, fallback);
	if (!expect(channel.client_recv(dg), "dismounted infantry frame dequeued")) return false;
	view.apply(nw::s2c::PER_FRAME_UPDATE, dg.body);
	decoded = view.state().find(ih.packed);
	if (!expect(decoded != nullptr && decoded->carrier_handle == 0xFFFF &&
	                    decoded->mount_bone == 0,
	            "production dismount clears decoded infantry selector")) return false;
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
	channel.host_send(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
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
	const bool ok = run_client_state_handle_lookup_contract() &&
	                run_world_stream_team_and_zone_fields_survive_client_fold() &&
	                run() &&
	                run_compact_pose_fields_survive_client_fold() &&
	                run_compact_lifecycle_survives_multi_frame_pump() &&
	                run_carrier_local_pose_lifts_after_later_carrier_record() &&
	                run_carrier_pitch_roll_persists_across_live_records() &&
	                run_parented_pool_spawn_follows_and_retires() &&
	                run_mounted_infantry_pose_fields_round_trip() &&
	                run_remote_lean_integrator_decays_before_ramping() &&
	                run_header_only_records_are_ignored_by_client_view() &&
	                run_apply_player_intent_stages_remote_peer() &&
	                run_apply_rejects_own_player() &&
	                run_motor_skips_net_peer();
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
