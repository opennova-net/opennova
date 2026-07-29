// Self-contained, CI-runnable decoder regression — no capture fixtures.
//
// Each case crafts a tiny in-memory pcap (apps/common build_pcap_udp), encodes
// a known pool batch through the FULL S2C stack (inner encoder -> 0x83 protocol
// frame -> SCRK -> outer NWU -> NAPI envelope -> UDP datagram), then reads that
// pcap back through the shared reader and the exact decode pipeline nw_pp and
// the real-capture harness use, asserting the decoded records equal the crafted
// input. Exercises build_pcap_udp + read_pcap_udp + envelope + NWU + SCRK +
// protocol reassembly + the §5.11 / §5.12 pool decoders end to end, on a few
// dozen bytes — so the decoders stay covered in CI without the .scratch capture.

#include <napi/envelope.h>
#include <novacrypto/nwu.h>
#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/protocol_message.h>
#include <npwire/replay_timeline.h>
#include <npwire/session_keys.h>
#include <npwire/wire_capture.h>

#include <pcapio/pcap_reader.h>

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

// Wrap an inner S2C protocol-message body (e.g. a 0x0D batch) into the exact
// on-wire UDP payload a server emits: 0x83 protocol frame (SCRK-encrypted) ->
// outer NWU -> NAPI envelope. Mirror of the test/nw_pp decode_outer +
// process_protocol path, run backwards.
std::vector<uint8_t> make_s2c_udp_payload(uint8_t tag,
                                          const std::vector<uint8_t> &inner) {
	ProtocolMessage msg = make_protocol_message(tag, inner);
	ProtocolPacketHeader hdr{};
	hdr.session_id = 0x1234;
	std::vector<uint8_t> proto; // NWU-plaintext, SCRK-encrypted
	encode_protocol_packet_plaintext(hdr, {msg}, kScrk, proto);
	// Outer NWU transform over the post-opcode region. nwu_encrypt/nwu_decrypt
	// are inverses (reference_nwu_names_swapped): the receiver's decode_outer
	// applies nwu_encrypt to DECRYPT, so the sender must apply nwu_decrypt here.
	nwu_decrypt(proto.data(), proto.size(), SESSION_NWU_KEY);
	std::vector<uint8_t> stripped;
	stripped.reserve(proto.size() + 1);
	stripped.push_back(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE); // 0x83
	stripped.insert(stripped.end(), proto.begin(), proto.end());
	std::vector<uint8_t> raw(stripped.size() + 4);
	size_t out = 0;
	if (napi_envelope_encode(stripped.data(), stripped.size(), raw.data(),
	                         raw.size(), &out) != 0)
		return {};
	raw.resize(out);
	return raw;
}

// Reverse of make_s2c_udp_payload, then dispatch the inner tag. Returns the
// reassembled inner body for `want_tag`, or empty.
std::vector<uint8_t> decode_s2c_udp_payload(const std::vector<uint8_t> &raw,
                                            int want_tag) {
	std::vector<uint8_t> stripped(raw.size());
	size_t out = 0;
	if (napi_envelope_decode(raw.data(), raw.size(), stripped.data(),
	                         stripped.size(), &out) != 0)
		return {};
	stripped.resize(out);
	if (stripped.empty() || stripped[0] != SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE)
		return {};
	std::vector<uint8_t> body(stripped.begin() + 1, stripped.end());
	nwu_encrypt(body.data(), body.size(), SESSION_NWU_KEY);
	ProtocolPacketHeader hdr;
	std::vector<ProtocolMessage> msgs;
	if (!decode_protocol_packet_plaintext(body.data(), body.size(), kScrk, hdr, msgs))
		return {};
	ProtocolReassemblyState rs;
	for (const auto &pm : msgs) {
		std::vector<uint8_t> assembled;
		if (!reassemble_protocol_payload(rs, pm, assembled)) continue;
		if (int(pm.full_tag) == want_tag) return assembled;
	}
	return {};
}

