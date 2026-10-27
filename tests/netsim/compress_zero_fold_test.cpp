// The compressed word of a zero magnitude (D-NET-1327): Network_CompressFixedPoint's
// `bsr` on a value of 0 or -1 leaves ECX as the caller had it, and each serializer's call site
// names what ECX holds there. Every site's 0 and -1 are pinned below:
//   - the player's and the vehicle's anchor-relative x: a payload address (the player's carrier
//     word, the vehicle's x word) in g_NapiMsgPayloadBuf at its fixed image address;
//   - the player's and the vehicle's carrier-local x: the value itself;
//   - the infantry's anchor-relative x: its live Yaw; its carrier-local x: the ECX
//     Entity_TransformWorldToLocal returns with (z's roll term);
//   - the vehicle's vertical velocity: the rounded heading word's high byte under the flags byte;
//     its forward register: the health word;
//   - the round event's x: its shot word;
//   - every y, z and the lateral register: the word written just before.
// A stock decoder reads every such word as 0 or -1 whatever its shift bits: the row is byte
// parity, not position.
// [orig: Network_CompressFixedPoint @0x4C2780 - `bsr ecx, eax` @0x4C279D, the borrow mask
//  @0x4C27A0..0x4C27AC; Server_SendEntityStateToPlayer @0x517BA0 - g_NapiMsgPayloadBuf
//  @0xB5BBB0 as the writer's base @0x517C2C; NetPacket_SerializePlayerState @0x4C09C0 -
//  @0x4C0A89, @0x4C0B0C, the calls @0x4C0B11..0x4C0C3A; Entity_SerializeVehicleState
//  @0x460560 - @0x460BBB, @0x460C55, @0x460D13, @0x460D84, the calls @0x460BC0..0x460DCF;
//  NetPacket_SerializeInfantryEntityState @0x4C0320 - @0x4C07C0, the calls @0x4C07ED..0x4C08BE;
//  Entity_TransformWorldToLocal @0x43BB50 - @0x43BCDF; NetPacket_SerializeRoundEvent
//  @0x504820 - @0x50496F, the calls @0x50498F..0x5049E3]

#include <runtime/replication/connection.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/inmatch/loopback_channel.h>

#include "conn_fan_test_util.h"

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/round_ring.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;
namespace w = opennova::world;

using test_expect::expect;

// The word a zero magnitude takes under `ecx`, as the function computes it.
uint16_t zero_word(uint32_t ecx, bool negative) {
	return uint16_t((((ecx >= 15u) ? ecx - 15u : 0u) | 1u) & 0x0Eu) | (negative ? 1u : 0u);
}

// The register rule. Oracles: L5-s41's player gunner on gun 0x102E, gun-local y 0 after the x
// word 0x90A1, which retail hosts (x8 RR, RR-aa, x10 RO) write 0x0002; the drive captures' live
// vehicles, whose zero forward register retail writes 0x0008 at health 3000 and 0x0000 at
// health 8000.
bool test_register_rule() {
	bool ok = true;
	ok &= expect(nw::network_compress_fixedpoint(0) == 0x0000 &&
					nw::network_compress_fixedpoint(-1) == 0x0001,
			"under ECX 0: the sign words");
	ok &= expect(nw::network_compress_fixedpoint(0, 0x90A1) == 0x0002 &&
					nw::network_compress_fixedpoint(-1, 0x90A1) == 0x0003,
			"after the x word 0x90A1: 0x0002 (retail's gun-local y) and 0x0003");
	ok &= expect(nw::network_compress_fixedpoint(0, 3000) == 0x0008 &&
					nw::network_compress_fixedpoint(-1, 3000) == 0x0009,
			"under health 3000: 0x0008 (retail's forward register) and 0x0009");
	ok &= expect(nw::network_compress_fixedpoint(0, 8000) == 0x0000, "under health 8000: 0x0000");
	ok &= expect(nw::network_compress_fixedpoint(0, 14) == 0x0000 &&
					nw::network_compress_fixedpoint(0, 15) == 0x0000 &&
					nw::network_compress_fixedpoint(0, 17) == 0x0002 &&
					nw::network_compress_fixedpoint(0, 0xFFFFFFFFu) == 0x0000,
			"the borrow below 15 and the bits 1..3 of ECX - 15 above it");
	// The carrier-local x of the player and the vehicle: ECX holds the value itself.
	ok &= expect(nw::network_compress_fixedpoint(0, 0u) == 0x0000 &&
					nw::network_compress_fixedpoint(-1, 0xFFFFFFFFu) == 0x0001,
			"ECX = the value: 0 and -1 keep the sign words");
	// A nonzero magnitude's bsr defines ECX: the caller's register never reaches its word.
	ok &= expect(nw::network_compress_fixedpoint(0x12345, 0) ==
							nw::network_compress_fixedpoint(0x12345, 0x9E3779B9u) &&
					nw::network_compress_fixedpoint(-7, 0) ==
							nw::network_compress_fixedpoint(-7, 0x9E3779B9u),
			"a nonzero value ignores ECX");
	if (ok)
		std::printf("PASS register_rule\n");
	return ok;
}

