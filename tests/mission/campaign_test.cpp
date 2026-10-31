// DFX2-60: dfx2.exe's campaign.def parse and its progress walk over the mission table
// (runtime/mission/campaign.h) [orig: sub_55FD20 @0x55fd20, sub_560190 @0x560190 (dfx2.exe)].
// The inputs are authored here: a campaign.def text and a menu table's Campaign section.

#include "common/test_expect.h"
#include <formats/rtxt/rtxt.h>
#include <runtime/mission/campaign.h>

#include <string>
#include <vector>

namespace {

using namespace opennova;
using mission_catalog::CampaignWords;

#define CHECK(c) test_expect::expect((c), #c)

rtxt::File menu_text() {
	rtxt::File file;
	file.sections.push_back({ "Menu", 1 });
	file.sections.push_back({ "Campaign", 3 });
	file.entries.push_back({ "ID_ALPHA", "not the campaign's", {}, 0 });
	file.entries.push_back({ "ID_ALPHA", "Alpha", {}, 1 });
	file.entries.push_back({ "ID_ALPHA_BRIEF", "Alpha briefing", {}, 1 });
	file.entries.push_back({ "ID_BRAVO", "Bravo", {}, 1 });
	return file;
}

std::string lines(std::initializer_list<const char *> rows) {
	std::string text;
	for (const char *row : rows) text += std::string(row) + "\r\n";
	return text;
}

const std::vector<std::string> kFiles = { "ONE.BMS", "two.bms", "three.bms", "four.bms" };

void test_parse() {
	const rtxt::File table = menu_text();
	std::vector<CampaignWords> rows(kFiles.size());
	const std::string def = lines({
			"// a comment line, never a keyword",
			"complete_overlay\tdone.tga",
			"begin alpha",
			"\ttitle\tID_ALPHA // the Campaign section's, not Menu's",
			"\tbriefing ID_ALPHA_BRIEF",
			"\timage camp.tga",
			"\tmission one.bms",
			"\tmission nosuch.bms",
			"\tmission three.bms one.bms nosuch.bms two.bms",
			"end",
			"begin bravo",
			"\ttitle ID_BRAVO",
			"\tbriefing ID_MISSING",
			"\tdepend zulu alpha alpha",
			"\tmission two.bms",
			"end",
			"begin charlie",
			"\ttitle ID_MISSING",
			"\tmission four.bms",
	});
	const campaign::CampaignList list = campaign::parse(def.data(), def.size(), rows, kFiles, &table);
	CHECK(list.complete_overlay == "done.tga");
	// charlie never reached its `end`: not counted, though its mission line marked four.bms.
	CHECK(list.campaigns.size() == 2);
	if (list.campaigns.size() != 2) return;
	const campaign::Campaign &alpha = list.campaigns[0];
	CHECK(alpha.name == "alpha" && alpha.title == "Alpha" && alpha.image == "camp.tga");
	CHECK(alpha.has_briefing && alpha.briefing == "Alpha briefing");
	// A file the table lacks still takes its slot, with the zeroed row 0 and no depends.
	CHECK(alpha.missions.size() == 3);
	CHECK(alpha.missions[0].row == 0 && alpha.missions[0].depends.empty());
	CHECK(alpha.missions[1].row == 0 && alpha.missions[1].depends.empty());
	// A depend name the table lacks is passed over.
	CHECK(alpha.missions[2].row == 2 &&
			alpha.missions[2].depends == (std::vector<uint16_t>{ 0, 1 }));
	// The matched entries carry their campaign and place, matched without case.
	CHECK(rows[0].campaign == 0 && rows[0].campaign_mission == 0 &&
			rows[0].flags == mission_catalog::kRowInCampaign);
	CHECK(rows[2].campaign == 0 && rows[2].campaign_mission == 2);
	const campaign::Campaign &bravo = list.campaigns[1];
	// A missing title reads "??<key>??"; a missing briefing leaves none.
	CHECK(bravo.title == "Bravo" && !bravo.has_briefing);
	// depend walks the counted campaigns, matching each against every name: alpha twice.
	CHECK(bravo.depends == (std::vector<uint8_t>{ 0, 0 }));
	CHECK(rows[1].campaign == 1 && rows[1].campaign_mission == 0);
	CHECK(rows[3].campaign == 2 && (rows[3].flags & mission_catalog::kRowInCampaign) != 0);

	// No table: every title "??<key>??", no briefing.
	std::vector<CampaignWords> bare(kFiles.size());
	const campaign::CampaignList untitled = campaign::parse(def.data(), def.size(), bare, kFiles, nullptr);
	CHECK(untitled.campaigns.size() == 2 && untitled.campaigns[0].title == "??ID_ALPHA??" &&
			!untitled.campaigns[0].has_briefing);
}

void test_cap() {
	// Sixteen counted slots and nothing past them, a complete_overlay line among it.
	std::string def;
	for (int i = 0; i < 17; ++i) def += lines({ "begin c", "end" });
	def += lines({ "complete_overlay late.tga" });
	std::vector<CampaignWords> rows(kFiles.size());
	const campaign::CampaignList list = campaign::parse(def.data(), def.size(), rows, kFiles, nullptr);
	CHECK(list.campaigns.size() == campaign::kMaxCampaigns && list.complete_overlay.empty());
	// A depend list holds four, a mission's depends four.
	const std::string many = lines({ "begin a", "end", "begin b",
			"depend a a a a a", "mission one.bms two.bms two.bms two.bms two.bms two.bms", "end" });
	std::vector<CampaignWords> words(kFiles.size());
	const campaign::CampaignList capped = campaign::parse(many.data(), many.size(), words, kFiles, nullptr);
	CHECK(capped.campaigns.size() == 2 && capped.campaigns[1].depends.size() == campaign::kMaxDepends &&
			capped.campaigns[1].missions[0].depends.size() == campaign::kMaxMissionDepends);
}

void test_progress() {
	std::vector<CampaignWords> rows(kFiles.size());
	const std::string def = lines({ "begin a", "title T", "mission one.bms", "mission two.bms one.bms",
			"end", "begin b", "title U", "depend a", "mission three.bms", "end", "begin c", "end" });
	campaign::CampaignList list = campaign::parse(def.data(), def.size(), rows, kFiles, nullptr);
	campaign::CompletionTable progress{};
	progress[0][0] = 1;
	campaign::apply_progress(list, progress, rows);
	CHECK(list.campaigns[0].missions[0].complete == 1 && !list.campaigns[0].complete);
	CHECK((rows[0].flags & mission_catalog::kRowComplete) != 0 && (rows[1].flags & mission_catalog::kRowComplete) == 0);
	// A campaign with no missions is complete; b waits on a.
	CHECK(list.campaigns[2].complete && !campaign::available(list, 1) && campaign::available(list, 0));
	// two.bms waits on one.bms, complete now; three.bms waits on nothing.
	CHECK(campaign::listed(list.campaigns[0].missions[1], rows) && campaign::listed(list.campaigns[1].missions[0], rows));
	progress[0][1] = 1;
	campaign::apply_progress(list, progress, rows);
	CHECK(list.campaigns[0].complete && campaign::available(list, 1));
	// A mission waiting on an incomplete entry is not listed.
	campaign::Mission waiting;
	waiting.depends = { 2 };
	CHECK(!campaign::listed(waiting, rows));
}

} // namespace

int main() {
	test_parse();
	test_cap();
	test_progress();
	if (test_expect::failures != 0) {
		std::fprintf(stderr, "campaign_test: %d failure(s)\n", test_expect::failures);
		return 1;
	}
	std::printf("campaign_test: ok\n");
	return 0;
}
