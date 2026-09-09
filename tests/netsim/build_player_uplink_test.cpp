// build_player_uplink (engine/runtime/replication) is the JOINER-side inverse of apply_player_intent: it
// synthesizes the C2S 0x0C extended (type-10) uplink BODY from the joiner's own live
// local-player state. This proves the full joiner->host round-trip — build the uplink from a
// source pose, encode it (+ the 5-B sub-header), decode it, and read-apply it to the joiner's
// peer entity on a host World: the peer SNAPs to exactly the source pose (i.e. the joiner
// moves on the host). [orig: Player_BuildTag0CInputBody @0x42A550; inverse of
// NetPacket_SerializePlayerState case 4 @0x4c2042-0x4c20a9.]

#include <runtime/replication/connection_fan.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/inmatch/loopback_channel.h>

#include "conn_fan_test_util.h"

#include <net/npwire/ingame_decode.h> // EntityPacketSubHeader / PlayerExtendedUplink
#include <net/npwire/ingame_encode.h> // encode_entity_packet_sub_header / encode_player_extended_uplink
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace w = opennova::world;
namespace ns = opennova::replication;
namespace nw = opennova;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr int64_t kBamPerDegree = 11930464;

int32_t carrier_heading_bam(const w::Entity &carrier) {
	return static_cast<int32_t>(
			static_cast<int64_t>(90 - carrier.yaw) * kBamPerDegree);
}

int32_t carrier_axis_bam(int16_t degrees) {
	return static_cast<int32_t>(static_cast<int64_t>(degrees) * kBamPerDegree);
}

bool drain_built_uplink(w::World &host, w::EntityHandle peer,
		const nw::PlayerExtendedUplink &up) {
	nw::EntityPacketSubHeader sub;
	sub.handle = peer.packed;
	sub.item_type_id = 0x14B9;
	sub.sub_op = 0x0A;
	std::vector<uint8_t> payload = nw::encode_entity_packet_sub_header(sub);
	const std::vector<uint8_t> body = nw::encode_player_extended_uplink(up);
	payload.insert(payload.end(), body.begin(), body.end());

	ns::LoopbackChannel channel;
	channel.client_send(0x0C, payload);
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{
			&channel, ns::TransportMode::Loopback, peer, 0});
	ns::test::drain_all(host, conns, /*is_authority=*/true);
	return channel.c2s_pending() == 0;
}

bool near_fixed(int32_t a, int32_t b, int32_t tolerance = 8) {
	return std::abs(static_cast<int64_t>(a) - static_cast<int64_t>(b)) <= tolerance;
}

// The state->struct field mapping (the inverse of apply_player_intent's reads).
bool run_field_mapping() {
	w::World source_world;
	w::Entity src_e{};
	src_e.net_move_input = 3; // the +0x12C movement-input byte (NOT the visual anim slot)
	src_e.equipped_adm_index = 0x08; // entity+0x2B0 — the wire byte-24 source (D-NET-143)
	// entity+0x24 low byte, raw: bit4 scope + bit3 binoculars + an out-of-mask bit that
	// must still ride the wire unmasked (the original writes the byte whole).
	src_e.flags = 0x10u | 0x08u | 0x20u;
	w::AiEntity src_ae{};
	src_ae.pos[0] = w::to_fixed(12.5);
	src_ae.pos[1] = w::to_fixed(-34.0);
	src_ae.pos[2] = w::to_fixed(5.25);
	src_ae.heading = 0x20000000; // BAM32 with zero low-16 -> exact i16 round-trip
	src_ae.pitch = 0x01000000;

	const nw::PlayerExtendedUplink up =
			ns::build_player_uplink(source_world, src_e, src_ae);
	if (!expect(up.carrier_handle == 0xFFFF, "on-foot vehicle handle")) return false;
	if (!expect(up.pos_x == src_ae.pos[0] && up.pos_y == src_ae.pos[1] && up.pos_z == src_ae.pos[2],
	            "pos = live AiEntity.pos (16.16)")) return false;
	if (!expect(up.heading == static_cast<int16_t>(src_ae.heading >> 16),
	            "heading = BAM32 high half")) return false;
	if (!expect(up.pitch == static_cast<int16_t>(src_ae.pitch >> 16),
	            "pitch = BAM32 high half")) return false;
	if (!expect(up.move_input_byte == 3, "movement-input byte carried from Entity.net_move_input"))
		return false;
	if (!expect(up.equipped_adm_index == 0x08,
	            "equipped adm index carried from Entity.equipped_adm_index (D-NET-143)"))
		return false;
	if (!expect(up.state_flags_byte == 0x38u,
	            "state flags = the RAW entity+0x24 low byte, unmasked on the write side"))
		return false;
	if (!expect(up.priority_handle_0 == 0 && up.priority_score_0 == 0,
	            "requested-interest feedback is not yet supplied by this builder")) return false;
	return true;
}