// The infantry's carrier-local x: ECX is z's roll term, m(ty, -sin roll). Roll 45 degrees and a
// 2-LSB local y step give -2 whatever the sine's last bit (2 x 0.707 floors to -2), so the
// local x 0 writes 0x000E and -1 writes 0x000F.
bool test_transform_exit_ecx() {
	bool ok = true;
	for (const int32_t x : { 0, -1 }) {
		int32_t ecx = 0;
		const nw::WorldPose local = nw::network_transform_world_to_local(
				100 + x, 200 + 2, 300, 100, 200, 300, 0u, 0u, 0x20000000u, &ecx);
		ok &= expect(local.x == x, "the local x is the world delta (no yaw, no pitch)");
		ok &= expect(ecx == -2, "the exit ECX is z's roll term, -2");
		ok &= expect(nw::network_compress_fixedpoint(local.x, uint32_t(ecx)) ==
						(x == 0 ? 0x000E : 0x000F),
				"the carrier-local infantry x takes the exit ECX's bits");
	}
	if (ok)
		std::printf("PASS transform_exit_ecx\n");
	return ok;
}

// x8 RR (a retail host), S2C 0x0A frame 252, through its second record, with the terminator
// put after it: the joiner's own player record and the host body's, both with no carrier, carry
// anchor-relative zeros. Their x words are 0x0002 (the carrier word at payload offset 34) and
// 0x000A (at 57): (0xB5BBB0 + offset - 15) | 1, bits 1..3. Each y follows its x word (0), the
// own record's z its y word (0).
const std::vector<uint8_t> kRetailFrame252Prefix = {
	0xb8,
	0x7a,
	0x08,
	0xfb,
	0x90,
	0x20,
	0xe5,
	0x02,
	0x00,
	0x00,
	0x2a,
	0x00,
	0x00,
	0x01,
	0x14,
	0x0d,
	0x36,
	0x00,
	0xff,
	0xff,
	0x00,
	0xff,
	0xff,
	0x96,
	0x00,
	0x00,
	0x00,
	0x01,
	0x5d,
	0x00,
	0xb9,
	0x14,
	0x00,
	0x00,
	0xff,
	0xff,
	0x02,
	0x00,
	0x00,
	0x00,
	0x00,
	0x00,
	0x0e,
	0x00,
	0x00,
	0x00,
	0x2b,
	0x53,
	0x01,
	0x28,
	0x01,
	0x5c,
	0x00,
	0xb9,
	0x14,
	0x00,
	0x00,
	0xff,
	0xff,
	0x0a,
	0x00,
	0x00,
	0x00,
	0x30,
	0x6f,
	0x0e,
	0x00,
	0x00,
	0x00,
	0x2c,
	0x45,
	0x09,
	0x28,
	0x00,
};

nw::EntityClass player_class(uint16_t) {
	return nw::EntityClass::Player;
}

uint16_t word_at(const std::vector<uint8_t> &bytes, size_t offset) {
	return uint16_t(bytes[offset] | (bytes[offset + 1] << 8));
}

