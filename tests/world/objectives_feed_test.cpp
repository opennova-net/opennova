// The SP objectives panel's row feed (world/objectives_feed.h), pinned where
// it used to live in the Godot binding (ADR 0040 ladder E0): the 1..8 slot
// walk with its 0/255 terminator, the show-win row gate, the won checkmark,
// the STRWINCOND%03i key through the game-text seam and the empty text of an
// absent key or table. [orig: HUD_DrawWinConditions @0x5ba940 / @0x5ba9e0]
#include <runtime/world/objectives_feed.h>

#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A mission text table that knows two win-condition lines and records the
// section every lookup asked for.
struct Table {
	std::vector<std::string> sections;
	opennova::hud::GameTextLookup lookup() {
		return [this](const char *section, const char *key, const char *fallback) {
			sections.emplace_back(section);
			if (std::strcmp(key, "STRWINCOND003") == 0) return std::string("Destroy the radar");
			if (std::strcmp(key, "STRWINCOND012") == 0) return std::string("Rescue the pilot");
			return std::string(fallback);
		};
	}
};

void test_row_walk() {
	World w;
	SubgoalState &sg = w.script.subgoals;
	sg.win_text_ids[1] = 3;
	sg.win_text_ids[2] = 5;  // no text authored for this id
	sg.win_text_ids[3] = 12; // hidden: its show-win bit is clear
	sg.win_text_ids[4] = 12;
	sg.win_text_ids[5] = 0; // the terminator; later slots never walk
	sg.win_text_ids[6] = 3;
	sg.show_win = (1u << 1) | (1u << 2) | (1u << 4) | (1u << 6);
	sg.won = (1u << 2) | (1u << 6);

	Table table;
	std::vector<opennova::hud::HudObjectiveRow> rows;
	fill_objective_rows(w, table.lookup(), rows);
	CHECK(rows.size() == 3);
	CHECK(rows[0].text == "Destroy the radar");
	CHECK(!rows[0].done);
	CHECK(rows[1].text.empty()); // an absent key reads as the empty fallback
	CHECK(rows[1].done);
	CHECK(rows[2].text == "Rescue the pilot");
	CHECK(!rows[2].done);
	CHECK(table.sections.size() == 3);
	CHECK(table.sections[0] == "WinConditions");

	// 255 terminates like 0.
	sg.win_text_ids[5] = 255;
	fill_objective_rows(w, table.lookup(), rows);
	CHECK(rows.size() == 3);

	// A second fill starts from an empty vector; no table means empty text.
	fill_objective_rows(w, opennova::hud::GameTextLookup{}, rows);
	CHECK(rows.size() == 3);
	CHECK(rows[0].text.empty());
	CHECK(rows[1].done);
}

void test_empty_panel() {
	World w;
	std::vector<opennova::hud::HudObjectiveRow> rows;
	rows.push_back(opennova::hud::HudObjectiveRow{});
	Table table;
	fill_objective_rows(w, table.lookup(), rows);
	CHECK(rows.empty()); // slot 1's id is 0: the panel hides
	CHECK(table.sections.empty());
}

} // namespace

int main() {
	test_row_walk();
	test_empty_panel();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("objectives_feed_test OK\n");
	return 0;
}