// ADS/scope is replicated by exactly ONE wire field: bit 0x10 of the uplink's state byte.
// There is no scope message and no scoped anim id on the wire — every observer re-derives
// the third-person scoped hold pose locally from this flag plus the ADM index. Prove the
// bit survives the whole joiner->host leg, because shipping a hardcoded 0 here (the
// pre-fix behaviour) left the host's copy of the joiner's Flags permanently unscoped and
// no other player ever saw the joiner aim down sights.
// [orig: write NetPacket_SerializePlayerState case 3 @0x4c1b17 `mov cl, [edi+24h]`;
//  host apply @0x4c1e4d `flags ^= (flags ^ wire) & 0x1C`; re-broadcast @0x4c0c7d;
//  the observer's scoped-variant selection Entity_UpdateInfantryPlayerBody @0x4b5deb]
bool run_scope_flag_reaches_host() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity peer;
	peer.kind = w::EntityKind::Organic;
	peer.item_id = 0x14B9;
	const w::EntityHandle ph = world.registry.spawn(0, peer);
	if (!expect(ph.valid(), "peer spawned")) return false;
	world.cached.local_player = w::EntityHandle::make(0, 7); // not the peer

	// The joiner scopes: apply_player_input_pre_tick folds bit 0x10 into its own entity.
	w::Entity src_e{};
	src_e.flags = 0x10u;
	w::AiEntity src_ae{};

	ns::PlayerIntent intent;
	intent.entity_handle = ph.packed;
	intent.item_type_id = 0x14B9;
	intent.state_flags =
			ns::build_player_uplink(world, src_e, src_ae).state_flags_byte;
	ns::apply_player_intent(world, intent);
	if (!expect((world.registry.get(ph)->flags & 0x10u) != 0,
	            "the host's copy of the joiner's entity is SCOPED after the uplink"))
		return false;
	// The re-broadcast the observers actually read is the raw low byte of that same word.
	if (!expect((ns::snapshot_of(*world.registry.get(ph)).state_flags & 0x10u) != 0,
	            "the S2C 0x0A player record re-broadcasts the scoped bit"))
		return false;

	// Un-scoping is the same channel: the replace-bits apply clears it again.
	src_e.flags = 0u;
	intent.state_flags =
			ns::build_player_uplink(world, src_e, src_ae).state_flags_byte;
	ns::apply_player_intent(world, intent);
	if (!expect((world.registry.get(ph)->flags & 0x10u) == 0,
	            "lowering the scope clears the host's bit (a REPLACE, not an OR)"))
		return false;
	return true;
}