bool test_retail_frame_vector() {
	nw::FrameUpdate fu;
	if (!expect(nw::decode_frame_update(kRetailFrame252Prefix.data(), kRetailFrame252Prefix.size(),
						player_class, fu) &&
						fu.records.size() == 2,
				"the retail prefix decodes to two player records"))
		return false;
	bool ok = true;
	ok &= expect(fu.records[0].player.pos_x_compressed == 0x0002 &&
					fu.records[1].player.pos_x_compressed == 0x000A,
			"retail's own and host records carry x words 0x0002 and 0x000A");
	// A decoded record carries no fold bits: its words go out as read.
	ok &= expect(nw::encode_frame_update(fu, false) == kRetailFrame252Prefix,
			"the decoded frame encodes back byte for byte");
	// Ours: the sign words and the fold bits the fan sets; the encoder supplies the addresses.
	nw::FrameUpdate ours = fu;
	ours.records[0].player.pos_x_compressed = nw::network_compress_fixedpoint(0);
	ours.records[0].player.pos_y_compressed = nw::network_compress_fixedpoint(0);
	ours.records[0].player.pos_z_compressed = nw::network_compress_fixedpoint(0);
	ours.records[0].player.pos_zero_fold = nw::kPosFoldX | nw::kPosFoldY | nw::kPosFoldZ;
	ours.records[1].player.pos_x_compressed = nw::network_compress_fixedpoint(0);
	ours.records[1].player.pos_y_compressed = nw::network_compress_fixedpoint(0);
	ours.records[1].player.pos_zero_fold = nw::kPosFoldX | nw::kPosFoldY;
	ok &= expect(nw::encode_frame_update(ours, false) == kRetailFrame252Prefix,
			"the folded words take retail's payload-address bits");
	// The same records at -1: the address's bits with the sign bit.
	nw::FrameUpdate negative = ours;
	for (nw::FrameUpdateRecord &r : negative.records) {
		r.player.pos_x_compressed = nw::network_compress_fixedpoint(-1);
		r.player.pos_y_compressed = nw::network_compress_fixedpoint(-1);
	}
	const std::vector<uint8_t> neg = nw::encode_frame_update(negative, false);
	ok &= expect(word_at(neg, 36) == 0x0003 && word_at(neg, 38) == 0x0001 &&
					word_at(neg, 59) == 0x000B && word_at(neg, 61) == 0x0001,
			"-1 takes the same address bits and the sign bit; y follows its x word");
	// A carrier-local record keeps its words: its x call's ECX is the value itself.
	nw::FrameUpdate carried = ours;
	carried.records[0].player.carrier_handle = 0x1015;
	const std::vector<uint8_t> bytes = nw::encode_frame_update(carried, false);
	ok &= expect(word_at(bytes, 36) == 0x0000, "a carried record's folded x stays the sign word");
	if (ok)
		std::printf("PASS retail_frame_vector\n");
	return ok;
}

// The vehicle's anchor-relative x: its call's ECX holds the x word's own payload address.
bool test_vehicle_world_x() {
	bool ok = true;
	for (const int32_t v : { 0, -1 }) {
		nw::FrameUpdate fu;
		fu.flags2 = 2; // the env sub-block: 11 B
		nw::FrameUpdateRecord rec;
		rec.cls = nw::EntityClass::Vehicle;
		rec.handle = 0x1015;
		rec.type_id = 0x0516;
		rec.vehicle.parent_slot_handle = 0xFFFF;
		rec.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(v);
		rec.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(0x30000);
		rec.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(v);
		rec.vehicle.pos_zero_fold = nw::kPosFoldX | nw::kPosFoldZ;
		fu.records.push_back(rec);
		const std::vector<uint8_t> bytes = nw::encode_frame_update(fu, false);
		// Header 12 + 2 + 11 + 7 = 32; the record's tag, handle and type to 37; the parent
		// word 37..38, x at 39.
		const uint32_t x_offset = 39;
		const uint16_t y = word_at(bytes, x_offset + 2);
		ok &= expect(word_at(bytes, x_offset) ==
						zero_word(nw::kNapiMsgPayloadBufAddress + x_offset, v < 0),
				"a folded vehicle x takes its own word's address");
		ok &= expect(y == nw::network_compress_fixedpoint(0x30000), "an unfolded y keeps its word");
		ok &= expect(word_at(bytes, x_offset + 4) == zero_word(y, v < 0),
				"a folded z follows the y word");
	}
	if (ok)
		std::printf("PASS vehicle_world_x\n");
	return ok;
}

