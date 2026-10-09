// The mission-authored halves of two S2C payloads on 00TRg, pinned against the
// retail capture: the 0x7E briefing body — [info]/briefing3 then
// [info]/briefing2 (or briefing), each NUL-terminated cp1252
// [orig: NetPacket_WriteBriefingText @0x506620] — and the 0x0F world-state's
// deploy-map location labels (Locations/LOCATION001..) plus the blue player
// route (exactly one blue path, DoesNotLoop|PlayerRoute, marker [12] — retail's
// pool-3 slot 0x300c).
// Gated on OPENNOVA_JO_DIR with the revx02 expansion mounted (the capture's
// data set); an extracted OPENNOVA_JO_ASSETS tree carrying the same 00TRg pair
// serves too.
#include "common/retail_paths.h"

#include <base/io/sha256.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/mission/bms.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/mission/runtime_boot.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

// The retail 0x7E witness on revx02's 00TRg.
constexpr size_t kBriefing3Len = 0;
constexpr size_t kBriefing2Len = 810;
const char *const kBodySha256 =
		"99ffe3380dd8fe7d06621c848532241d5426d83ac6832d12189984c315d94c07";

void put_cstr(std::vector<uint8_t> &b, const std::string &s) {
	b.insert(b.end(), s.begin(), s.end());
	b.push_back(0);
}

bool mount(ResourceIndex &index, std::string &where) {
	const std::string install = retail::install();
	if (!install.empty() && index.scan(install, "revx02") && index.mounted_expansion() == "revx02" &&
			index.has_file("00TRg.bms")) {
		where = install + " (revx02)";
		return true;
	}
	const std::string assets = retail::assets();
	if (!assets.empty() &&
			(index.scan(assets) || index.scan(assets, std::string(), VfsMountMode::LooseOnly)) &&
			index.has_file("00TRg.bms")) {
		where = assets + " (loose)";
		return true;
	}
	return false;
}

} // namespace

int main() {
	ResourceIndex index;
	std::string where;
	if (!mount(index, where))
		return retail::skip("OPENNOVA_JO_DIR with the revx02 expansion (or OPENNOVA_JO_ASSETS) carrying 00TRg");
	std::printf("authored_payload: 00TRg from %s\n", where.c_str());

	// --- 0x7E briefing body.
	mission::BootFileSource files;
	files.has_file = [&index](const std::string &name) { return index.has_file(name); };
	files.read_file = [&index](const std::string &name, std::vector<uint8_t> &out) {
		return index.read_file(name, out);
	};
	std::vector<uint8_t> text_bytes;
	const mission::MissionTextSource source = mission::resolve_mission_text(files, "00TRg", text_bytes);
	if (!expect(source != mission::MissionTextSource::kNone, "00TRg resolves a mission text table"))
		return 1;
	rtxt::File table;
	std::string error;
	if (!expect(rtxt::parse(text_bytes.data(), text_bytes.size(), table, error), "the table parses")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	std::string briefing3, briefing2;
	if (const rtxt::Entry *e = table.find_in_section("info", "briefing3")) briefing3 = e->text;
	if (const rtxt::Entry *e = table.find_in_section("info", "briefing2")) briefing2 = e->text;
	if (briefing2.empty())
		if (const rtxt::Entry *e = table.find_in_section("info", "briefing")) briefing2 = e->text;
	std::vector<uint8_t> body;
	put_cstr(body, briefing3);
	put_cstr(body, briefing2);
	const std::string digest = opennova::io::sha256_hex(body.data(), body.size());
	std::printf("authored_payload: briefing first_len=%zu second_len=%zu body_len=%zu sha256=%s\n",
			briefing3.size(), briefing2.size(), body.size(), digest.c_str());
	expect(briefing3.size() == kBriefing3Len, "briefing3 length matches the retail 0x7E witness");
	expect(briefing2.size() == kBriefing2Len, "briefing2 length matches the retail 0x7E witness");
	expect(digest == kBodySha256, "the 0x7E body digest matches the retail witness");

	// --- 0x0F locations.
	const std::string loc1 = table.get_in_section("Locations", "LOCATION001");
	const std::string loc2 = table.get_in_section("Locations", "LOCATION002");
	std::printf("authored_payload: locations [%s] [%s]\n", loc1.c_str(), loc2.c_str());
	expect(loc1 == "Weapons Cache" && loc2 == "Rebel Outpost",
			"00TRg location strings match the retail 0x0F witness");

	// --- 0x0F blue route.
	std::vector<uint8_t> bms_bytes;
	bms::File mission;
	if (!expect(index.read_file("00TRg.bms", bms_bytes) &&
						bms::parse(bms_bytes.data(), bms_bytes.size(), mission, error),
				"00TRg.bms parses"))
		return 1;
	std::vector<const bms::WaypointRecord *> blue;
	for (const bms::WaypointRecord &rec : mission.waypoint_records)
		if ((static_cast<uint32_t>(rec.flags) & static_cast<uint32_t>(bms::WaypointFlags::PlayerRoute)) != 0u)
			blue.push_back(&rec);
	std::printf("authored_payload: %zu waypoint paths, %zu blue\n", mission.waypoint_records.size(),
			blue.size());
	if (expect(blue.size() == 1, "00TRg has exactly one blue player waypoint path")) {
		const bms::WaypointRecord &b = *blue[0];
		std::printf("authored_payload: blue flags=%u markers=%zu first=%u\n",
				static_cast<uint32_t>(b.flags), b.waypoint_numbers.size(),
				b.waypoint_numbers.empty() ? 0u : b.waypoint_numbers[0]);
		expect(static_cast<uint32_t>(b.flags) == 3u, "the blue path is DoesNotLoop|PlayerRoute");
		expect(b.waypoint_numbers.size() == 1 && b.waypoint_numbers[0] == 12u,
				"the blue path's marker list is [12] (retail's pool-3 slot 0x300c)");
	}

	if (failures == 0) std::printf("authored_payload_00trg: OK\n");
	return failures == 0 ? 0 : 1;
}