// build -> encode -> decode -> host read-apply (drain_connection_c2s): the host peer SNAPs to the source pose.
bool run_roundtrip_to_host_snap() {
	// Source: the joiner's live local-player pose (zero low-16 heading/pitch for exact round-trip).
	w::World source_world;
	w::Entity src_e{};
	src_e.body_anim_slot = -1;
	w::AiEntity src_ae{};
	const int32_t sx = w::to_fixed(100.0), sy = w::to_fixed(200.0), sz = w::to_fixed(-50.0);
	src_ae.pos[0] = sx;
	src_ae.pos[1] = sy;
	src_ae.pos[2] = sz;
	src_ae.heading = 0x20000000; // i16 high 0x2000 -> mission yaw 45
	src_ae.pitch = 0x01000000;
	const nw::PlayerExtendedUplink up =
			ns::build_player_uplink(source_world, src_e, src_ae);

	// Host World with the joiner's peer entity at handle H.
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity peer;
	peer.kind = w::EntityKind::Organic;
	peer.item_id = 0x14B9; // player infantry template
	peer.position = {0.0f, 0.0f, 0.0f};
	const w::EntityHandle ph = world.registry.spawn(0, peer);
	if (!expect(ph.valid(), "host peer spawned")) return false;
	w::AiSystem &ai = world.ai;
	ai.attach(ph);
	world.cached.local_player = w::EntityHandle::make(0, 7); // a DIFFERENT handle is the host's own

	// Encode the full 0x0C payload (5-B sub-header at handle H + 43-B uplink body).
	nw::EntityPacketSubHeader sub;
	sub.handle = ph.packed;
	sub.item_type_id = 0x14B9;
	sub.sub_op = 0x0A;
	std::vector<uint8_t> payload = nw::encode_entity_packet_sub_header(sub);
	const std::vector<uint8_t> body = nw::encode_player_extended_uplink(up);
	payload.insert(payload.end(), body.begin(), body.end());

	// Drain through the host's authority C2S read-apply.
	ns::LoopbackChannel channel;
	channel.client_send(0x0C, payload);
	std::vector<ns::Connection> conns;
	conns.push_back(ns::Connection{&channel, ns::TransportMode::Loopback, ph, 0}); // the connection owns
	                                     // peer ph — the owner gate (D-NET-119) requires the uplink
	                                     // handle to match conn.owned_entity
	ns::test::drain_all(world, conns, /*is_authority=*/true);

	const w::Entity *pe = world.registry.get(ph);
	if (!expect(pe != nullptr, "host peer present")) return false;
	if (!expect(pe->position.x == static_cast<float>(w::from_fixed(sx)) &&
	            pe->position.y == static_cast<float>(w::from_fixed(sy)) &&
	            pe->position.z == static_cast<float>(w::from_fixed(sz)),
	            "host peer snapped to the joiner's source position")) return false;
	if (!expect(pe->yaw == 45, "host peer yaw = 45 (from the joiner's heading)")) return false;
	const w::AiEntity *ae = ai.for_handle(ph);
	if (!expect(ae != nullptr, "host peer has an AiEntity")) return false;
	if (!expect(ae->pos[0] == sx && ae->pos[1] == sy && ae->pos[2] == sz,
	            "host peer AiEntity live pos = source (16.16 exact)")) return false;
	if (!expect(ae->heading == src_ae.heading && ae->pitch == src_ae.pitch,
	            "host peer heading/pitch round-trip exactly (zero low-16 source)")) return false;
	if (!expect(ae->net_is_remote_peer, "host peer marked net-snapped")) return false;
	return true;
}

