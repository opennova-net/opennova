// engine/formats/score — score.ini read as ScoreConfig_LoadFile reads it, written as ScoreConfig_SaveFile
// writes it, over the file's modeled layout (textlayout), and the witnessed row-index ladder.
//
// Committed fixture: fixtures/score/score_sample.ini, AUTHORED for this test
// (not retail data). It carries three GAMETYPE blocks in the shipped file's
// index order so the block-order-is-the-row-index rule and the index-0 -> 2
// remap are both exercised, with the retail Co-op row's witnessed ENEMYKILL 5 /
// MEDICSAVE 2. Read with its layout and written again it is itself, byte for
// byte (its comments, its blank lines, its LF endings); a value changed changes
// its one line.
//
// The reader's lines: a line whose first or second character is '/' is read for
// nothing, every other line's tokens split at white space with quotes dropped,
// FIELD / VAR / EXP_FANFARE taking three tokens and a name of their tables, so a
// line the game reads nothing of is never an error. A minted file (no source
// text) is ScoreConfig_SaveFile's form.
//
// The retail leg (--retail) reads <OPENNOVA_JO_DIR>/score.ini: its 12 blocks, its
// Co-op row 2's ENEMYKILL 5 (the value the S2C 0x81 score mirror reproduces in the
// retail capture: 5, 10, 20, ... 220), and the writer's own form of it IS the
// shipped file byte for byte (the game wrote it with these defaults), as is the
// noted write.
#include <formats/score/score.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "common/retail_paths.h"

namespace {

using namespace opennova;

score::File parsed(const std::string &text) {
	score::File file;
	std::string error;
	score::parse(reinterpret_cast<const uint8_t *>(text.data()), text.size(), file, error);
	return file;
}

std::string text_of(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

int lines_changed(const std::string &a, const std::string &b) {
	const auto split = [](const std::string &t) {
		std::vector<std::string> out;
		size_t at = 0;
		while (at < t.size()) {
			const size_t end = t.find('\n', at);
			out.push_back(t.substr(at, end == std::string::npos ? std::string::npos : end - at));
			at = end == std::string::npos ? t.size() : end + 1;
		}
		return out;
	};
	const std::vector<std::string> x = split(a), y = split(b);
	if (x.size() != y.size()) return -1;
	int n = 0;
	for (size_t i = 0; i < x.size(); ++i) n += x[i] != y[i];
	return n;
}

} // namespace

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
	const std::string fixture =
			std::string(test_paths_repo_root(__FILE__)) + "/fixtures/score/score_sample.ini";
	const std::vector<uint8_t> bytes = test_io::read_file(fixture);
	TEST_EXPECT(!bytes.empty());

	score::File file;
	std::string error;
	TEST_EXPECT(score::parse(bytes.data(), bytes.size(), file, error));
	TEST_EXPECT(file.version == 40);
	TEST_EXPECT(file.exp_fanfare[0] == 0 && file.exp_fanfare[1] == 0 && !score::exp_fanfare_kept(file));
	TEST_EXPECT(file.blocks.size() == 3);

	// Block ORDER is the engine's row index [orig: ScoreConfig_LoadScoringTableForGameType
	// @ 0x52D300 — `score_type_index *= 452`].
	TEST_EXPECT(file.blocks[0].name == "COOP");
	TEST_EXPECT(file.blocks[1].name == "TDM");
	TEST_EXPECT(file.blocks[2].name == "COOP");
	// A GAMETYPE's row is found by its name, the first of the twelve [orig: sub_52D850 @ 0x52D850].
	TEST_EXPECT(score::game_type_row("coop") == 2 && score::game_type_row("TDM") == 1 && score::game_type_row("XYZ") == -1);

