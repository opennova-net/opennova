// Replay-timeline assembler regression — inline crafted pcap, CI-runnable.
//
// Crafts a tiny in-memory capture (apps/common build_pcap_udp) that exercises
// the WHOLE replay path with no file fixtures: a ServerAuth + ClientAuth pair
// delivers the per-direction SCRK, an S2C 0x0D spawn batch lays down two
// entities, and two C2S 0x0C extended uplinks move one of them. The capture is
// read back through the shared reader and the shared outer-decode pipeline
// (libs/novaworld decode_capture_to_messages) into build_replay_timeline, then
// the assembled entities + tracks are asserted against the crafted input.
//
// Covers, end to end on a few hundred bytes: build_pcap_udp + read_pcap_udp +
// envelope + outer NWU + SCRK recovery (both directions) + protocol reassembly +
// the §5.11 spawn decoder + the §5.10 uplink decoder + the timeline assembler —
// so the replay foundation stays covered in CI without the .scratch capture (the
// real-capture cross-validation lives in nw_pool_groundtruth, path-gated).

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <novaworld/ingame_decode.h>
#include <novaworld/ingame_encode.h>
#include <novaworld/protocol_message.h>
#include <novaworld/replay_timeline.h>
#include <novaworld/session_hello.h>
#include <novaworld/session_keys.h>
#include <novaworld/wire_capture.h>

#include "pcap_reader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace opennova;

