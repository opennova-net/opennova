// Generator + guard for fixtures/dbf/synth_bank.dbf: a synthetic mission
// dialog bank written by encode_dbf from integer data — eleven dialog groups
// (dlg001..dlg011) of one to three lines each, the shape the dialog runtime
// resolves (a dialog id -> the LWF set names of its lines, with per-line
// delays and flags) [orig: DialogManager_LoadFromFile @ 0x44e650: the 28-byte
// header, 52-byte group records each followed by its 68-byte line records].
// No retail bank is carried: the shipped 00TRg.DBF is the reference-tree leg
// of dbf_roundtrip.
//
// Default: rebuild in memory and byte-compare the committed file. `--write`
// (re)writes it.
#include "common/test_paths.h"

#include <formats/dbf/dbf.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

opennova::dbf::Line line(const char *def_id_name, uint8_t delay, uint32_t flags = 0) {
	opennova::dbf::Line l;
	l.def_id_name = def_id_name;
	l.sequence = "_00000";
	l.line_flags = flags;
	l.def_id_index = 0;
	l.delay = delay;
	return l;
}

// The bank: dlg001 is the one-line dialog the runtime tests resolve first
// (dlg001 -> SynR100); the rest vary the line count, delays and flags so
// every record field moves.
opennova::dbf::File make_bank() {
	opennova::dbf::File f;
	f.header.magic = opennova::dbf::kMagic;
	f.header.version = 0x100;
	f.header.header_size = 28;
	const struct {
		const char *name;
		std::vector<opennova::dbf::Line> lines;
	} groups[] = {
	    {"dlg001", {line("SynR100", 0)}},
	    {"dlg002", {line("SynR101", 5), line("SynR102", 12)}},
	    {"dlg003", {line("SynR103", 0, 1)}},
	    {"dlg004", {line("SynR104", 30), line("SynR105", 0), line("SynR106", 8)}},
	    {"dlg005", {line("SynR107", 2)}},
	    {"dlg006", {line("SynR108", 0)}},
	    {"dlg007", {line("SynR109", 15, 2)}},
	    {"dlg008", {line("SynR110", 0), line("SynR111", 40)}},
	    {"dlg009", {line("SynR112", 0)}},
	    {"dlg010", {line("SynR113", 7)}},
	    {"dlg011", {line("SynR114", 0), line("SynR115", 3)}},
	};
	for (const auto &g : groups) {
		opennova::dbf::Group group;
		group.group_name = g.name;
		group.lines = g.lines;
		group.idlist_count = 0;
		f.groups.push_back(group);
	}
	return f;
}

using test_io::read_file;

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/dbf/synth_bank.dbf";

	std::vector<uint8_t> bytes;
	std::string err;
	if (!expect(opennova::dbf::encode_dbf(make_bank(), bytes, err), ("encode_dbf: " + err).c_str())) return 1;
	// The writer's output is its own fixed point and keeps the pinned shape.
	opennova::dbf::File back;
	if (!expect(opennova::dbf::parse_dbf_memory(bytes.data(), bytes.size(), back, err),
	            ("parse_dbf_memory: " + err).c_str()))
		return 1;
	std::vector<uint8_t> again;
	if (!expect(opennova::dbf::encode_dbf(back, again, err) && again == bytes,
	            "encode(parse(encode(bank))) is not byte-stable"))
		return 1;
	const opennova::dbf::Group *first = opennova::dbf::find_group(back, "dlg001");
	if (!expect(back.groups.size() == 11 && first != nullptr && !first->lines.empty() &&
	                first->lines[0].def_id_name == "SynR100",
	            "the re-parsed bank lost a pinned field"))
		return 1;

	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str())) return 1;
	static const char kLfsSentinel[] = "version https://git-lfs";
	if (committed.size() >= sizeof(kLfsSentinel) - 1 &&
	    std::memcmp(committed.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()))
		return 1;
	std::printf("OK: fixtures/dbf/synth_bank.dbf byte-reproducible\n");
	return 0;
}