// A mounted local body is posed in world space after the vehicle mover, but the
// C2S op-3 record is carrier-local. Move and rotate the real attached vehicle,
// rebuild the local seat pose, then send the production builder through the
// codec and host read-apply against the host's matching live relationship.
bool run_mounted_moving_carrier_roundtrip() {
	w::World source;
	source.registry.configure_pool(0, 8);
	source.registry.configure_pool(1, 8);
	w::AiSystem &source_ai_system = source.ai;

	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.item_id = 0x14B9;
	player_seed.player_class = 8;
	player_seed.health = 150;
	player_seed.health_max = 150;
	const w::EntityHandle source_player_h = source.registry.spawn(0, player_seed);
	if (!expect(source_player_h.valid(), "source mounted player spawned")) return false;
	source_ai_system.attach(source_player_h);
	w::AiEntity *source_body = source_ai_system.for_handle(source_player_h);
	if (!expect(source_body != nullptr, "source mounted body attached")) return false;
	source_body->inf.active = true;
	source_body->inf.is_local_player = true;

	w::Entity carrier_seed;
	carrier_seed.kind = w::EntityKind::Item;
	carrier_seed.item_id = 0x1004;
	carrier_seed.position = {10.0f, 20.0f, 2.0f};
	carrier_seed.yaw = 90;
	carrier_seed.health = 3000;
	carrier_seed.health_max = 3000;
	w::Seat seat;
	seat.type = w::SeatType::Driver;
	seat.retail_slot = 8;
	seat.bone_index = 3;
	seat.seat_local = {2.5f, -1.25f, 1.0f};
	carrier_seed.seats.push_back(seat);
	const w::EntityHandle source_carrier_h = source.registry.spawn(1, carrier_seed);
	if (!expect(source_carrier_h.valid(), "source moving carrier spawned")) return false;
	if (!expect(source.vehicles.process_attach(source_player_h, source_carrier_h, 3),
	            "source player actually mounted")) return false;
	source_body->heading = 0x61230000;
	source_body->pitch = static_cast<int32_t>(0xF4000000u);
	if (!expect(source_ai_system.refresh_mounted_pose(*source_body, source),
	            "initial mounted pose refreshed")) return false;
	const int32_t initial_x = source_body->pos[0];
	const int32_t initial_y = source_body->pos[1];

	// Simulate the carrier's later mover and the joiner's final same-frame seat
	// refresh. Non-zero pitch/roll ensure the builder and apply use one full frame.
	w::Entity *source_carrier = source.registry.get(source_carrier_h);
	source_carrier->position = {85.0f, -20.0f, 7.0f};
	source_carrier->yaw = -35;
	source_carrier->pitch = 12;
	source_carrier->roll = -7;
	source_body->heading = 0x61230000; // independent mounted LOOK
	source_body->pitch = static_cast<int32_t>(0xF4000000u);
	if (!expect(source_ai_system.refresh_mounted_pose(*source_body, source),
	            "moved carrier pose refreshed")) return false;
	source.registry.get(source_player_h)->net_analog_x = -64;
	source.registry.get(source_player_h)->net_analog_y = 37;
	source.registry.get(source_player_h)->net_analog_z = -128;
	if (!expect(source_body->pos[0] != initial_x || source_body->pos[1] != initial_y,
	            "mounted local body followed the moving carrier")) return false;
	const nw::PlayerExtendedUplink up = ns::build_player_uplink(
			source, *source.registry.get(source_player_h), *source_body);
	if (!expect(up.carrier_handle == source_carrier_h.packed,
	            "mounted uplink carries the actual vehicle handle")) return false;
	if (!expect(up.pos_x != source_body->pos[0] || up.pos_y != source_body->pos[1],
	            "mounted uplink position is carrier-local, not world")) return false;
	const int32_t source_carrier_yaw = carrier_heading_bam(*source_carrier);
	const int32_t expected_local_heading = static_cast<int32_t>(
			static_cast<uint32_t>(source_body->heading) -
			static_cast<uint32_t>(source_carrier_yaw));
	if (!expect(up.heading == static_cast<int16_t>(expected_local_heading >> 16),
	            "mounted uplink heading is carrier-relative")) return false;

	// Matching authority world and relationship. The same 0x0C packet must lift
	// the local sample through the host carrier without disturbing the mount.
	w::World host;
	host.registry.configure_pool(0, 8);
	host.registry.configure_pool(1, 8);
	w::AiSystem &host_ai_system = host.ai;
	host.cached.local_player = w::EntityHandle::make(0, 7);
	const w::EntityHandle host_player_h = host.registry.spawn(0, player_seed);
	host_ai_system.attach(host_player_h);
	w::Entity host_carrier_seed = carrier_seed;
	host_carrier_seed.position = source_carrier->position;
	host_carrier_seed.yaw = source_carrier->yaw;
	host_carrier_seed.pitch = source_carrier->pitch;
	host_carrier_seed.roll = source_carrier->roll;
	const w::EntityHandle host_carrier_h = host.registry.spawn(1, host_carrier_seed);
	if (!expect(host_player_h.packed == source_player_h.packed &&
	                    host_carrier_h.packed == source_carrier_h.packed,
	            "source and host carrier handles match")) return false;
	if (!expect(host.vehicles.process_attach(host_player_h, host_carrier_h, 3),
	            "host peer actually mounted")) return false;
	if (!expect(drain_built_uplink(host, host_player_h, up),
	            "mounted uplink encoded, decoded, and applied")) return false;

	const nw::WorldPose lifted = nw::network_transform_local_to_world(
			up.pos_x, up.pos_y, up.pos_z,
			w::to_fixed(source_carrier->position.x),
			w::to_fixed(source_carrier->position.y),
			w::to_fixed(source_carrier->position.z),
			static_cast<uint32_t>(source_carrier_yaw),
			static_cast<uint32_t>(carrier_axis_bam(source_carrier->pitch)),
			static_cast<uint32_t>(carrier_axis_bam(source_carrier->roll)));
	const w::AiEntity *host_body = host_ai_system.for_handle(host_player_h);
	const w::Entity *host_player = host.registry.get(host_player_h);
	if (!expect(host_player != nullptr && host_player->net_analog_x == -64 &&
	                    host_player->net_analog_y == 37 && host_player->net_analog_z == -128,
	            "mounted throttle and steering reach authority through the real uplink"))
		return false;
	if (!expect(host_body != nullptr && host_body->pos[0] == lifted.x &&
	                    host_body->pos[1] == lifted.y && host_body->pos[2] == lifted.z,
	            "host lifts mounted local pose through the full carrier frame")) return false;
	if (!expect(near_fixed(host_body->pos[0], source_body->pos[0]) &&
	                    near_fixed(host_body->pos[1], source_body->pos[1]) &&
	                    near_fixed(host_body->pos[2], source_body->pos[2]),
	            "mounted builder/apply round-trip preserves the source world pose")) return false;
	const int32_t expected_host_heading = static_cast<int32_t>(
			(static_cast<uint32_t>(static_cast<uint16_t>(up.heading)) << 16) +
			static_cast<uint32_t>(source_carrier_yaw));
	return expect(host_player != nullptr && host_player->mounted &&
	                      host_player->mount_target == host_carrier_h &&
	                      host_body->heading == expected_host_heading,
	              "mounted round-trip preserves relationship and composes local heading");
}