// --- the pcap reader/builder on their own --------------------------------
void test_pcap_roundtrip() {
	std::vector<net::PcapDatagram> in(3);
	in[0] = {32768, 32769, 1, {0xDE, 0xAD, 0xBE, 0xEF}};
	in[1] = {7597, 12345, 2, {}}; // empty payload
	in[2] = {32769, 32768, 3, {0x01, 0x02, 0x03, 0x04, 0x05}};

	std::vector<uint8_t> pcap = net::build_pcap_udp(in);
	EXPECT(!pcap.empty());

	std::vector<net::PcapDatagram> got;
	EXPECT(net::read_pcap_udp(pcap.data(), pcap.size(), got));
	EXPECT(got.size() == in.size());
	for (size_t i = 0; i < got.size() && i < in.size(); ++i) {
		EXPECT(got[i].srcport == in[i].srcport);
		EXPECT(got[i].dstport == in[i].dstport);
		EXPECT(got[i].payload == in[i].payload);
		EXPECT(got[i].frame_index == int(i) + 1);
	}
}

// --- full stack: a crafted 0x0D vehicle spawn through a tiny pcap ----------
void test_pool_spawn_0d_via_pcap() {
	PoolSpawnRecord r;
	r.slot_id = 0x1002;          // pool-1 slot 2
	r.item_type_id = 0x050E;     // the dvxi5 truck type
	r.pos_x = -5570560;          // -85.0
	r.pos_y = 2949120;           //  45.0
	r.pos_z = 2480896;           //  37.857
	r.euler_z = int32_t(0x40000000u); // entity+16 yaw heading 90 deg BAM -> gate 0x0001 (D-NET-86)
	r.team_byte = 2;             // -> gate 0x0010
	r.target_handle = 0x2044;    // -> gate 0x0200
	PoolSpawnBatch in;
	in.entity_count = 1;
	in.records.push_back(r);

	std::vector<uint8_t> inner = encode_pool_spawn_batch(in);
	std::vector<uint8_t> raw = make_s2c_udp_payload(0x0D, inner);
	std::vector<net::PcapDatagram> dgrams(1);
	dgrams[0] = {32768, 32769, 1, raw};
	std::vector<uint8_t> pcap = net::build_pcap_udp(dgrams);

	std::vector<net::PcapDatagram> got;
	EXPECT(net::read_pcap_udp(pcap.data(), pcap.size(), got));
	EXPECT(got.size() == 1);
	if (got.empty()) return;

	std::vector<uint8_t> assembled = decode_s2c_udp_payload(got[0].payload, 0x0D);
	EXPECT(!assembled.empty());
	PoolSpawnBatch out;
	EXPECT(decode_pool_spawn_batch(assembled.data(), assembled.size(), out));
	EXPECT(out.records.size() == 1);
	if (out.records.empty()) return;
	const auto &d = out.records[0];
	EXPECT(d.item_type_id == 0x050E);
	EXPECT(d.slot_id == 0x1002);
	EXPECT(d.pos_x == -5570560 && d.pos_y == 2949120 && d.pos_z == 2480896);
	EXPECT((d.spawn_flags & 0x0001) != 0);          // yaw-heading gate (was mislabeled velocity)
	EXPECT(d.euler_z == int32_t(0x40000000u));      // entity+16 yaw survives the full stack
	EXPECT((d.spawn_flags & 0x0010) != 0);
	EXPECT(d.team_byte == 2);
	EXPECT((d.spawn_flags & 0x0200) != 0);
	EXPECT(d.target_handle == 0x2044);
}

