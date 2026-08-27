// engine/formats/score — score.ini parse, canonical write, and the witnessed
// row-index ladder.
//
// Committed fixture: fixtures/score/score_sample.ini, AUTHORED for this test
// (not retail data). It carries three GAMETYPE blocks in the shipped file's
// index order so the block-order-is-the-row-index rule and the index-0 -> 2
// remap are both exercised, with the retail Co-op row's witnessed ENEMYKILL 5 /
// MEDICSAVE 2.
//
// The retail sweep is env-gated the way tests/mission/mission_corpus_test.cpp
// gates its corpus: set OPENNOVA_SCORE_INI to a local score.ini from a licensed
// game installation and this test additionally asserts it
// parses, carries the 12 shipped blocks, and that its Co-op row 2 reads
// ENEMYKILL 5 -- the value the S2C 0x81 score mirror reproduces in the retail
// capture (5, 10, 20, ... 220). Unset, that half prints a skip line and passes.
#include <formats/score/score.h>

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace opennova;

std::vector<uint8_t> read_file(const std::string &path) {
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in.good()) return {};
	const std::streamsize size = in.tellg();
	in.seekg(0, std::ios::beg);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	in.read(reinterpret_cast<char *>(data.data()), size);
	if (!in.good()) return {};
	return data;
}

} // namespace

int main() {
	const std::string fixture =
			std::string(test_paths_repo_root(__FILE__)) + "/fixtures/score/score_sample.ini";
	const std::vector<uint8_t> bytes = read_file(fixture);
	TEST_EXPECT(!bytes.empty());

	score::File file;
	std::string error;
	TEST_EXPECT(score::parse(bytes.data(), bytes.size(), file, error));
	TEST_EXPECT(file.version == 40);
	TEST_EXPECT(file.exp_fanfare[0] == 0 && file.exp_fanfare[1] == 0);
	TEST_EXPECT(file.blocks.size() == 3);

	// Block ORDER is the engine's row index [orig: load_scoring_table_for_game_type
	// @ 0x52D300 — `score_type_index *= 452`].
	TEST_EXPECT(file.blocks[0].name == "COOP");
	TEST_EXPECT(file.blocks[1].name == "TDM");
	TEST_EXPECT(file.blocks[2].name == "COOP");

	const score::GameTypeBlock *coop = score::block_at(file, 2);
	TEST_EXPECT(coop != nullptr);
	if (coop != nullptr) {
		TEST_EXPECT(score::var_value(*coop, "ENEMYKILL", -1) == 5);
		TEST_EXPECT(score::var_value(*coop, "MEDICSAVE", -1) == 2);
		TEST_EXPECT(score::var_value(*coop, "enemykill", -1) == 5);
		TEST_EXPECT(score::var_value(*coop, "NOPE", -7) == -7);
		TEST_EXPECT(score::field_value(*coop, "NUMENEMYKILLS", -1) == 1);
	}

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

	// Canonical write: the fixture is authored in canonical form, so the round
	// trip is byte-exact. write() builds from scratch (ADR 0003) -- comments are
	// deliberately not preserved, which is why only canonical input matches.
	std::vector<uint8_t> encoded;
	TEST_EXPECT(score::write(file, encoded, error));
	score::File reparsed;
	TEST_EXPECT(score::parse(encoded.data(), encoded.size(), reparsed, error));
	TEST_EXPECT(score::equal(file, reparsed));
	std::vector<uint8_t> encoded2;
	TEST_EXPECT(score::write(reparsed, encoded2, error));
	TEST_EXPECT(encoded == encoded2);

	// Malformed input is rejected, never silently dropped.
	{
		score::File bad;
		const std::string src = "FIELD \"X\" 1\n";
		TEST_EXPECT(!score::parse(reinterpret_cast<const uint8_t *>(src.data()),
							  src.size(), bad, error));
		const std::string src2 = "GAMETYPE \"A\"\nVAR \"X\"\n";
		TEST_EXPECT(!score::parse(reinterpret_cast<const uint8_t *>(src2.data()),
							  src2.size(), bad, error));
	}

	// --- retail sweep, env-gated (skip-and-pass when unset) ---
	const char *retail = std::getenv("OPENNOVA_SCORE_INI");
	if (retail == nullptr || retail[0] == '\0') {
		std::fprintf(stderr,
				"score_roundtrip: retail sweep skipped (set OPENNOVA_SCORE_INI to a "
				"score.ini extracted from the game archives)\n");
	} else {
		const std::vector<uint8_t> rbytes = read_file(retail);
		TEST_EXPECT(!rbytes.empty());
		{
			score::File rfile;
			TEST_EXPECT(score::parse(rbytes.data(), rbytes.size(), rfile, error));
			TEST_EXPECT(rfile.blocks.size() == 12);
			const score::GameTypeBlock *rcoop = score::block_at(rfile, 2);
			TEST_EXPECT(rcoop != nullptr && rcoop->name == "COOP");
			if (rcoop != nullptr)
				TEST_EXPECT(score::var_value(*rcoop, "ENEMYKILL", -1) == 5);
			// The 0 -> 2 remap is only unobservable because the shipped blocks
			// 0 and 2 agree; assert that rather than assume it.
			const score::GameTypeBlock *r0 = score::block_at(rfile, 0);
			if (r0 != nullptr && rcoop != nullptr) {
				score::File a, b;
				a.blocks.push_back(*r0);
				b.blocks.push_back(*rcoop);
				TEST_EXPECT(score::equal(a, b));
			}
			std::vector<uint8_t> renc;
			score::File rre;
			TEST_EXPECT(score::write(rfile, renc, error));
			TEST_EXPECT(score::parse(renc.data(), renc.size(), rre, error));
			TEST_EXPECT(score::equal(rfile, rre));
		}
	}

	std::printf("score roundtrip tests passed\n");
	return 0;
}