// Standing on a moving deck/building uses the same carrier-local form without
// creating a seat relationship. Prove ground_target is selected, survives the
// real wire path, and is lifted back to the original world pose on authority.
bool run_ground_target_carrier_roundtrip() {
	w::World source;
	source.registry.configure_pool(0, 8);
	source.registry.configure_pool(2, 8);
	w::AiSystem &source_ai_system = source.ai;
	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.item_id = 0x14B9;
	player_seed.player_class = 8;
	const w::EntityHandle source_player_h = source.registry.spawn(0, player_seed);
	source_ai_system.attach(source_player_h);
	w::AiEntity *source_body = source_ai_system.for_handle(source_player_h);
	if (!expect(source_body != nullptr, "grounded source body attached")) return false;

	w::Entity deck_seed;
	deck_seed.kind = w::EntityKind::Building;
	deck_seed.item_id = 0x0465;
	deck_seed.position = {-40.0f, 65.0f, 4.0f};
	deck_seed.yaw = 25;
	deck_seed.pitch = -6;
	deck_seed.roll = 4;
	const w::EntityHandle source_deck_h = source.registry.spawn(2, deck_seed);
	if (!expect(source_deck_h.valid(), "ground carrier spawned")) return false;
	w::Entity *source_player = source.registry.get(source_player_h);
	source_player->ground_target = source_deck_h;
	const w::Entity *source_deck = source.registry.get(source_deck_h);
	const int32_t deck_yaw = carrier_heading_bam(*source_deck);
	const nw::WorldPose source_world_pose = nw::network_transform_local_to_world(
			w::to_fixed(3.0), w::to_fixed(-2.0), w::to_fixed(1.5),
			w::to_fixed(source_deck->position.x),
			w::to_fixed(source_deck->position.y),
			w::to_fixed(source_deck->position.z),
			static_cast<uint32_t>(deck_yaw),
			static_cast<uint32_t>(carrier_axis_bam(source_deck->pitch)),
			static_cast<uint32_t>(carrier_axis_bam(source_deck->roll)));
	source_body->pos[0] = source_world_pose.x;
	source_body->pos[1] = source_world_pose.y;
	source_body->pos[2] = source_world_pose.z;
	source_body->heading = static_cast<int32_t>(
			static_cast<uint32_t>(deck_yaw) + 0x12340000u);
	source_body->pitch = 0x04000000;
	const nw::PlayerExtendedUplink up =
			ns::build_player_uplink(source, *source_player, *source_body);
	if (!expect(up.carrier_handle == source_deck_h.packed,
	            "ground_target handle rides the uplink")) return false;
	if (!expect(!source_player->mounted,
	            "ground carrier does not invent a mounted relationship")) return false;

	w::World host;
	host.registry.configure_pool(0, 8);
	host.registry.configure_pool(2, 8);
	w::AiSystem &host_ai_system = host.ai;
	host.cached.local_player = w::EntityHandle::make(0, 7);
	const w::EntityHandle host_player_h = host.registry.spawn(0, player_seed);
	host_ai_system.attach(host_player_h);
	const w::EntityHandle host_deck_h = host.registry.spawn(2, deck_seed);
	if (!expect(host_player_h.packed == source_player_h.packed &&
	                    host_deck_h.packed == source_deck_h.packed,
	            "source and host ground handles match")) return false;
	if (!expect(drain_built_uplink(host, host_player_h, up),
	            "ground-target uplink encoded, decoded, and applied")) return false;

	const w::AiEntity *host_body = host_ai_system.for_handle(host_player_h);
	const w::Entity *host_player = host.registry.get(host_player_h);
	const nw::WorldPose lifted = nw::network_transform_local_to_world(
			up.pos_x, up.pos_y, up.pos_z,
			w::to_fixed(deck_seed.position.x), w::to_fixed(deck_seed.position.y),
			w::to_fixed(deck_seed.position.z), static_cast<uint32_t>(deck_yaw),
			static_cast<uint32_t>(carrier_axis_bam(deck_seed.pitch)),
			static_cast<uint32_t>(carrier_axis_bam(deck_seed.roll)));
	if (!expect(host_body != nullptr && host_body->pos[0] == lifted.x &&
	                    host_body->pos[1] == lifted.y && host_body->pos[2] == lifted.z,
	            "authority lifts ground-local pose through the carrier")) return false;
	if (!expect(near_fixed(host_body->pos[0], source_body->pos[0]) &&
	                    near_fixed(host_body->pos[1], source_body->pos[1]) &&
	                    near_fixed(host_body->pos[2], source_body->pos[2]),
	            "ground-target round-trip preserves source world pose")) return false;
	return expect(host_player != nullptr && !host_player->mounted &&
	                      host_player->ground_target == host_deck_h,
	              "authority retains ground_target without inventing a mount");
}

