// Parse + byte-exact round-trip for opennova::dbf (a group's stale id-list bytes written zero), and
// the dlg-id -> def-id (LWF set) mapping the runtime uses. Mirrors lwf_roundtrip_test.
//
// The banks as the sound script compiler and the tools leave them (tests/fixtures/minimal_dbf_gen.cpp):
// fixtures/dbf/synth_bank.dbf, a bank of the shape the shipped 00TRg.DBF has (eleven dialogs, dlg001's
// first line its first wave), written back byte for byte as every shipped bank but one is; and
// fixtures/dbf/stale_bank.dbf, its lines' sequences "##" over older text as 04TR.DBF's are, written
// back with the bytes past each sequence's terminator zero (ADR 0003), every value the game reads the
// same. The writer's bytes of both are pinned (FNV-1a64): a change to a vector is a change to the bytes
// the writer writes, which needs an [orig] witness or a D-SND entry (`dbf_roundtrip_test --dump`
// prints the table).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/dbf/dbf.h>

#include "common/file_io.h"

namespace {

using test_io::read_file;

uint64_t fnv1a64(const std::vector<uint8_t> &bytes) {
	uint64_t h = 1469598103934665603ull;
	for (uint8_t b : bytes) {
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

struct Vector {
	const char *name;
	size_t len;
	uint64_t hash;
};

// The writer's bytes of each bank.
const Vector kExpected[] = {
	{"synth_bank.dbf", 1688u, 0x200e8a88e9c347d6ull},
	{"stale_bank.dbf", 712u, 0x1badbab665e30f39ull},
};

// One bank: header pins, group count, the first dialog's first wave name, case-insensitive lookup;
// written from its fields, the same size, reading back as the same values, a fixed point, and its
// bytes again where `stale` is false (else every byte but those past a line's sequence terminator).
// The writer's bytes pinned unless `dump`, which prints them.
int check_bank(const std::string &name, size_t groups, const char *first_set, bool stale, const Vector &pin,
               bool dump) {
	const std::vector<uint8_t> original = read_file(test_paths_repo_root(__FILE__) + std::string("/fixtures/dbf/") + name);
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
	TEST_EXPECT(g != nullptr && !g->lines.empty());
	TEST_EXPECT(g != nullptr && !g->lines.empty() && g->lines[0].def_id_name == first_set);

	// Case-insensitive lookup contract.
	TEST_EXPECT(opennova::dbf::find_group(file, "DLG001") != nullptr);
	TEST_EXPECT(opennova::dbf::find_group(file, "nope") == nullptr);

	std::vector<uint8_t> encoded;
	TEST_EXPECT(opennova::dbf::encode_dbf(file, encoded, error));
	if (dump) {
		std::printf("\t{\"%s\", %zuu, 0x%016llxull},\n", name.c_str(), encoded.size(),
		            (unsigned long long)fnv1a64(encoded));
		return 0;
	}
	TEST_EXPECT(encoded.size() == original.size());
	if (encoded.size() != original.size()) return 1;

	// The bytes past each line's sequence terminator, which no reader reaches [orig: Dialog_LoadAudioClip
	// @ 0x44ddec, strlen]: a stale bank's differ there and only there.
	std::vector<bool> past(original.size(), false);
	size_t at = 28;
	for (const opennova::dbf::Group &group : file.groups) {
		at += 52;
		for (const opennova::dbf::Line &line : group.lines) {
			for (size_t k = line.sequence.size() + 1; k < opennova::dbf::kNameBytes; ++k) past[at + 0x1C + k] = true;
			at += 68;
		}
	}
	size_t differ = 0, elsewhere = 0;
	for (size_t i = 0; i < original.size(); ++i) {
		if (encoded[i] == original[i]) continue;
		++differ;
		elsewhere += past[i] ? 0 : 1;
	}
	TEST_EXPECT(elsewhere == 0);
	TEST_EXPECT(stale ? differ > 0 : differ == 0);

	// Every value the game reads, the same; the write a fixed point.
	opennova::dbf::File again;
	TEST_EXPECT(opennova::dbf::parse_dbf_memory(encoded.data(), encoded.size(), again, error));
	bool same = again.groups.size() == file.groups.size();
	for (size_t i = 0; same && i < file.groups.size(); ++i) {
		const opennova::dbf::Group &a = file.groups[i], &b = again.groups[i];
		same = a.group_name == b.group_name && a.idlist_count == b.idlist_count && a.def_id_indices == b.def_id_indices &&
		       a.lines.size() == b.lines.size();
		for (size_t l = 0; same && l < a.lines.size(); ++l)
			same = a.lines[l].def_id_name == b.lines[l].def_id_name && a.lines[l].sequence == b.lines[l].sequence &&
			       a.lines[l].delay == b.lines[l].delay && a.lines[l].line_flags == b.lines[l].line_flags &&
			       a.lines[l].def_id_index == b.lines[l].def_id_index && a.lines[l].param == b.lines[l].param;
	}
	TEST_EXPECT(same);
	std::vector<uint8_t> twice;
	TEST_EXPECT(opennova::dbf::encode_dbf(again, twice, error) && twice == encoded);
	if (name != pin.name || encoded.size() != pin.len || fnv1a64(encoded) != pin.hash) {
		std::printf("FAIL: %s written: %zu bytes, hash 0x%016llx; pinned %s, %zu bytes, 0x%016llx\n", name.c_str(),
		            encoded.size(), (unsigned long long)fnv1a64(encoded), pin.name, pin.len,
		            (unsigned long long)pin.hash);
		return 1;
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
	const std::string root = test_paths_repo_root(__FILE__);
	if (dump) std::printf("const Vector kExpected[] = {\n");
	// 00TRg.DBF's shape: eleven dialogs, dlg001 its first line's wave.
	if (check_bank("synth_bank.dbf", 11, "SynR100", false, kExpected[0], dump) != 0) return 1;
	// 04TR.DBF's form: the sequences "##" over older text.
	if (check_bank("stale_bank.dbf", 4, "Z90R100", true, kExpected[1], dump) != 0) return 1;
	if (dump) {
		std::printf("};\n");
		return 0;
	}

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
		// A name as wide as its 24-byte field comes back as it went (no terminator).
		read.groups[0].group_name = std::string(24, 'G');
		TEST_EXPECT(opennova::dbf::encode_dbf(read, written, error));
		opennova::dbf::File back;
		TEST_EXPECT(opennova::dbf::parse_dbf_memory(written.data(), written.size(), back, error) &&
				!back.groups.empty() && back.groups[0].group_name == std::string(24, 'G'));
	}
	std::printf("dbf_roundtrip: the writer's bank and the stale bank through the writer\n");
	return 0;
}
