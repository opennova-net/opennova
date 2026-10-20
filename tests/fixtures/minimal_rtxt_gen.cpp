// Synthetic RTXT fixtures: rebuild with --write and verify byte-exact round trips.
#include <formats/rtxt/rtxt.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::rtxt;

#include "common/file_io.h"

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

struct Row {
	const char *key;
	const char *text;
	int16_t x = 0;
	int16_t y = 0;
};

// Append one section; its keys are grouped contiguously (the engine derives an
// entry's index by accumulating the preceding sections' string_counts).
void add_section(File &f, const std::string &section, const std::vector<Row> &rows) {
	const uint32_t index = static_cast<uint32_t>(f.sections.size());
	f.sections.push_back({section, static_cast<uint32_t>(rows.size())});
	for (const Row &row : rows) {
		Entry e;
		e.key = row.key;
		e.text = row.text;
		e.position.x = row.x;
		e.position.y = row.y;
		e.section_index = index;
		f.entries.push_back(e);
	}
}

// Compose a single-section synthetic table.
File make_table(const std::string &section, const std::vector<std::pair<std::string, std::string>> &kv) {
	File f;
	std::vector<Row> rows;
	for (const auto &pair : kv) rows.push_back({pair.first.c_str(), pair.second.c_str()});
	add_section(f, section, rows);
	return f;
}

// --- fixtures/rtxt: the synthetic parity set ------------------------------

// A game table: several sections of unequal size, position hints on the HUD
// rows (the packed_xy word), texts of assorted lengths so the entry table,
// text blob and key run all move.
File synth_game_table() {
	File f;
	add_section(f, "Server", {
	                             {"STRSRV_MEDREQ", "Medic!"},
	                             {"STRSRV_JOINED", "%s has joined the game"},
	                             {"STRSRV_LEFT", "%s has left"},
	                             {"STRSRV_KICK", "You were removed by the host"},
	                         });
	add_section(f, "WepDes", {
	                             {"WEAP_SHORT_RIFLE", "Rifle"},
	                             {"WEAP_SHORT_PISTOL", "Pistol"},
	                             {"WEAP_SHORT_SATCHEL", "Satchel"},
	                             {"WEAP_ROUND_556", "5.56x45"},
	                             {"WEAP_ROUND_FLASH", "Flashbang"},
	                             {"WEAP_ROUND_NONE", ""},
	                         });
	add_section(f, "Hud", {
	                          {"HUD_HEALTH", "Health", 2, 739},
	                          {"HUD_STANCE", "Stance", 141, 739},
	                          {"HUD_AMMO", "{hot}Ammo", 810, 552},
	                      });
	add_section(f, "Loading", {
	                              {"LTGT_TDM", "Team Deathmatch"},
	                              {"LTGT_COOP", "Cooperative"},
	                              {"LOAD_WAIT", "Loading, please wait..."},
	                          });
	return f;
}

// A menu table: one long section, the shape of a screen's TEXT_RSRC ids.
File synth_menu_table() {
	File f;
	add_section(f, "Menu", {
	                           {"MM_Singleplayer", "Single Player"},
	                           {"MM_Multiplayer", "Multiplayer"},
	                           {"MM_Options", "Options"},
	                           {"MM_Exit", "Exit"},
	                           {"MP_Host", "Host"},
	                           {"MP_Join", "Join"},
	                           {"MP_Back", "Back"},
	                           {"OPT_Video", "Video"},
	                           {"OPT_Audio", "Audio"},
	                           {"OPT_Controls", "Controls"},
	                           {"NAV_ACCEPT", "Accept"},
	                           {"NAV_CANCEL", "Cancel"},
	                       });
	add_section(f, "Tips", {
	                           {"TIP_1", "Press F1 for help"},
	                       });
	return f;
}

// A mission sidecar with cp1252 text (accented letters, typographic quotes and
// dash) and odd-length strings, so the text blob ends off the dword boundary
// and the writer's zero padding up to the aligned section meta is exercised.
File synth_mission_table() {
	File f;
	add_section(f, "Info", {
	                           {"TITLE", "Training: Br\xFC" "cke \x96 Caf\xE9"},
	                           {"BRIEFING",
	                            "\x93Take the bridge,\x94 the captain said. Secure the caf\xE9 "
	                            "and hold until relieved."},
	                           {"OBJ_1", "Cross the Br\xFC" "cke"},
	                       });
	add_section(f, "Dialog", {
	                             {"DLG_1", "Go!"},
	                         });
	return f;
}

// The smallest shape: one section, one key, a one-character text.
File synth_tiny_table() {
	return make_table("Info", {{"TITLE", "x"}});
}

// A long briefing text from integer data: numbered sentences to `length` bytes.
std::string long_text(size_t length) {
	std::string out;
	for (int n = 1; out.size() < length; ++n) out += "Sector " + std::to_string(n) + " holds. ";
	out.resize(length);
	return out;
}