// The equipped-adm ingest gate (D-NET-143): a table-less world accepts the uplinked byte
// verbatim; with the armory fed, only an existing entry with category < 11 is stored
// [orig: case-4 store @0x4C20A3 gated AdmDefs[idx].category < 11; a missing entry
// (including the 0xFF none sentinel) mirrors the failed AdmDef_GetEntryByIndex leg].
bool run_equipped_adm_ingest_gate() {
	w::World world;
	world.registry.configure_pool(0, 8);
	w::Entity peer;
	peer.kind = w::EntityKind::Organic;
	peer.item_id = 0x14B9;
	const w::EntityHandle ph = world.registry.spawn(0, peer);
	if (!expect(ph.valid(), "peer spawned")) return false;
	world.cached.local_player = w::EntityHandle::make(0, 7); // not the peer

	ns::PlayerIntent intent;
	intent.entity_handle = ph.packed;
	intent.item_type_id = 0x14B9;

	// Table-less world: accepted verbatim (unit path — a live host always feeds weapon.def).
	intent.equipped_adm_index = 0x30;
	ns::apply_player_intent(world, intent);
	if (!expect(world.registry.get(ph)->equipped_adm_index == 0x30,
	            "table-less world stores the uplinked byte verbatim")) return false;

	// Armory fed: category < 11 passes, the emplaced band (>= 11) and missing entries do not.
	world.tables.weapons.entries.resize(12);
	world.tables.weapons.entries[8].valid = true;
	world.tables.weapons.entries[8].name = "WPN_T";
	world.tables.weapons.entries[8].category = 3;
	world.tables.weapons.entries[11].valid = true;
	world.tables.weapons.entries[11].name = "WPN_EMPL";
	world.tables.weapons.entries[11].category = 11;

	intent.equipped_adm_index = 8;
	ns::apply_player_intent(world, intent);
	if (!expect(world.registry.get(ph)->equipped_adm_index == 8,
	            "category 3 entry passes the < 11 gate")) return false;
	intent.equipped_adm_index = 11;
	ns::apply_player_intent(world, intent);
	if (!expect(world.registry.get(ph)->equipped_adm_index == 8,
	            "category 11 (emplaced band) is NOT stored")) return false;
	intent.equipped_adm_index = 0xFF;
	ns::apply_player_intent(world, intent);
	if (!expect(world.registry.get(ph)->equipped_adm_index == 8,
	            "the 0xFF none sentinel / missing entry is NOT stored")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = run_field_mapping() && ok;
	ok = run_roundtrip_to_host_snap() && ok;
	ok = run_mounted_moving_carrier_roundtrip() && ok;
	ok = run_ground_target_carrier_roundtrip() && ok;
	ok = run_scope_flag_reaches_host() && ok;
	ok = run_equipped_adm_ingest_gate() && ok;
	return ok ? 0 : 1;
}
