#pragma once

// DFX2-60: the single-player campaigns dfx2.exe reads from campaign.def, each time its
// SINGLE_PLAYER screen populates: the file walked line by line through its keyword callback
// (begin / end / title / briefing / image / depend / mission / complete_overlay), then each
// campaign's missions marked complete from the current profile's completion bytes. The parse
// writes the campaign words of the mission table's entries it names (mission_catalog.h
// CampaignWords). Jointops.exe has no reader and its install ships no campaign.def.
// [orig: sub_560190 @0x560190 - campaign count 0 @0x5601b5, File_ParseASCIIFile("campaign.def",
//  sub_55FD20) @0x5601bb, the progress walk @0x5601c9..0x56023c; the parser sub_55FD20
//  @0x55fd20 (dfx2.exe)]
//
// The campaign slots are retail's static 16 x 0x2DC-byte table: name +0x000, title +0x040,
// briefing text pointer +0x140, image +0x144, depend count +0x154 and campaign indices +0x155,
// complete +0x159, mission count +0x15A and 32 x 12-byte missions from +0x15C (catalog row +0,
// complete +2, depend count +3, depend rows +4). Lines between a `begin` and its `end` write the
// slot the count names; `end` counts it, so a slot left without its `end` is never shown, though
// its `mission` lines have already marked their entries. The fixed buffers' overflow (a name past
// 63 characters, an image past 15, a complete_overlay past 15 running into the count) is not
// modeled.

#include <runtime/mission/mission_catalog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::rtxt {
struct File;
}

namespace opennova::campaign {

// [orig: "campaign.def" @0x5601bb; the menu table g_TextMenuUi's "Campaign" section
//  @0x55ff0b / @0x55ff5b (dfx2.exe)]
inline constexpr const char *kCampaignFile = "campaign.def";
inline constexpr const char *kTextSection = "Campaign";
// [orig: `cmp dword_25DB330, 10h` @0x55fd2e; the mission count's `cmp 20h` @0x55ffb5; the depend
//  count's `cmp 4` @0x55fdfb; a mission's depend count's `cmp 4` @0x5600ae (dfx2.exe)]
inline constexpr size_t kMaxCampaigns = 16;
inline constexpr size_t kMaxMissions = 32;
inline constexpr size_t kMaxDepends = 4;
inline constexpr size_t kMaxMissionDepends = 4;

struct Mission {
	uint16_t row = 0;              // the mission table entry (+0); a name the table lacks keeps 0
	uint8_t complete = 0;          // the profile's byte (+2), apply_progress's
	std::vector<uint16_t> depends; // the entries it waits on (+3 count, +4 rows)
};

struct Campaign {
	std::string name;      // begin's (+0x000)
	std::string title;     // title's text, or "??<key>??" (+0x040)
	bool has_briefing = false;
	std::string briefing;  // briefing's text (+0x140, null: has_briefing false)
	std::string image;     // image's file (+0x144)
	std::vector<uint8_t> depends; // the campaigns it waits on (+0x154 count, +0x155)
	bool complete = false; // every mission's byte set (+0x159), apply_progress's
	std::vector<Mission> missions; // (+0x15A count, +0x15C)
};

struct CampaignList {
	std::string complete_overlay; // byte_25DB320
	std::vector<Campaign> campaigns; // the counted slots (dword_25DB330)
};

// The completion bytes the reader takes from the current profile: player.sav's table, a byte a
// mission, +64 + 32 * campaign + mission.
using CompletionTable = std::array<std::array<uint8_t, kMaxMissions>, kMaxCampaigns>;

// The parse of campaign.def's bytes over the mission table `rows` (its entries' file names
// matched without case, the first match; a `mission` line marks its entry in-campaign with the
// campaign and the mission's place), the titles and briefings read in `menu_text`'s Campaign
// section (null: none, every title "??<key>??", no briefing).
// [orig: sub_55FD20 @0x55fd20 (dfx2.exe) - the full table @0x55fd2e; end @0x55fd43..0x55fd57;
//  begin's memset and name @0x55fd61..0x55fdb2; depend's walk over the counted campaigns
//  @0x55fdcf..0x55fe68; image @0x55fe7e..0x55feb4; complete_overlay @0x55febe..0x55fee6;
//  briefing's TextResource_FindEntryBySectionAndKey @0x55ff0b; title's
//  TextResource_GetStringWithFallback @0x55ff5b ("??%s??", @0x561e76); mission @0x55ffc1..0x56016a]
CampaignList parse(const char *text, size_t size, std::vector<mission_catalog::CampaignWords> &rows,
		const std::vector<std::string> &files, const rtxt::File *menu_text);

// The reader's progress walk: each campaign complete unless one of its missions' bytes is clear,
// each mission's byte copied in and its entry marked complete when set. The OR'd unlock word
// dword_B45E64 has no writer in dfx2.exe, so only the byte counts.
// [orig: sub_560190 @0x5601c9..0x56023c - the complete store @0x5601d7, the byte @0x5601e7, the
//  |= 4 @0x56020e, the clear @0x5601fa; dword_B45E64 read alone @0x5601f1 (dfx2.exe)]
void apply_progress(CampaignList &list, const CompletionTable &progress,
		std::vector<mission_catalog::CampaignWords> &rows);

// Whether the campaign list offers `campaign` for a pick: every campaign it depends on complete.
// [orig: sub_560190 @0x56028b..0x5602bb, CListWnd_SetRowAvailable(row, 0) @0x5602d9 (dfx2.exe)]
bool available(const CampaignList &list, size_t campaign);

// Whether a campaign's mission list shows `mission`: every entry it depends on complete.
// [orig: sub_560190 @0x5603d1..0x5603fb; sub_55FAD0 @0x55fbdc..0x55fc11 (dfx2.exe)]
bool listed(const Mission &mission, const std::vector<mission_catalog::CampaignWords> &rows);

} // namespace opennova::campaign
