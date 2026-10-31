#include <runtime/mission/campaign.h>

#include <base/io/ascii_config.h>
#include <base/io/cp1252.h>
#include <formats/rtxt/rtxt.h>

namespace opennova::campaign {

namespace {

// A mission table entry by file name: the first whose name matches without case, -1 for none
// (an empty table finds none) [orig: sub_55FD20 - the walks @0x55ffd5..0x55ffff and
// @0x5600c9..0x5600f8, _stricmp over entry+0 (dfx2.exe)]. _stricmp folds through the ".ACP"
// locale; the names compare as their cp1252 bytes, as the catalog's own sort does (D-MNU-39): the
// file's token is its bytes, a table name the UTF-8 the scan made of it.
int find_row(const std::vector<std::string> &files, const char *name) {
	for (size_t i = 0; i < files.size(); ++i)
		if (cp1252_stricmp(utf8_to_retail_text(files[i]), name) == 0) return static_cast<int>(i);
	return -1;
}

bool keyword(const io::ConfigTokens &line, const char *word) {
	return cp1252_stricmp(line.token(0), word) == 0;
}

} // namespace

CampaignList parse(const char *text, size_t size, std::vector<mission_catalog::CampaignWords> &rows,
		const std::vector<std::string> &files, const rtxt::File *menu_text) {
	// The static table, zeroed at boot; `count` its counted slots [orig: dword_25DB330, 0
	// @0x5601b5].
	std::array<Campaign, kMaxCampaigns> slots{};
	size_t count = 0;
	CampaignList out;
	io::for_each_config_line(text, size, [&](io::ConfigTokens &line) {
		if (count >= kMaxCampaigns) return; // [orig: @0x55fd2e]
		Campaign &slot = slots[count];
		const char *arg = line.token(1);
		if (keyword(line, "end")) {
			++count; // [orig: @0x55fd4f]
		} else if (keyword(line, "begin")) {
			slot = Campaign{}; // [orig: memset 0x2DC @0x55fd86]
			slot.name = arg;   // [orig: @0x55fd90..0x55fdad]
		} else if (keyword(line, "depend")) {
			// The counted campaigns in order, each matched against every name on the line: a
			// depend list follows the campaigns' order, not the line's, and a campaign named twice
			// counts twice [orig: @0x55fddd..0x55fe68].
			for (size_t c = 0; c < count; ++c)
				for (int t = 1; t < line.count; ++t)
					if (slot.depends.size() < kMaxDepends &&
							cp1252_stricmp(slots[c].name, line.token(t)) == 0)
						slot.depends.push_back(static_cast<uint8_t>(c));
		} else if (keyword(line, "image")) {
			slot.image = arg; // [orig: @0x55fe8f..0x55feac]
		} else if (keyword(line, "complete_overlay")) {
			out.complete_overlay = arg; // [orig: byte_25DB320 @0x55feca..0x55fede]
		} else if (keyword(line, "briefing")) {
			// A key the table lacks leaves the slot's pointer as it was [orig: @0x55ff15].
			if (menu_text != nullptr) {
				if (const rtxt::Entry *entry = rtxt::find_with_text_override(*menu_text, kTextSection, arg)) {
					slot.has_briefing = true;
					slot.briefing = entry->text; // [orig: @0x55ff2c]
				}
			}
		} else if (keyword(line, "title")) {
			const rtxt::Entry *entry = menu_text != nullptr
					? rtxt::find_with_text_override(*menu_text, kTextSection, arg)
					: nullptr;
			slot.title = entry != nullptr ? entry->text : "??" + std::string(arg) + "??";
		} else if (keyword(line, "mission") && slot.missions.size() < kMaxMissions) {
			// The mission's slot is taken whether the table has its file or not; a name it lacks
			// keeps the zeroed row 0 and marks nothing [orig: @0x55ffcf, LABEL_53 @0x56016a].
			Mission mission;
			const int row = find_row(files, arg);
			if (row >= 0 && static_cast<size_t>(row) < rows.size()) {
				mission_catalog::CampaignWords &words = rows[static_cast<size_t>(row)];
				words.flags |= mission_catalog::kRowInCampaign;       // [orig: @0x560019]
				words.campaign = static_cast<int32_t>(count);          // [orig: @0x56002c]
				mission.row = static_cast<uint16_t>(row);              // [orig: @0x560050]
				words.campaign_mission = static_cast<int32_t>(slot.missions.size()); // [orig: @0x560071]
				// The entries it waits on, a name the table lacks passed over [orig: @0x56007b..0x560164].
				for (int t = 2; t < line.count; ++t) {
					if (mission.depends.size() >= kMaxMissionDepends) continue;
					const int depend = find_row(files, line.token(t));
					if (depend >= 0) mission.depends.push_back(static_cast<uint16_t>(depend));
				}
			}
			slot.missions.push_back(std::move(mission));
		}
	});
	out.campaigns.assign(slots.begin(), slots.begin() + static_cast<std::ptrdiff_t>(count));
	return out;
}

void apply_progress(CampaignList &list, const CompletionTable &progress,
		std::vector<mission_catalog::CampaignWords> &rows) {
	for (size_t c = 0; c < list.campaigns.size(); ++c) {
		Campaign &campaign = list.campaigns[c];
		campaign.complete = true;
		for (size_t m = 0; m < campaign.missions.size(); ++m) {
			Mission &mission = campaign.missions[m];
			mission.complete = progress[c][m];
			if (mission.complete != 0) {
				if (mission.row < rows.size()) rows[mission.row].flags |= mission_catalog::kRowComplete;
			} else {
				campaign.complete = false;
			}
		}
	}
}

bool available(const CampaignList &list, size_t campaign) {
	if (campaign >= list.campaigns.size()) return false;
	for (const uint8_t depend : list.campaigns[campaign].depends)
		if (depend >= list.campaigns.size() || !list.campaigns[depend].complete) return false;
	return true;
}

bool listed(const Mission &mission, const std::vector<mission_catalog::CampaignWords> &rows) {
	for (const uint16_t depend : mission.depends)
		if (depend >= rows.size() || (rows[depend].flags & mission_catalog::kRowComplete) == 0) return false;
	return true;
}

} // namespace opennova::campaign