namespace {

int g_failures = 0;
#define EXPECT(cond)                                                            \
	do {                                                                        \
		if (!(cond)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
			g_failures++;                                                       \
		}                                                                       \
	} while (0)

constexpr std::string_view kScrk = "UNIT_TEST_SCRK_0";

// Little-endian appenders for hand-crafting the C2S 0x0C uplink (no encoder for
// it yet; the byte order mirrors decode_entity_packet_sub_header +
// decode_player_extended_uplink, §5.10).
void put_u16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(uint8_t(v));
	b.push_back(uint8_t(v >> 8));
}
void put_u32(std::vector<uint8_t> &b, uint32_t v) {
	for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

// The 48-byte C2S 0x0C inner payload: 5-byte sub-header (handle/type/sub_op=0x0A
// extended) + 43-byte extended uplink body (unmounted -> world position).
std::vector<uint8_t> make_uplink(uint16_t handle, uint16_t type, int32_t x,
                                 int32_t y, int32_t z, int16_t heading) {
	std::vector<uint8_t> b;
	put_u16(b, handle);
	put_u16(b, type);
	b.push_back(0x0A); // sub_op = extended (type-10)
	put_u16(b, 0xFFFF); // vehicle_handle = none (unmounted)
	put_u32(b, uint32_t(x));
	put_u32(b, uint32_t(y));
	put_u32(b, uint32_t(z));
	put_u16(b, uint16_t(heading));
	put_u16(b, 0); // pitch
	for (int i = 0; i < 9; ++i) b.push_back(0); // reserved/anim/stat bytes
	for (int i = 0; i < 8; ++i) put_u16(b, 0);  // 4x (weapon_id, fire_counter)
	return b;
}

// Wrap a post-opcode plaintext body into the on-wire UDP payload: outer NWU
// transform (sender applies nwu_decrypt so the receiver's nwu_encrypt recovers
// it — reference_nwu_names_swapped), opcode prefix, NAPI envelope.
std::vector<uint8_t> nwu_outer_encode(uint8_t opcode, std::vector<uint8_t> body) {
	if (!body.empty()) nwu_decrypt(body.data(), body.size(), SESSION_NWU_KEY);
	std::vector<uint8_t> stripped;
	stripped.reserve(body.size() + 1);
	stripped.push_back(opcode);
	stripped.insert(stripped.end(), body.begin(), body.end());
	std::vector<uint8_t> raw(stripped.size() + 16);
	size_t out = 0;
	if (napi_envelope_encode(stripped.data(), stripped.size(), raw.data(),
	                         raw.size(), &out) != 0)
		return {};
	raw.resize(out);
	return raw;
}

// Build a minimal S2C 0x0A inner body: 12-byte anchor header, flags1=0/flags2=0
// (sub-block 0 -> 11 B), the 7-byte tail, one tag==1 infantry compact record,
// then the EOB terminator. Mirrors the §5.9 layout decode_frame_update walks.
std::vector<uint8_t> make_frame_update_0a(int32_t ax, int32_t ay, int32_t az,
                                          uint16_t handle, uint16_t type,
                                          const InfantryCompactRecord &inf) {
	std::vector<uint8_t> b;
	put_u32(b, uint32_t(ax));
	put_u32(b, uint32_t(ay));
	put_u32(b, uint32_t(az));
	b.push_back(0); // flags1
	b.push_back(0); // flags2 -> sub-block 0
	for (int i = 0; i < 11; ++i) b.push_back(0); // sub-block 0 (11 B)
	b.push_back(0);        // state_flag
	put_u16(b, 0xFFFF);    // mount handle
	put_u16(b, 150);       // health (i16)
	put_u16(b, 0);         // state_word (i16)
	b.push_back(1);        // event tag = 1 (compact record)
	put_u16(b, handle);
	put_u16(b, type);
	const std::vector<uint8_t> compact = encode_infantry_compact_record(inf);
	b.insert(b.end(), compact.begin(), compact.end());
	b.push_back(0);        // event tag = 0 (EOB)
	return b;
}

// Wrap an inner protocol-message body (a tag-0xNN payload) as a full datagram:
// SCRK-encrypted 0x43/0x83 protocol packet -> nwu_outer_encode.
std::vector<uint8_t> make_proto_payload(uint8_t opcode, uint8_t tag,
                                        const std::vector<uint8_t> &inner) {
	ProtocolMessage msg = make_protocol_message(tag, inner);
	ProtocolPacketHeader hdr{};
	hdr.session_id = 0x1234;
	std::vector<uint8_t> proto;
	encode_protocol_packet_plaintext(hdr, {msg}, kScrk, proto);
	return nwu_outer_encode(opcode, proto);
}

// Direct vectors for the compressed-fixed-point decoder, hand-computed against
// [orig: Network_DecompressFixedPoint @ 0x4C27E0]: sign(bit0) ^ (mantissa(bits
// 4-15) << ((bits 1-3)|1)).
void test_decompress_vectors() {
	EXPECT(network_decompress_fixedpoint(0x0000) == 0);
	EXPECT(network_decompress_fixedpoint(0x0002) == 0);        // mantissa 0
	EXPECT(network_decompress_fixedpoint(0x5007) == -2621441); // sign, exp 7
	EXPECT(network_decompress_fixedpoint(0x4609) == -9175041); // sign, exp 9
	EXPECT(network_decompress_fixedpoint(0x4608) == 9175040);  // +,    exp 9
}

// End-to-end: a crafted S2C 0x0A message through the full pipeline + the timeline
// fold. Asserts an unmounted compact record decodes to world = decompress + the
// header anchor (the §5.9 model proven on the real capture by first-0x0A==spawn).
void test_frame_update_fold() {
	constexpr uint16_t kType = 0x0816;   // resolves to Infantry below
	constexpr uint16_t kHandle = 0x0002; // pool-0 slot 2
	const int32_t ax = 1638400, ay = 4587520, az = 1048576; // 25 / 70 / 16 (16.16)

	InfantryCompactRecord inf;
	inf.vehicle_slot_handle = 0xFFFF; // unmounted -> decompress + anchor
	inf.pos_x_compressed = 0x5007;
	inf.pos_y_compressed = 0x4608;
	inf.pos_z_compressed = 0x0000;
	inf.yaw_byte = 0x40; // BAM high -> 90 deg

	const std::vector<uint8_t> body =
	    make_frame_update_0a(ax, ay, az, kHandle, kType, inf);

	ClientAuth ca;
	ca.na = "t";
	ca.scrk = std::string(kScrk);
	ServerAuth sa = build_server_auth(ca, 0x7F000001u, 32768, 0x55, kScrk);
	std::vector<net::PcapDatagram> dgrams;
	int f = 1;
	dgrams.push_back({32769, 32768, f++,
	                  nwu_outer_encode(SESSION_OPCODE_SERVER_AUTH,
	                                   server_auth_to_bytes(sa))});
	dgrams.push_back({32769, 32768, f++,
	                  make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	                                     0x0A, body)});
	const std::vector<uint8_t> pcap = net::build_pcap_udp(dgrams);
	std::vector<net::PcapDatagram> got;
	EXPECT(net::read_pcap_udp(pcap.data(), pcap.size(), got));
	std::vector<CaptureDatagram> caps;
	for (auto &pk : got) caps.push_back({pk.frame_index, std::move(pk.payload)});

	auto class_of = [kType](uint16_t t) -> EntityClass {
		return t == kType ? EntityClass::Infantry : EntityClass::Unknown;
	};
	const ReplayTimeline tl =
	    build_replay_timeline(decode_capture_to_messages(caps), class_of);

	const ReplayEntity *e = nullptr;
	for (const auto &ent : tl.entities)
		if (ent.handle == kHandle) e = &ent;
	EXPECT(e != nullptr);
	if (!e) return;
	const ReplaySample *s = nullptr;
	for (const auto &x : e->track)
		if (x.source == ReplaySampleSource::FrameUpdate) s = &x;
	EXPECT(s != nullptr);
	if (!s) return;
	EXPECT(s->x == ax + network_decompress_fixedpoint(0x5007)); // -983041
	EXPECT(s->y == ay + network_decompress_fixedpoint(0x4608)); // 13762560
	EXPECT(s->z == az);                                         // +0
	EXPECT(s->has_heading && std::fabs(s->heading_deg - 90.0) < 0.5);
}