	const score::GameTypeBlock *coop = score::block_at(file, 2);
	TEST_EXPECT(coop != nullptr);
	if (coop != nullptr) {
		TEST_EXPECT(score::var_value(*coop, "ENEMYKILL", -1) == 5);
		TEST_EXPECT(score::var_value(*coop, "MEDICSAVE", -1) == 2);
		TEST_EXPECT(score::var_value(*coop, "enemykill", -1) == 5);
		TEST_EXPECT(score::var_value(*coop, "NOPE", -7) == -7);
		TEST_EXPECT(score::field_value(*coop, "NUMENEMYKILLS", -1) == 1);
	}
	// The tables [orig: the FIELD names @ 0x830240, the VAR names @ 0x830348].
	TEST_EXPECT(score::field_id("NUMSUICIDES") == 1 && score::field_id("NUMLFPTAKEOVERS") == 32);
	TEST_EXPECT(score::var_id("FIRE") == 0 && score::var_id("ENEMYKILL") == 3 && score::var_id("FRIENDLYKILL") == 2);
	TEST_EXPECT(score::var_id("ALIVEQUANTUM") == 36 && score::var_id("ZONEQUANTUM") == 12);

	// The witnessed ladder [orig: @0x52D300]. Objective Co-op is 0x30020.
	TEST_EXPECT(score::row_for_game_type(0x30020) == 2);
	TEST_EXPECT(score::row_for_game_type(0x10000) == 1);
	TEST_EXPECT(score::row_for_game_type(0) == 11);
	TEST_EXPECT(score::row_for_game_type(65537) == 3);
	TEST_EXPECT(score::row_for_game_type(327696) == 0xA);
	// Stock (non-objective) Co-op 0x10020 misses the objective-bit arm and falls
	// to the default, which the original remaps 0 -> 2.
	TEST_EXPECT(score::row_for_game_type(0x10020) == 2);
	// g_GameType == 8 selects 12, which the loader's own `<= 11` guard rejects.
	TEST_EXPECT(score::row_for_game_type(8) == -1);

	// The writer's own form: ScoreConfig_SaveFile's, reading back as the file, and a second write the same.
	std::vector<uint8_t> encoded;
	TEST_EXPECT(score::write(file, encoded, error));
	score::File reparsed;
	TEST_EXPECT(score::parse(encoded.data(), encoded.size(), reparsed, error));
	TEST_EXPECT(score::equal(file, reparsed));
	std::vector<uint8_t> encoded2;
	TEST_EXPECT(score::write(reparsed, encoded2, error));
	TEST_EXPECT(encoded == encoded2);
	const std::string form = text_of(encoded);
	TEST_EXPECT(form.rfind("//---------------------------------------------------\r\n// NovaLogic Score INI file\r\n", 0) == 0);
	TEST_EXPECT(form.find("\r\nVERSION 40\r\n\r\nEXP_FANFARE 0 0\r\n\r\n// FIELD \"NUMSUICIDES\"\r\n") != std::string::npos);
	TEST_EXPECT(form.find("\r\n\r\n\r\nGAMETYPE \"COOP\"\r\n\r\nFIELD \"NUMENEMYKILLS\" 1\r\n\r\nVAR \"ENEMYKILL\" 5\r\nVAR \"MEDICSAVE\" 2\r\n") !=
	            std::string::npos);

	// The fixture read with its layout and written again is itself; one value changed, one line.
	{
		textlayout::Notes notes;
		score::File noted;
		TEST_EXPECT(score::parse(bytes.data(), bytes.size(), noted, error, notes));
		std::vector<uint8_t> out;
		bool rewritten = true;
		TEST_EXPECT(score::write(noted, &notes, out, error, &rewritten) && !rewritten);
		TEST_EXPECT(out == bytes);
		noted.blocks[2].vars[0].value = 7;
		TEST_EXPECT(score::write(noted, &notes, out, error, &rewritten));
		TEST_EXPECT(!rewritten);
		TEST_EXPECT(lines_changed(text_of(bytes), text_of(out)) == 1);
		TEST_EXPECT(score::var_value(parsed(text_of(out)).blocks[2], "ENEMYKILL", -1) == 7);
		// A FIELD added: its line after the block's last FIELD, in the writer's form and the file's ending.
		noted.blocks[1].fields.push_back({"NUMDEATHS", 1, 0});
		TEST_EXPECT(score::write(noted, &notes, out, error, &rewritten));
		TEST_EXPECT(!rewritten);
		TEST_EXPECT(text_of(out).find("GAMETYPE \"TDM\"\n\nFIELD \"NUMENEMYKILLS\" 1\nFIELD \"NUMDEATHS\" 1\n") != std::string::npos);
		TEST_EXPECT(score::equal(parsed(text_of(out)), noted));
	}

