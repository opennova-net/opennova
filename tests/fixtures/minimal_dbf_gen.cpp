// Generator + guard for fixtures/dbf/synth_bank.dbf: a synthetic mission
// dialog bank written by encode_dbf from integer data — eleven dialog groups
// (dlg001..dlg011) of one to three lines each, the shape the dialog runtime
// resolves (a dialog id -> the LWF set names of its lines, with per-line
// delays and flags) [orig: DialogManager_LoadFromFile @ 0x44e650: the 28-byte
// header, 52-byte group records each followed by its 68-byte line records].
// And fixtures/dbf/stale_bank.dbf: a bank the writer writes, then left as the
// tools left 04TR.DBF, its lines' sequences "##" over the older "_0000N" each
// once held (the older text's tail past the new terminator, which no reader
// reaches [orig: Dialog_LoadAudioClip @ 0x44ddec, strlen]): the one form of
// stale bytes JO:CA's seventeen banks hold, every other shipped bank its
// writer's bytes. No retail bank is carried.
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
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

// The stale bank: four dialogs of one to three lines, the subtitled lines' sequences "_0000N",
// the others "##"; its seven lines' waves Z90R100..Z90R106, the singles fixtures/lwf/stale.lwf
// holds (a mission's dialog bank and its sounds, as 04TR.DBF and 04TR.LWF are).
opennova::dbf::File make_stale_bank() {
	opennova::dbf::File f;
	f.header.magic = opennova::dbf::kMagic;
	f.header.version = 0x100;
	f.header.header_size = 28;
	const char *const names[] = {"dlg001", "dlg002", "dlg003", "dlg004"};
	uint32_t n = 0, wave_number = 100;
	for (const char *name : names) {
		opennova::dbf::Group group;
		group.group_name = name;
		for (uint32_t k = 0; k < 1 + n % 3; ++k) {
			const std::string wave = "Z90R" + std::to_string(wave_number++);
			opennova::dbf::Line l = line(wave.c_str(), uint8_t(5 * k));
			l.sequence = (n + k) % 2 == 0 ? "##" : "_0000" + std::to_string(n + k);
			group.lines.push_back(l);
		}
		f.groups.push_back(group);
		++n;
	}
	return f;
}

// Each "##" over the older sequence its line once held, "_0000N" for its place N: past the new
// terminator, that text's fourth character on ("00N"), as 04TR.DBF holds it.
std::vector<uint8_t> with_stale_sequences(const opennova::dbf::File &f, std::vector<uint8_t> bytes) {
	size_t at = 28;
	uint32_t place = 1;
	for (const opennova::dbf::Group &group : f.groups) {
		at += 52;
		for (const opennova::dbf::Line &l : group.lines) {
			if (l.sequence == "##") {
				const std::string older = "_0000" + std::to_string(place % 10);
				std::memcpy(bytes.data() + at + 0x1C + 3, older.data() + 3, older.size() - 3);
			}
			at += 68;
			++place;
		}
	}
	return bytes;
}

using test_io::read_file;

bool guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return true;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return false;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return true;
	}
	return expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str());
}

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

	// The stale bank reads as its bank and writes back as the writer's bytes.
	const opennova::dbf::File stale_bank = make_stale_bank();
	std::vector<uint8_t> stale_clean;
	if (!expect(opennova::dbf::encode_dbf(stale_bank, stale_clean, err), ("encode_dbf stale: " + err).c_str()))
		return 1;
	const std::vector<uint8_t> stale = with_stale_sequences(stale_bank, stale_clean);
	opennova::dbf::File stale_read;
	std::vector<uint8_t> stale_written;
	if (!expect(stale != stale_clean && opennova::dbf::parse_dbf_memory(stale.data(), stale.size(), stale_read, err) &&
	                    opennova::dbf::encode_dbf(stale_read, stale_written, err) && stale_written == stale_clean,
	            "stale_bank.dbf reads as its bank and writes back as the writer's bytes"))
		return 1;

	const std::string stale_path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/dbf/stale_bank.dbf";
	if (!guard(path, bytes, write_mode) || !guard(stale_path, stale, write_mode)) return 1;
	if (write_mode) return 0;
	std::printf("OK: fixtures/dbf/synth_bank.dbf + stale_bank.dbf byte-reproducible\n");
	return 0;
}