// --- event-stream crafters ---------------------------------------------------
void put_i16(std::vector<uint8_t> &b, int16_t v) { put_u16(b, uint16_t(v)); }

// S2C 0x1E game event — 8 B (§ NetPacket_HandleGameEvent @ 0x426270).
std::vector<uint8_t> make_game_event_1e(uint8_t type, uint8_t att, uint8_t vic,
                                        uint8_t aux, int16_t x, int16_t y) {
	std::vector<uint8_t> b{type, att, vic, aux};
	put_i16(b, x);
	put_i16(b, y);
	return b;
}

// C2S 0x06 client-fired-round — 45 B (§5.16).
std::vector<uint8_t> make_fired_round_06(uint16_t shooter, uint8_t adm, int32_t x,
                                         int32_t y, int32_t z, int32_t dx, int32_t dy) {
	std::vector<uint8_t> b;
	put_u32(b, 0x12345678);   // current_tick
	put_u16(b, shooter);
	b.push_back(0);           // fire_flags
	b.push_back(adm);
	put_u32(b, uint32_t(x)); put_u32(b, uint32_t(y)); put_u32(b, uint32_t(z));
	put_u32(b, uint32_t(dx)); put_u32(b, uint32_t(dy));
	put_u16(b, 0xFFFF);       // target_handle
	put_u16(b, 0);            // hit_part
	b.push_back(0); b.push_back(0); b.push_back(0); // extras
	for (int i = 0; i < 5; ++i) put_u16(b, 0);      // muzzle block
	return b;                 // 45 B
}