w::PlayerSpawn player_spawn(w::Vec3 pos, uint16_t net_id) {
	w::PlayerSpawn s;
	s.position = pos;
	s.heading = w::placed_heading_bam(0);
	s.net_id = net_id;
	return s;
}

nw::EntityClass fan_class(uint16_t type_id) {
	if (type_id == 0x14B9)
		return nw::EntityClass::Player;
	if (type_id == 0x050B)
		return nw::EntityClass::Vehicle;
	return nw::EntityClass::Infantry;
}

// Through the host's fan, every anchor-relative value `v` (0 or -1): the recipient's own record
// (its eye offset -v on x and y, so its x and y are v), two live vehicles at health 3000 with
// Flags 0x08 and a heading whose rounded word's high byte is 0x40 (the first's registers v, the
// second's forward 3 u so its lateral v follows a large word), an infantry body standing at
// the anchor's x plus v under a Yaw whose ECX - 15 has bits 1..3 of 4, and a round fired from
// the anchor plus v on x and y with shot word 77.
bool fan_case(int32_t v) {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 8);
	world->registry.configure_pool(1, 8);
	const w::EntityHandle me =
			w::spawn_remote_player(*world, player_spawn({ 1.0f, 2.0f, 3.0f }, 0xFFF0));
	const w::EntityHandle shooter =
			w::spawn_remote_player(*world, player_spawn({ 4.0f, 5.0f, 6.0f }, 0xFFF1));
	w::AiEntity *body = world->ai.for_handle(me);
	if (!expect(me.valid() && shooter.valid() && body != nullptr, "players spawned"))
		return false;
	body->inf.eye_offset_x = -v;
	body->inf.eye_offset_y = -v;
	body->inf.eye_offset_z = 0xD000;
	world->ai.mirror_wire_anim(*body, *world);
	const int32_t ax = 0x10000 - v, ay = 0x20000 - v, az = 0x30000 + 0xD000;

	w::EntityHandle vh[2];
	for (int i = 0; i < 2; ++i) {
		w::Entity veh;
		veh.kind = w::EntityKind::Item;
		veh.item_id = 0x050B;
		veh.net_class_code = uint8_t(nw::EntityClass::Vehicle);
		veh.health = 3000;
		veh.health_max = 3000;
		veh.flags = 0x08;
		veh.heading = 0x40000000;
		veh.position = { 6.0f + float(i), 2.0f, 3.0f };
		veh.veh.cmd_speed = i == 0 ? v : 0x30000;
		veh.veh.cmd_lateral_speed = v;
		veh.veh.slide_z = v;
		veh.has_item_def = true;
		veh.item_type_index = 1;
		vh[i] = world->registry.spawn(1, veh);
	}

	constexpr int32_t kYaw = 0x40000013; // ECX - 15 = 0x40000004
	w::Entity inf;
	inf.kind = w::EntityKind::Organic;
	inf.item_id = 0x080F;
	inf.net_class_code = uint8_t(nw::EntityClass::Infantry);
	inf.health = 100;
	inf.health_max = 100;
	inf.position = { 1.0f, 7.0f, 3.0f }; // x 0x10000 = the anchor x plus v
	inf.heading = kYaw;
	inf.has_item_def = true;
	inf.item_type_index = 1;
	const w::EntityHandle ih = world->registry.spawn(0, inf);
	if (!expect(vh[0].valid() && vh[1].valid() && ih.valid(), "vehicles and infantry spawned"))
		return false;

	std::vector<ns::Connection> conns;
	ns::LoopbackChannel ch;
	conns.push_back(ns::Connection{ &ch, ns::TransportMode::Loopback, me, 0 });
	// The join's arm, then a round fired after it [orig: the frame clock @0x5001A8].
	if (world->out.rounds.frame_clock < 2)
		world->out.rounds.frame_clock = 2;
	for (ns::Connection &c : conns)
		c.round_watermark = world->out.rounds.frame_clock - 1;
	w::RoundEvent ev;
	ev.shooter_handle = shooter.packed;
	ev.origin_x = ax + v;
	ev.origin_y = ay + v;
	ev.origin_z = az + 0x8000;
	ev.shot_seq = 77;
	ev.adm_index = 11;
	world->out.rounds.add(ev);
	ns::test::emit_all(*world, conns);

	ns::Datagram dg;
	nw::FrameUpdate fu;
	if (!expect(ch.client_recv(dg) &&
						nw::decode_frame_update(dg.body.data(), dg.body.size(), fan_class, fu),
				"the 0x0A frame decodes"))
		return false;
	const nw::FrameUpdateRecord *own = nullptr, *v0 = nullptr, *v1 = nullptr, *irec = nullptr;
	for (const nw::FrameUpdateRecord &r : fu.records) {
		if (r.handle == me.packed)
			own = &r;
		if (r.handle == vh[0].packed)
			v0 = &r;
		if (r.handle == vh[1].packed)
			v1 = &r;
		if (r.handle == ih.packed)
			irec = &r;
	}
	if (!expect(own != nullptr && v0 != nullptr && v1 != nullptr && irec != nullptr,
				"the own, vehicle and infantry records are present"))
		return false;
	if (!expect(fu.anchor_x == ax && fu.anchor_y == ay, "the anchor is the recipient's eye"))
		return false;
	const bool neg = v < 0;
	bool ok = true;
	// The own record's carrier word: find its header (tag 1, handle, type) in the payload.
	size_t body_offset = 0;
	for (size_t i = 0; i + 5 <= dg.body.size(); ++i) {
		if (dg.body[i] == 1 && dg.body[i + 1] == (me.packed & 0xFF) &&
				dg.body[i + 2] == (me.packed >> 8) && dg.body[i + 3] == 0xB9 &&
				dg.body[i + 4] == 0x14) {
			body_offset = i + 5;
			break;
		}
	}
	ok &= expect(body_offset != 0, "the own record's header is in the payload");
	const uint16_t own_x = own->player.pos_x_compressed;
	ok &= expect(own_x == zero_word(nw::kNapiMsgPayloadBufAddress + uint32_t(body_offset) + 2, neg),
			"the own record's x takes its carrier word's address");
	ok &= expect(own->player.pos_y_compressed == zero_word(own_x, neg),
			"the own record's y follows its x word");
	ok &= expect(own->player.pos_z_compressed == 0x6801, "the own z is the 0xD000 eye's code");
	ok &= expect(
			v0->vehicle.health_word == 3000 && v0->vehicle.weapon_aim_y == (neg ? 0x0009 : 0x0008),
			"a folded forward register takes the health word's bits");
	ok &= expect(v0->vehicle.weapon_aim_z == zero_word(v0->vehicle.weapon_aim_y, neg) &&
					v1->vehicle.weapon_aim_z == zero_word(v1->vehicle.weapon_aim_y, neg) &&
					v1->vehicle.weapon_aim_y == nw::network_compress_fixedpoint(0x30000),
			"a folded lateral register follows the forward word");
	ok &= expect(uint16_t(v0->vehicle.euler_z) == 0x4000 && v0->vehicle.flags_byte == 0x08 &&
					v0->vehicle.vertical_velocity == (neg ? 0x0009 : 0x0008),
			"a folded vertical velocity takes the heading high byte under the flags byte");
	ok &= expect(irec->infantry.pos_x_compressed == (neg ? 0x0005 : 0x0004),
			"the infantry's folded x takes its live Yaw's bits");
	ok &= expect(fu.round_events.size() == 1, "the round reaches the recipient");
	if (fu.round_events.size() == 1) {
		const nw::RoundEventRecord &re = fu.round_events[0];
		ok &= expect(re.shot_seq == 77 && re.pos_x_compressed == zero_word(77, neg),
				"the round's folded x takes the shot word's bits");
		ok &= expect(re.pos_y_compressed == zero_word(re.pos_x_compressed, neg),
				"the round's folded y follows its x word");
	}
	if (ok)
		std::printf("PASS connection_fan %d\n", v);
	return ok;
}

} // namespace

int main() {
	bool ok = true;
	ok &= test_register_rule();
	ok &= test_transform_exit_ecx();
	ok &= test_retail_frame_vector();
	ok &= test_vehicle_world_x();
	ok &= fan_case(0);
	ok &= fan_case(-1);
	if (!ok)
		return 1;
	std::printf("ALL PASS\n");
	return 0;
}