	// What the reader reads nothing of is no error: a FIELD outside any GAMETYPE, a VAR of two tokens, a name
	// its table lacks, a line whose second character is '/'.
	{
		const score::File none = parsed("VERSION 40\nFIELD \"NUMDEATHS\" 1\nGAMETYPE \"A\"\nVAR \"FIRE\"\nVAR \"NOPE\" 3\n /VAR \"FIRE\" 2\n");
		TEST_EXPECT(none.blocks.size() == 1 && none.blocks[0].name == "A" && none.blocks[0].fields.empty() &&
		            none.blocks[0].vars.empty());
		// A VAR read twice keeps its last value; a FIELD of a byte keeps its byte; a fanfare of 1 2 is kept.
		const score::File twice = parsed("VERSION 40\r\nGAMETYPE \"DM\"\r\nVAR \"FIRE\" 1\r\nVAR \"fire\" 4\r\nFIELD \"NUMDEATHS\" 300\r\nEXP_FANFARE 1 2\r\n");
		TEST_EXPECT(twice.blocks[0].vars.size() == 1 && twice.blocks[0].vars[0].value == 4);
		TEST_EXPECT(twice.blocks[0].fields[0].value == 44 && score::exp_fanfare_kept(twice));
		score::File refused = twice;
		refused.blocks[0].fields.push_back({"NOPE", 1, 0});
		TEST_EXPECT(!score::write(refused, encoded, error) && error.find("NOPE") != std::string::npos);
		// A file of another version keeps it (the game reads it for nothing, and writes its own over it).
		score::File old = twice;
		old.version = 39;
		TEST_EXPECT(score::write(old, encoded, error) && parsed(text_of(encoded)).version == 39);
	}

	// The review's cases (#987). A File built in code is the game's version 40 [orig: ScoreConfig_SaveFile @
	// 0x52CE66]; a file read at another version reads to no block (the game reads none of it [orig: @ 0x52DA8A]),
	// and its noted write is the file; the line cut at 2047 characters, the byte after it stepped over [orig:
	// Text_ReadLine @ 0x52D110]; the fanfare stored only through its gate [orig: @ 0x52DC75..0x52DC9F].
	{
		score::File minted;
		score::GameTypeBlock dm;
		dm.name = "DM";
		dm.vars.push_back({"FIRE", 1, 0});
		minted.blocks.push_back(dm);
		TEST_EXPECT(score::write(minted, encoded, error) && text_of(encoded).find("\r\nVERSION 40\r\n") != std::string::npos &&
		            text_of(encoded).find("\r\nEXP_FANFARE 0 0\r\n") != std::string::npos);
		TEST_EXPECT(parsed(text_of(encoded)).blocks.size() == 1);

		const std::string v39 = "VERSION 39\r\nGAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\nEXP_FANFARE 1 2\r\n";
		textlayout::Notes notes;
		score::File old;
		TEST_EXPECT(score::parse(reinterpret_cast<const uint8_t *>(v39.data()), v39.size(), old, error, notes));
		TEST_EXPECT(old.version == 39 && old.blocks.empty() && !old.has_exp_fanfare);
		std::vector<uint8_t> out;
		bool rewritten = true;
		TEST_EXPECT(score::write(old, &notes, out, error, &rewritten) && !rewritten && text_of(out) == v39);
		TEST_EXPECT(parsed("GAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\n").version == 0 &&
		            parsed("GAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\n").blocks.empty());

		const std::string longer = "VERSION 40\r\nGAMETYPE \"DM\"\r\n" + std::string(2047, ' ') + "XVAR \"FIRE\" 5\r\n";
		const score::File cut = parsed(longer);
		TEST_EXPECT(cut.blocks.size() == 1 && cut.blocks[0].vars.size() == 1 && cut.blocks[0].vars[0].value == 5);
		textlayout::Notes long_notes;
		score::File long_file;
		TEST_EXPECT(score::parse(reinterpret_cast<const uint8_t *>(longer.data()), longer.size(), long_file, error, long_notes));
		TEST_EXPECT(score::write(long_file, &long_notes, out, error, &rewritten) && !rewritten && text_of(out) == longer);

		const score::File fanfare = parsed("VERSION 40\r\nEXP_FANFARE 1 2\r\nEXP_FANFARE 0 0\r\n");
		TEST_EXPECT(fanfare.has_exp_fanfare && fanfare.exp_fanfare[0] == 1 && fanfare.exp_fanfare[1] == 2 &&
		            score::exp_fanfare_kept(fanfare));
		const score::File unkept = parsed("VERSION 40\r\nEXP_FANFARE 2 1\r\n");
		TEST_EXPECT(!unkept.has_exp_fanfare && unkept.exp_fanfare[0] == 0 && unkept.exp_fanfare[1] == 0);
		std::printf("review cases: a minted File at 40, a version-39 file read to nothing and kept, the 2047 cut, "
		            "the fanfare's gate\n");
	}