// S2C 0x0A with an ENV sub-block (case 2) + a single weapon-hit (tag==2, flags=0
// → 17 B) + EOB. Exercises decode_frame_update's env + hits capture.
std::vector<uint8_t> make_frame_update_env_hit(int32_t ax, int32_t ay, int32_t az,
                                               uint16_t hit_target, uint8_t adm,
                                               uint16_t cpx, uint16_t cpy, uint16_t cpz) {
	std::vector<uint8_t> b;
	put_u32(b, uint32_t(ax)); put_u32(b, uint32_t(ay)); put_u32(b, uint32_t(az));
	b.push_back(0);           // flags1
	b.push_back(2);           // flags2 -> sub-block 2 (ENV)
	put_u16(b, 1000);         // fog_dist
	put_u16(b, 0xFF00);       // fog_accel
	put_u16(b, 0x7957);       // tod_fixed
	b.push_back(0);           // quake
	b.push_back(15);          // cloud_scroll
	b.push_back(0);           // cloud_byte2
	b.push_back(0);           // overcast
	b.push_back(0);           // env_trail
	b.push_back(0); put_u16(b, 0xFFFF); put_u16(b, 200); put_u16(b, 0); // 7-B tail
	b.push_back(2);           // event tag 2 = weapon-hit
	b.push_back(0);           // flags (no parent / no weapon -> 17 B)
	b.push_back(adm);         // adm_index
	b.push_back(0);           // hit_subtype
	put_u16(b, hit_target);   // target_handle
	put_u16(b, 0xF7FF);       // damage_extra
	put_u16(b, cpx); put_u16(b, cpy); put_u16(b, cpz); // compressed pos
	put_u16(b, 0); put_u16(b, 0);                      // yaw / pitch
	b.push_back(0);           // event tag 0 = EOB
	return b;
}

// End-to-end event stream: a kill (S2C 0x1E), a fire (C2S 0x06) and a 0x0A
// carrying an env snapshot + a weapon-hit, asserted out of build_replay_timeline.
void test_event_stream() {
	const int32_t ax = 1000000, ay = 2000000, az = 500000;
	const uint16_t kHitTgt = 0x0003;

	ClientAuth ca;
	ca.na = "tester";
	ca.ci = 1;
	ca.ck = 2;
	ca.scrk = std::string(kScrk);
	ServerAuth sa = build_server_auth(ca, 0x7F000001u, 32768, 0x55, kScrk);

	std::vector<net::PcapDatagram> dgrams;
	int f = 1;
	dgrams.push_back({32769, 32768, f++,
	                  nwu_outer_encode(SESSION_OPCODE_SERVER_AUTH,
	                                   server_auth_to_bytes(sa))});
	dgrams.push_back({32768, 32769, f++,
	                  nwu_outer_encode(SESSION_OPCODE_CLIENT_AUTH,
	                                   client_auth_to_bytes(ca))});
	dgrams.push_back({32769, 32768, f++,
	                  make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	                                     0x1E, make_game_event_1e(4, 5, 4, 0xFF, 0, 0))});
	dgrams.push_back({32768, 32769, f++,
	                  make_proto_payload(SESSION_OPCODE_PROTOCOL_MESSAGE, 0x06,
	                                     make_fired_round_06(0x0005, 24, 3000000,
	                                                         1000000, 3700000,
	                                                         -30000, -1200))});
	dgrams.push_back({32769, 32768, f++,
	                  make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, 0x0A,
	                                     make_frame_update_env_hit(ax, ay, az, kHitTgt,
	                                                               18, 0x5007, 0x4608, 0))});
	const std::vector<uint8_t> pcap = net::build_pcap_udp(dgrams);
	std::vector<net::PcapDatagram> got;
	EXPECT(net::read_pcap_udp(pcap.data(), pcap.size(), got));
	std::vector<CaptureDatagram> caps;
	for (auto &pk : got) caps.push_back({pk.frame_index, std::move(pk.payload)});
	const ReplayTimeline tl =
	    build_replay_timeline(decode_capture_to_messages(caps));

	int nfire = 0, nhit = 0, nkill = 0;
	const ReplayEvent *kill = nullptr, *fire = nullptr, *hit = nullptr;
	for (const auto &e : tl.events) {
		if (e.kind == ReplayEventKind::Fire) { nfire++; fire = &e; }
		else if (e.kind == ReplayEventKind::Hit) { nhit++; hit = &e; }
		else if (e.kind == ReplayEventKind::Kill) { nkill++; kill = &e; }
	}
	EXPECT(nkill == 1 && nfire == 1 && nhit == 1);
	if (kill) {
		EXPECT(kill->source == 5 && kill->target == 4);
		EXPECT(kill->event_type == 4 && kill->label == "STRCND04");
	}
	if (fire) {
		EXPECT(fire->source == 0x0005 && fire->adm_index == 24);
		EXPECT(fire->has_pos && fire->x == 3000000);
		EXPECT(fire->has_dir && fire->dir_x == -30000);
	}
	if (hit) {
		EXPECT(hit->target == kHitTgt && hit->adm_index == 18);
		EXPECT(hit->has_pos);
		EXPECT(hit->x == ax + network_decompress_fixedpoint(0x5007));
		EXPECT(hit->y == ay + network_decompress_fixedpoint(0x4608));
	}
	EXPECT(tl.environment.size() == 1);
	if (!tl.environment.empty()) {
		EXPECT(tl.environment[0].fog_dist == 1000);
		EXPECT(tl.environment[0].tod_fixed == 0x7957);
		EXPECT(tl.environment[0].cloud_scroll == 15);
	}
}

} // namespace