// --- full stack: a crafted 0x20 start marker through a tiny pcap -----------
void test_pool3_sync_0x20_via_pcap() {
	Pool3SyncRecord r;
	r.item_type_id = 0x1774;     // Red start marker
	r.pos_x = 4587520;           //  70.0
	r.pos_y = 1638400;           //  25.0
	r.pos_z = 3633152;           //  55.43
	r.movement_val = 0xC0000000; // 270 deg BAM -> gate 0x01
	r.net_handle = 8;            // authored bms id (always present)
	r.team_byte = 2;             // -> gate 0x08
	Pool3SyncBatch in;
	in.start_index = 0;
	in.entity_count = 1;
	in.records.push_back(r);

	std::vector<uint8_t> inner = encode_pool3_sync_batch(in);
	std::vector<uint8_t> raw = make_s2c_udp_payload(0x20, inner);
	std::vector<net::PcapDatagram> dgrams(1);
	dgrams[0] = {32768, 32769, 1, raw};
	std::vector<uint8_t> pcap = net::build_pcap_udp(dgrams);

	std::vector<net::PcapDatagram> got;
	EXPECT(net::read_pcap_udp(pcap.data(), pcap.size(), got));
	if (got.empty()) { EXPECT(false); return; }

	std::vector<uint8_t> assembled = decode_s2c_udp_payload(got[0].payload, 0x20);
	EXPECT(!assembled.empty());
	Pool3SyncBatch out;
	EXPECT(decode_pool3_sync_batch(assembled.data(), assembled.size(), out));
	EXPECT(out.records.size() == 1);
	if (out.records.empty()) return;
	const auto &d = out.records[0];
	EXPECT(d.item_type_id == 0x1774);
	EXPECT(d.pos_x == 4587520 && d.pos_y == 1638400 && d.pos_z == 3633152);
	EXPECT((d.flags_byte & 0x01) != 0);
	EXPECT(d.movement_val == 0xC0000000u);
	EXPECT((d.flags_byte & 0x08) != 0);
	EXPECT(d.team_byte == 2);
	EXPECT(d.net_handle == 8);
}

// --- the spawn yaw heading flows to the timeline (D-NET-86) ----------------
// A pool spawn's 0x01-gated euler_z (entity+16) is the engine yaw heading, NOT
// velocity. It must surface as the spawn-pose heading the spectator renders, and
// (90 - heading) must recover the authored BMS yaw the editor places from — so a
// net static sits exactly where ONED's bms_to_godot_basis would put it. Before the
// fix this field was dropped and every static stood at heading 0 (faced east).
void test_spawn_heading_flows_to_timeline() {
	const double bms_yaw = 30.0;                 // what an author would set in the .bms
	const double engine_heading = 90.0 - bms_yaw; // the engine/wire frame = 60 deg
	const uint32_t yaw_bam = uint32_t((uint64_t(int(engine_heading)) << 32) / 360);

	PoolSpawnRecord r;
	r.slot_id = 0x1003;          // pool-1 slot 3 -> handle 0x1003
	r.item_type_id = 0x050E;
	r.pos_x = 1000; r.pos_y = 2000; r.pos_z = 3000;
	r.euler_z = int32_t(yaw_bam); // entity+16 yaw heading -> gate 0x0001
	PoolSpawnBatch in;
	in.entity_count = 1;
	in.records.push_back(r);

	// Inner encode/decode round-trip preserves the renamed field.
	std::vector<uint8_t> inner = encode_pool_spawn_batch(in);
	PoolSpawnBatch rt;
	EXPECT(decode_pool_spawn_batch(inner.data(), inner.size(), rt));
	EXPECT(rt.records.size() == 1 && rt.records[0].euler_z == int32_t(yaw_bam));

	// Flow it through the timeline as a server 0x0D message.
	InGameMessage m;
	m.frame_index = 1;
	m.dir = 'S';
	m.tag = 0x0D;
	m.payload = inner;
	ReplayTimeline tl = build_replay_timeline({m});

	const ReplayEntity *e = nullptr;
	for (const auto &ent : tl.entities)
		if (ent.handle == 0x1003) { e = &ent; break; }
	EXPECT(e != nullptr);
	if (!e) return;
	EXPECT(e->has_spawn && e->spawn.has_heading);
	EXPECT(std::fabs(e->spawn.heading_deg - engine_heading) < 0.5);   // ~60 deg on the wire
	// Placement-equivalence: 90 - heading == the authored BMS yaw the editor uses.
	EXPECT(std::fabs((90.0 - e->spawn.heading_deg) - bms_yaw) < 0.5); // ~30 deg
}

} // namespace

int main() {
	test_pcap_roundtrip();
	test_pool_spawn_0d_via_pcap();
	test_pool3_sync_0x20_via_pcap();
	test_spawn_heading_flows_to_timeline();
	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("PASS: inline-pcap round-trip of the 0x0D / 0x20 pool decoders "
	            "through the full S2C stack.\n");
	return 0;
}
