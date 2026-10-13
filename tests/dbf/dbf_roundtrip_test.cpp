// Parse + byte-exact round-trip for opennova::dbf (a group's stale id-list bytes written zero), and
// the dlg-id -> def-id (LWF set) mapping the runtime uses. Mirrors lwf_roundtrip_test.
//
// Two legs: the minted fixtures/dbf/synth_bank.dbf (tests/fixtures/
// minimal_dbf_gen.cpp) runs unconditionally; the shipped 00TRg.DBF, read from
// the reference fixture set behind OPENNOVA_JO_ASSETS, proves the parser reads
// (and the encoder reproduces byte for byte) what the retail compiler wrote.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/dbf/dbf.h>

#include "common/file_io.h"

namespace {

using test_io::read_file;

// One bank: header pins, group count, the first dialog's first set name,
// case-insensitive lookup, byte-exact re-encode.
int check_bank(const std::string &path, size_t groups, const char *first_set) {
	const std::vector<uint8_t> original = read_file(path);
	TEST_EXPECT(!original.empty());

	opennova::dbf::File file;
	std::string error;
	TEST_EXPECT(opennova::dbf::parse_dbf_memory(original.data(), original.size(), file, error));
	TEST_EXPECT(file.header.magic == opennova::dbf::kMagic);
	TEST_EXPECT(file.header.header_size == 28);
	TEST_EXPECT(file.groups.size() == groups);

	// dlg001 -> its first line's set name (the dialog id -> LWF set-name mapping).
	const opennova::dbf::Group *g = opennova::dbf::find_group(file, "dlg001");
	TEST_EXPECT(g != nullptr);
	TEST_EXPECT(!g->lines.empty());
	TEST_EXPECT(g->lines[0].def_id_name == first_set);

	// Case-insensitive lookup contract.
	TEST_EXPECT(opennova::dbf::find_group(file, "DLG001") != nullptr);
	TEST_EXPECT(opennova::dbf::find_group(file, "nope") == nullptr);

	// Byte-exact round-trip: all parsed fields are preserved on encode.
	std::vector<uint8_t> encoded;
	TEST_EXPECT(opennova::dbf::encode_dbf(file, encoded, error));
	TEST_EXPECT(encoded.size() == original.size());
	TEST_EXPECT(std::memcmp(encoded.data(), original.data(), original.size()) == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
	const std::string root = test_paths_repo_root(__FILE__);
	if (check_bank(root + "/fixtures/dbf/synth_bank.dbf", 11, "SynR100") != 0) return 1;

	// A group's id list is its count's entries: the bytes past them, the tools' stale memory in a
	// shipped bank (04TR.DBF), are written zero (ADR 0003), the list itself as read.
	{
		std::vector<uint8_t> stale = read_file(root + "/fixtures/dbf/synth_bank.dbf");
		TEST_EXPECT(stale.size() > 28 + 52);
		if (stale.size() <= 28 + 52)
			return 1;
		const uint32_t count = 2;
		std::memcpy(stale.data() + 28 + 0x20, &count, 4);
		const uint8_t ids[16] = { 7, 9, 0xAB, 0xCD, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
		std::memcpy(stale.data() + 28 + 0x24, ids, sizeof(ids));
		opennova::dbf::File read;
		std::string error;
		TEST_EXPECT(opennova::dbf::parse_dbf_memory(stale.data(), stale.size(), read, error));
		TEST_EXPECT(!read.groups.empty() && read.groups[0].idlist_count == 2 &&
				read.groups[0].def_id_indices == std::vector<uint8_t>({ 7, 9 }));
		std::vector<uint8_t> written;
		TEST_EXPECT(
				opennova::dbf::encode_dbf(read, written, error) && written.size() == stale.size());
		if (written.size() == stale.size()) {
			TEST_EXPECT(written[28 + 0x24] == 7 && written[28 + 0x25] == 9);
			for (size_t i = 2; i < 16; ++i)
				TEST_EXPECT(written[28 + 0x24 + i] == 0);
		}
	}

	// The retail leg: 00TRg's bank (11 dialogs; dlg001 -> Z00gR100).
	const std::string retail_bank = retail::reference_fixture("dbf/00TRg.DBF");
	if (retail_bank.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/dbf/00TRg.DBF (the shipped dialog bank)");
	if (check_bank(retail_bank, 11, "Z00gR100") != 0) return 1;
	std::printf("retail leg: 00TRg.DBF parsed and re-encoded byte for byte\n");
	return 0;
}