// The shapes the shipped tables take beyond the four above (the JO:CA install's RTXT bins, surveyed
// once on 2026-10-19 by the retired rtxt_jo_install_sweep): sections of no string first (72 tables),
// between others (153) and last (1); texts broken across lines with CR LF (243 texts), one with a tab;
// a key twice in one section (CP08.bin's STRWINCOND003 and 005); texts up to 11853 bytes; the same
// text under several keys; empty texts. The text blob ends on each of the four dword offsets in the
// shipped bins (the padding before the section meta 0..3 bytes); the four tables above leave 1, 1, 1
// and 2, this one 3 and synth_variants_aligned.bin 0.
File synth_variants_table() {
	File f;
	add_section(f, "Empty", {});
	add_section(f, "Brief", {
	                            {"BRIEF_1", "Hold the ridge.\r\nWait for the convoy.\r\n"},
	                            {"BRIEF_2", "Grid\t42 north."},
	                            {"BRIEF_LONG", "-"},
	                            {"BRIEF_3", ""},
	                            {"BRIEF_4", "Hold the ridge."},
	                            {"BRIEF_5", "Hold the ridge."},
	                        });
	add_section(f, "Between", {});
	add_section(f, "Win", {
	                          {"WIN_1", "Take the hill", 12, 300},
	                          {"WIN_1", "Take the hill again"},
	                          {"WIN_2", ""},
	                      });
	add_section(f, "Last", {});
	// The long text is built, not a literal.
	static const std::string kLong = long_text(11853);
	for (Entry &e : f.entries)
		if (e.key == "BRIEF_LONG") e.text = kLong;
	return f;
}

// A table of a section of no string ahead of one whose text blob ends on a dword (no padding before the
// section meta, 34 of the shipped bins).
File synth_variants_aligned_table() {
	File f;
	add_section(f, "Empty", {});
	add_section(f, "Info", {{"TITLE", "Bridge"}, {"OBJ_1", "Hold"}});
	return f;
}

using test_io::read_file;

struct Target {
	const char *name;
	File (*build)();
};

int run(const std::string &dir, const Target *targets, size_t count, bool write_mode) {
	int failures = 0;
	for (size_t i = 0; i < count; ++i) {
		const Target &t = targets[i];
		const std::string path = dir + "/" + t.name;
		File f = t.build();
		std::vector<uint8_t> bytes;
		std::string err;
		if (!expect(write(f, bytes, err), (std::string("write ") + t.name + ": " + err).c_str())) {
			++failures;
			continue;
		}
		// Round-trip: the emitted bytes parse and re-write identically.
		File reparsed;
		if (!expect(parse(bytes.data(), bytes.size(), reparsed, err),
		            (std::string("parse ") + t.name + ": " + err).c_str())) {
			++failures;
			continue;
		}
		std::vector<uint8_t> rewritten;
		if (!expect(write(reparsed, rewritten, err) && rewritten == bytes,
		            (std::string(t.name) + " is not byte-stable across a round-trip").c_str())) {
			++failures;
			continue;
		}
		if (write_mode) {
			std::ofstream o(path, std::ios::binary);
			o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		} else {
			std::vector<uint8_t> committed;
			if (!expect(read_file(path, committed),
			            (std::string("committed ") + t.name + " missing — run with "
			                                                  "--write")
			                .c_str())) {
				++failures;
				continue;
			}
			// A checkout without LFS pulled leaves a pointer file — skip clean
			// rather than fail a byte compare against the pointer text.
			if (test_io::is_lfs_pointer(committed)) {
				std::printf("[skip] %s is an unpulled LFS pointer\n", t.name);
				continue;
			}
			if (!expect(committed == bytes,
			            (std::string(t.name) + " committed bytes differ from the writer output — "
			                                    "regenerate with --write")
			                .c_str()))
				++failures;
		}
	}
	return failures;
}

} // namespace

int main(int argc, char **argv) {
#ifndef RTXT_FIXTURE_DIR
#define RTXT_FIXTURE_DIR "."
#endif
	// `--write` regenerates the committed tables; the ctest registration passes nothing.
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const Target fixture_targets[] = {
	    {"synth_game.bin", synth_game_table},
	    {"synth_menu.bin", synth_menu_table},
	    {"synth_mission.bin", synth_mission_table},
	    {"synth_tiny.bin", synth_tiny_table},
	    {"synth_variants.bin", synth_variants_table},
	    {"synth_variants_aligned.bin", synth_variants_aligned_table},
	};
	int failures = run(RTXT_FIXTURE_DIR, fixture_targets, sizeof(fixture_targets) / sizeof(fixture_targets[0]),
	                write_mode);
	if (failures == 0)
		std::printf("OK: synthetic RTXT parity set valid + byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
