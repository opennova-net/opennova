// engine/formats/score — score.ini read as ScoreConfig_LoadFile reads it, written as ScoreConfig_SaveFile
// writes it from the model alone, and the witnessed row-index ladder.
//
// Committed fixture: fixtures/score/score_sample.ini, AUTHORED for this test
// (not retail data). It carries three GAMETYPE blocks in the shipped file's
// index order so the block-order-is-the-row-index rule and the index-0 -> 2
// remap are both exercised, with the retail Co-op row's witnessed ENEMYKILL 5 /
// MEDICSAVE 2. Written from its model and read again it is the same model; a
// value changed reads back changed.
//
// The reader's lines: a line whose first or second character is '/' is read for
// nothing, every other line's tokens split at white space with quotes dropped,
// FIELD / VAR / EXP_FANFARE taking three tokens and a name of their tables, so a
// line the game reads nothing of is never an error. A minted file (no source
// text) is ScoreConfig_SaveFile's form byte for byte, and reads back as itself.
//
// The retail leg (--retail) reads <OPENNOVA_JO_DIR>/score.ini: its 12 blocks, its
// Co-op row 2's ENEMYKILL 5 (the value the S2C 0x81 score mirror reproduces in the
// retail capture: 5, 10, 20, ... 220), and the writer's own form of it IS the
// shipped file byte for byte (the game wrote it with these defaults), and reads
// back as the same model.
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

