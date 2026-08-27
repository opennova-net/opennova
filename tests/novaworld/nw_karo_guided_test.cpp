// Wire validation of the §5.15 guided codec against REAL guided traffic — the
// leg D-NET-64 deferred as "no capture in hand carries rocket/missile state".
// The reference wire is a local retail Karo Highlands capture (Stinger duels;
// the fork's measured wire: 709 S2C 0x44 records, 100% stng, 4 shooters).
// Asset-gated (docs/asset-gated-tests.md): set NW_KARO_GUIDED_PCAP to the
// local pcap; absent, this SKIPS AS PASS and CI stays green.
//
// [orig: NapiNPClientMsg_0x044 @0x422710 -> Entity_SerializeGuidedMissileState
//  @0x447C50; sub-header [u16 shooter][i16 netId][u8 group]]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/wire_capture.h>
#include <base/pcapio/pcap_reader.h>

using namespace opennova;

int main() {
	std::string path;
	if (const char *env = std::getenv("NW_KARO_GUIDED_PCAP"); env && *env) path = env;
	std::vector<net::PcapDatagram> pkts;
	if (path.empty() || !net::read_pcap_udp_file(path, pkts)) {
		std::printf("[skip] Karo guided capture not found (set NW_KARO_GUIDED_PCAP) — '%s'\n",
		            path.c_str());
		return 0;   // skip clean — CI stays green without the local capture
	}

	std::vector<CaptureDatagram> caps;
	for (const net::PcapDatagram &p : pkts) {
		CaptureDatagram c;
		c.frame_index = p.frame_index;
		c.src_port = p.srcport;
		c.dst_port = p.dstport;
		c.payload = p.payload;
		caps.push_back(std::move(c));
	}
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(caps);

	int total = 0, undecodable = 0;
	std::map<int, int> per_group;
	std::set<int16_t> missiles;
	std::set<uint16_t> shooters;
	std::set<uint16_t> lock_targets;
	for (const InGameMessage &m : msgs) {
		if (m.settings_update) continue;
		if (m.dir != 'S' || static_cast<uint8_t>(m.tag & 0xFF) != 0x44) continue;
		++total;
		EntityRoutedPacket pkt;
		if (!decode_entity_routed_packet(m.payload.data(), m.payload.size(), pkt)) {
			++undecodable;
			continue;
		}
		per_group[pkt.subtype]++;
		missiles.insert(pkt.net_id);
		shooters.insert(pkt.field0);
		const auto group = static_cast<GuidedFieldGroup>(pkt.subtype);
		GuidedRecord rec;
		size_t used = 0;
		switch (group) {
			case GuidedFieldGroup::Status:
			case GuidedFieldGroup::ClearTarget:
			case GuidedFieldGroup::TargetPos:
			case GuidedFieldGroup::TargetTypePos:
			case GuidedFieldGroup::Pos:
			case GuidedFieldGroup::AttachOffsets:
				if (!decode_guided_field_group(GuidedMode::ReadFull, group, pkt.body,
				                               pkt.body_size, rec, used)) {
					++undecodable;
					continue;
				}
				if ((group == GuidedFieldGroup::TargetPos ||
				     group == GuidedFieldGroup::TargetTypePos) &&
				    rec.target_slot != 0xFFFF)
					lock_targets.insert(rec.target_slot);
				break;
			default:
				++undecodable;
				continue;
		}
	}

	std::printf("[karo-guided] total=%d undecodable=%d missiles=%zu shooters=%zu "
	            "lock_targets=%zu\n",
	            total, undecodable, missiles.size(), shooters.size(), lock_targets.size());
	for (const auto &g : per_group)
		std::printf("[karo-guided] group %d: %d\n", g.first, g.second);

	int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

	// The wire must carry guided traffic and every record must decode — this is
	// the D-NET-64 validation the codec's round-trip test could not provide.
	CHECK(total > 0);
	CHECK(undecodable == 0);
	CHECK(!missiles.empty());
	CHECK(!shooters.empty());
	CHECK(per_group.count(3) + per_group.count(4) > 0);   // locks carry steer points
	// MEASURED PINS — the 2026-08-18 run of this test against
	// matches/got-rolled-2026-07-11/map-3-KaroHighlandsTAC.pcap. (The fork's
	// own wire measurement of its Karo reference — 709 records, 4 shooters —
	// was a different slice of the same match; these pins are THIS file's.)
	// A change here means the decoder or the capture changed — re-measure,
	// never guess.
	CHECK(total == 649);
	CHECK(missiles.size() == 12);
	CHECK(shooters.size() == 3);
	CHECK(lock_targets.size() == 3);
	CHECK(per_group[1] == 110);   // Status (detonate/terminate)
	CHECK(per_group[3] == 198);   // TargetPos (lock + steer)
	CHECK(per_group[5] == 341);   // Pos (flare-decoy steer)

	if (failures == 0) std::printf("nw_karo_guided_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