int main() {
	test_decompress_vectors();
	test_frame_update_fold();
	test_event_stream();

	// --- craft the inputs -----------------------------------------------------
	// Two pool-1 spawns: a "blue" entity at (-85,45) and a "red" one at (85,45).
	PoolSpawnRecord truck1;
	truck1.slot_id = 0x1002;
	truck1.item_type_id = 0x050E;
	truck1.pos_x = -5570560; // -85.0
	truck1.pos_y = 2949120;  //  45.0
	truck1.pos_z = 2480896;  //  37.85
	truck1.team_byte = 1;    // -> gate 0x0010

	PoolSpawnRecord truck2;
	truck2.slot_id = 0x1003;
	truck2.item_type_id = 0x050E;
	truck2.pos_x = 5570560; // 85.0
	truck2.pos_y = 2949120; // 45.0
	truck2.pos_z = 3356672; // 51.2
	truck2.team_byte = 2;

	PoolSpawnBatch spawn;
	spawn.entity_count = 2;
	spawn.records.push_back(truck1);
	spawn.records.push_back(truck2);
	const std::vector<uint8_t> spawn_inner = encode_pool_spawn_batch(spawn);

	// Two uplinks for entity 0x1002: first at the spawn position heading 90 deg
	// (BAM 0x4000 high), then nudged +1.0 in x heading 180 deg (BAM 0x8000 high).
	const std::vector<uint8_t> up1 =
	    make_uplink(0x1002, 0x050E, -5570560, 2949120, 2480896, int16_t(0x4000));
	const std::vector<uint8_t> up2 =
	    make_uplink(0x1002, 0x050E, -5505024, 2949120, 2480896, int16_t(0x8000));

	// SCRK delivery: a ServerAuth (recovers server_scrk for S2C) + a ClientAuth
	// (recovers client_scrk for C2S). Same key for both in this fixture.
	ClientAuth ca;
	ca.na = "tester";
	ca.ci = 1;
	ca.ck = 2;
	ca.scrk = std::string(kScrk);
	ServerAuth sa = build_server_auth(ca, 0x7F000001u, 32768, /*server_sk*/ 0x55,
	                                  kScrk);

	// --- assemble the capture in wire order -----------------------------------
	std::vector<net::PcapDatagram> dgrams;
	int f = 1;
	dgrams.push_back({32769, 32768, f++,
	                  nwu_outer_encode(SESSION_OPCODE_SERVER_AUTH,
	                                   server_auth_to_bytes(sa))});
	dgrams.push_back({32768, 32769, f++,
	                  nwu_outer_encode(SESSION_OPCODE_CLIENT_AUTH,
	                                   client_auth_to_bytes(ca))});
	dgrams.push_back({32769, 32768, f++,
	                  make_proto_payload(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	                                     0x0D, spawn_inner)});
	dgrams.push_back({32768, 32769, f++,
	                  make_proto_payload(SESSION_OPCODE_PROTOCOL_MESSAGE, 0x0C, up1)});
	dgrams.push_back({32768, 32769, f++,
	                  make_proto_payload(SESSION_OPCODE_PROTOCOL_MESSAGE, 0x0C, up2)});
	for (const auto &d : dgrams) EXPECT(!d.payload.empty());

	const std::vector<uint8_t> pcap = net::build_pcap_udp(dgrams);
	EXPECT(!pcap.empty());

	// --- read it back through the shared pipeline + assembler -----------------
	std::vector<net::PcapDatagram> got;
	EXPECT(net::read_pcap_udp(pcap.data(), pcap.size(), got));
	EXPECT(got.size() == dgrams.size());

	std::vector<CaptureDatagram> caps;
	for (auto &pk : got) caps.push_back({pk.frame_index, std::move(pk.payload)});
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(caps);

	// Sanity: the shared pipeline yielded exactly the three protocol messages
	// (one S2C 0x0D + two C2S 0x0C), in order.
	int n0d = 0, n0c = 0;
	for (const auto &m : msgs) {
		if (m.dir == 'S' && m.tag == 0x0D) n0d++;
		if (m.dir == 'C' && m.tag == 0x0C) n0c++;
	}
	EXPECT(n0d == 1);
	EXPECT(n0c == 2);

	const ReplayTimeline tl = build_replay_timeline(msgs);
	EXPECT(tl.entities.size() == 2);

	const ReplayEntity *e1002 = nullptr;
	const ReplayEntity *e1003 = nullptr;
	for (const auto &e : tl.entities) {
		if (e.handle == 0x1002) e1002 = &e;
		if (e.handle == 0x1003) e1003 = &e;
	}
	EXPECT(e1002 != nullptr);
	EXPECT(e1003 != nullptr);

	if (e1002) {
		EXPECT(e1002->pool == 1);
		EXPECT(e1002->type_id == 0x050E);
		EXPECT(e1002->team_known && e1002->team == 1);
		EXPECT(e1002->has_spawn);
		EXPECT(e1002->spawn.x == -5570560 && e1002->spawn.y == 2949120 &&
		       e1002->spawn.z == 2480896);
		// track = spawn + 2 uplinks.
		EXPECT(e1002->track.size() == 3);
		if (e1002->track.size() == 3) {
			EXPECT(e1002->track[0].source == ReplaySampleSource::Spawn);
			EXPECT(e1002->track[1].source == ReplaySampleSource::ClientUplink);
			EXPECT(e1002->track[1].x == -5570560 && e1002->track[1].y == 2949120);
			EXPECT(std::fabs(e1002->track[1].heading_deg - 90.0) < 0.01);
			EXPECT(e1002->track[2].x == -5505024); // nudged +1.0
			EXPECT(std::fabs(e1002->track[2].heading_deg - 180.0) < 0.01);
		}
	}
	if (e1003) {
		EXPECT(e1003->team_known && e1003->team == 2);
		EXPECT(e1003->track.size() == 1); // spawn only — never uplinked
	}

	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("PASS: replay timeline from an inline capture — spawns + C2S 0x0C "
	            "uplink + S2C 0x0A decompressed motion (Network_DecompressFixedPoint "
	            "+ header anchor) through the full shared pipeline.\n");
	return 0;
}