// The model written and read again.
score::File round_trip(const score::File &file) {
	std::vector<uint8_t> out;
	std::string error;
	if (!score::write(file, out, error)) return score::File{};
	return parsed(text_of(out));
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

	// The model written and read again is the same model: one value changed, a FIELD added.
	{
		score::File edited = file;
		edited.blocks[2].vars[0].value = 7;
		TEST_EXPECT(score::equal(round_trip(edited), edited) && score::var_value(round_trip(edited).blocks[2], "ENEMYKILL", -1) == 7);
		edited.blocks[1].fields.push_back({"NUMDEATHS", 1});
		TEST_EXPECT(score::equal(round_trip(edited), edited) && round_trip(edited).blocks[1].fields.size() == 2);
	}

	// What the reader reads nothing of is no error: a FIELD outside any GAMETYPE, a VAR of two tokens, a name
	// its table lacks, a line whose second character is '/'.
	{
		const score::File none = parsed("VERSION 40\nFIELD \"NUMDEATHS\" 1\nGAMETYPE \"A\"\nVAR \"FIRE\"\nVAR \"NOPE\" 3\n /VAR \"FIRE\" 2\n");
		TEST_EXPECT(none.blocks.size() == 1 && none.blocks[0].name == "A" && none.blocks[0].fields.empty() &&
		            none.blocks[0].vars.empty());
		// A VAR read twice keeps its last value; a FIELD of a byte keeps its byte; a fanfare of 1 2 is stored (a
		// block's line).
		const score::File twice = parsed("VERSION 40\r\nGAMETYPE \"DM\"\r\nVAR \"FIRE\" 1\r\nVAR \"fire\" 4\r\nFIELD \"NUMDEATHS\" 300\r\nEXP_FANFARE 1 2\r\n");
		TEST_EXPECT(twice.blocks[0].vars.size() == 1 && twice.blocks[0].vars[0].value == 4);
		TEST_EXPECT(twice.blocks[0].fields[0].value == 44 && twice.exp_fanfare[0] == 1 && twice.exp_fanfare[1] == 2);
		TEST_EXPECT(score::equal(round_trip(twice), twice));
		score::File refused = twice;
		refused.blocks[0].fields.push_back({"NOPE", 1});
		TEST_EXPECT(!score::write(refused, encoded, error) && error.find("NOPE") != std::string::npos);
		// A file of another version keeps it (the game reads it for nothing, and writes its own over it).
		score::File old = twice;
		old.version = 39;
		TEST_EXPECT(score::write(old, encoded, error) && parsed(text_of(encoded)).version == 39);
	}

	// The review's cases (#987). A File built in code is the game's version 40 [orig: ScoreConfig_SaveFile @
	// 0x52CE66]; a file read at another version reads to no block (the game reads none of it [orig: @ 0x52DA8A]),
	// and its write reads back so; the line cut at 2047 characters, the byte after it stepped over [orig:
	// Text_ReadLine @ 0x52D110]; the fanfare stored only through its gate [orig: @ 0x52DC75..0x52DC9F].
	{
		score::File minted;
		score::GameTypeBlock dm;
		dm.name = "DM";
		dm.vars.push_back({"FIRE", 1});
		minted.blocks.push_back(dm);
		TEST_EXPECT(score::write(minted, encoded, error) && text_of(encoded).find("\r\nVERSION 40\r\n") != std::string::npos &&
		            text_of(encoded).find("\r\nEXP_FANFARE 0 0\r\n") != std::string::npos);
		TEST_EXPECT(parsed(text_of(encoded)).blocks.size() == 1 && score::equal(parsed(text_of(encoded)), minted));

		const std::string v39 = "VERSION 39\r\nGAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\nEXP_FANFARE 1 2\r\n";
		const score::File old = parsed(v39);
		TEST_EXPECT(old.version == 39 && old.blocks.empty() && old.exp_fanfare[0] == 0 && old.exp_fanfare[1] == 0);
		TEST_EXPECT(score::equal(round_trip(old), old));
		TEST_EXPECT(parsed("GAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\n").version == 0 &&
		            parsed("GAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\n").blocks.empty());

		const std::string longer = "VERSION 40\r\nGAMETYPE \"DM\"\r\n" + std::string(2047, ' ') + "XVAR \"FIRE\" 5\r\n";
		const score::File cut = parsed(longer);
		TEST_EXPECT(cut.blocks.size() == 1 && cut.blocks[0].vars.size() == 1 && cut.blocks[0].vars[0].value == 5);
		TEST_EXPECT(score::equal(round_trip(cut), cut));

		// The game stores the last pair past the gate, an earlier line's where a later one fails it, a block's line's
		// after the lines before it; the defaults' 0 0 where none passes. Written, the stored pair reads back.
		const score::File fanfare = parsed("VERSION 40\r\nEXP_FANFARE 1 2\r\nEXP_FANFARE 0 0\r\n");
		TEST_EXPECT(fanfare.exp_fanfare[0] == 1 && fanfare.exp_fanfare[1] == 2 && score::exp_fanfare_kept(fanfare));
		TEST_EXPECT(score::equal(round_trip(fanfare), fanfare));
		const score::File unkept = parsed("VERSION 40\r\nEXP_FANFARE 2 1\r\n");
		TEST_EXPECT(unkept.exp_fanfare[0] == 0 && unkept.exp_fanfare[1] == 0 && !score::exp_fanfare_kept(unkept));
		TEST_EXPECT(score::equal(round_trip(unkept), unkept));
		const score::File after = parsed("VERSION 40\r\nEXP_FANFARE 1 2\r\nGAMETYPE \"DM\"\r\nEXP_FANFARE 3 4\r\n");
		TEST_EXPECT(after.exp_fanfare[0] == 3 && after.exp_fanfare[1] == 4 && score::equal(round_trip(after), after));
		std::printf("review cases: a minted File at 40, a version-39 file read to nothing and kept, the 2047 cut, "
		            "the fanfare's gate, each written and read back\n");
	}

	// The #997 review's case: a pair set is the writer's one EXP_FANFARE line, and reads back.
	{
		score::File set = parsed("VERSION 40\r\n\r\nEXP_FANFARE 0 0\r\n\r\nGAMETYPE \"DM\"\r\n\r\nVAR \"FIRE\" 5\r\n");
		set.exp_fanfare[0] = 2;
		set.exp_fanfare[1] = 5;
		std::vector<uint8_t> out;
		TEST_EXPECT(score::write(set, out, error));
		const std::string text = text_of(out);
		TEST_EXPECT(text.find("\r\nEXP_FANFARE 2 5\r\n") != std::string::npos && text.find("EXP_FANFARE") == text.rfind("EXP_FANFARE"));
		TEST_EXPECT(score::equal(parsed(text), set));
		std::printf("fanfare: a pair set is the one EXP_FANFARE line, read back\n");
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
		// The game's reader takes the written file as the shipped one: the same fields.
		TEST_EXPECT(score::equal(parsed(text_of(renc)), rfile));
		score::File edited = rfile;
		edited.blocks[3].vars[0].value += 1;
		TEST_EXPECT(score::equal(round_trip(edited), edited));
		std::printf("retail: score.ini is the writer's own form byte for byte, read back as the same fields; an edit "
		            "reads back\n");
	}

	std::printf("score roundtrip tests passed\n");
	return 0;
}