	// --- retail: <OPENNOVA_JO_DIR>/score.ini, the score table the install
	// ships loose beside its archives (never inside a .pff, so never in an
	// extracted tree) ---
	const std::string install = retail::install();
	const std::string retail_ini =
			install.empty() ? std::string() : retail::join(install, "score.ini");
	if (retail_ini.empty() || !retail::file_exists(retail_ini)) {
		retail::skip_leg("OPENNOVA_JO_DIR/score.ini (the retail score table beside the archives)");
	} else {
		const std::vector<uint8_t> rbytes = test_io::read_file(retail_ini.c_str());
		TEST_EXPECT(!rbytes.empty());
			score::File rfile;
			TEST_EXPECT(score::parse(rbytes.data(), rbytes.size(), rfile, error));
			TEST_EXPECT(rfile.blocks.size() == 12);
			const score::GameTypeBlock *rcoop = score::block_at(rfile, 2);
			TEST_EXPECT(rcoop != nullptr && rcoop->name == "COOP");
		if (rcoop != nullptr) TEST_EXPECT(score::var_value(*rcoop, "ENEMYKILL", -1) == 5);
			// The 0 -> 2 remap is only unobservable because the shipped blocks
			// 0 and 2 agree; assert that rather than assume it.
			const score::GameTypeBlock *r0 = score::block_at(rfile, 0);
			if (r0 != nullptr && rcoop != nullptr) {
				score::File a, b;
				a.blocks.push_back(*r0);
				b.blocks.push_back(*rcoop);
				TEST_EXPECT(score::equal(a, b));
			}
		// The writer's own form is the shipped file, byte for byte: the game wrote it [orig: ScoreConfig_SaveFile @
		// 0x52CDD0].
			std::vector<uint8_t> renc;
			TEST_EXPECT(score::write(rfile, renc, error));
		TEST_EXPECT(renc == rbytes);
		textlayout::Notes notes;
		score::File noted;
		TEST_EXPECT(score::parse(rbytes.data(), rbytes.size(), noted, error, notes));
		bool rewritten = true;
		TEST_EXPECT(score::write(noted, &notes, renc, error, &rewritten) && !rewritten && renc == rbytes);
		noted.blocks[3].vars[0].value += 1;
		TEST_EXPECT(score::write(noted, &notes, renc, error, &rewritten) && !rewritten);
		TEST_EXPECT(lines_changed(text_of(rbytes), text_of(renc)) == 1);
		std::printf("retail: score.ini is the writer's own form byte for byte, and its noted write too; one VAR one line\n");
	}

	std::printf("score roundtrip tests passed\n");
	return 0;
}
