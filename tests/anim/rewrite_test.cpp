// Every committed animation file through the engine's reader and writer, the
// path the editor saves them by (ADR 0046 S10):
//   .bad: parse -> write -> parse is field-equal to the file (common/bad_equal.h:
//         every field the reader keeps; the bone table's stale child/parent
//         addresses and the translation pad row are the writer's own), and the
//         write is a fixed point (writing the re-read clip gives the same bytes).
//   .adm: the canonical write reads back as the file's rows, and is a fixed point.
// The files are fixtures/anim: our one-bone clips and tables, the minted
// person and first-person sets, and one clip per form the shipped clips take
// that a set does not (tests/fixtures/minimal_bad_gen.cpp: the short, sparse
// and long channels, a version 0 clip, the exporter's leftovers in the bone
// rows, a translated clip with no pad row, an X-triplicated rig), which is
// every form the 477 shipped clips held when this row read them (2026-10-19).
// The write of each is pinned by its FNV-1a64 below: a vector that moves is a
// change to the bytes the editor saves, and needs an [orig] witness or a
// D-entry in the same commit (`anim_rewrite_test --dump` prints the table).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_write.h>

#include "common/bad_equal.h"
#include "common/file_io.h"
#include "common/test_paths.h"

using namespace opennova;

namespace {

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

// The write of every fixture, in name order.
const Vector kExpected[] = {
		{"US01.adm", 118u, 0x18cf7b7d5f386bf3ull},
		{"idle.bad", 552u, 0xce2eca6bbecc88cdull},
		{"rig19.adm", 377u, 0x8f0dc1b9a749a337ull},
		{"rig19_crch.bad", 3344u, 0xf13beeff754681b7ull},
		{"rig19_dthb.bad", 13592u, 0x2a188ef23b29a36eull},
		{"rig19_dthf.bad", 13592u, 0xd36ce266768305eeull},
		{"rig19_dthl.bad", 13592u, 0x69966147b8a2a52eull},
		{"rig19_dthr.bad", 13592u, 0x4b58d15ac2ebd1aeull},
		{"rig19_idle.bad", 3672u, 0xb003cbab07dfabedull},
		{"rig19_junk.bad", 3672u, 0x922016a099677225ull},
		{"rig19_rld1.bad", 9932u, 0x7a247869ce289239ull},
		{"rig19_rld2.bad", 9932u, 0xf8cf056d2a443079ull},
		{"rig19_rst.bad", 3344u, 0x879e44792a407907ull},
		{"rig19_short.bad", 2964u, 0x969dc0552e988772ull},
		{"rig19_sparse.bad", 4800u, 0x915e727be10f6145ull},
		{"rig19_twist.bad", 3672u, 0x81d1f36d7477fd91ull},
		{"rig19_v0.bad", 3332u, 0x1287915a43f691e0ull},
		{"rig19_walk.bad", 13592u, 0x8bec0f99729b75f9ull},
		{"soldier.adm", 207u, 0x04b6f61fef708d81ull},
		{"vm40.adm", 276u, 0x1188df81b1b43874ull},
		{"vm40_brk.bad", 7488u, 0x8cb97075db1d6c30ull},
		{"vm40_e.bad", 8792u, 0x41244cdb7aef8323ull},
		{"vm40_f.bad", 11240u, 0xff3fefead84b5094ull},
		{"vm40_i.bad", 16136u, 0xaae57333a5b0b52bull},
		{"vm40_long.bad", 14120u, 0x8c156a6f10a8c155ull},
		{"vm40_nopad.bad", 7488u, 0x343e14f671cc5944ull},
		{"vm40_r.bad", 43064u, 0xbcc356701fc4a0a3ull},
		{"vm40_rst.bad", 7488u, 0x343e14f671cc5944ull},
		{"vm40_swf.bad", 18584u, 0x14cf17174e5e9621ull},
		{"vm40_swr.bad", 13688u, 0x2b6f2bc706630024ull},
		{"vm40_swt.bad", 18584u, 0x0e5e4a488fde8c89ull},
		{"walk.bad", 552u, 0x92f2917f19b1b72bull},
};

std::string check_clip(const std::vector<uint8_t> &original, std::vector<uint8_t> &first) {
	bad::BadFile parsed = {}, reread = {};
	std::string problem;
	std::vector<uint8_t> second;
	if (bad::bad_parse_buffer(original.data(), original.size(), &parsed) != 0) problem = "does not parse";
	else if (bad::bad_write_buffer(&parsed, first) != 0) problem = "does not write";
	else if (bad::bad_parse_buffer(first.data(), first.size(), &reread) != 0) problem = "its rewrite does not parse";
	else if (!bad_equal::field_equal(parsed, reread)) problem = "its rewrite reads back different fields";
	else if (bad::bad_write_buffer(&reread, second) != 0 || second != first) problem = "the write is not a fixed point";
	bad::bad_free(&parsed);
	bad::bad_free(&reread);
	return problem;
}

bool same_rows(const adm::AdmFile &a, const adm::AdmFile &b) {
	if (a.count != b.count) return false;
	for (size_t i = 0; i < a.count; ++i) {
		const adm::AdmEntry &x = a.entries[i];
		const adm::AdmEntry &y = b.entries[i];
		if (std::strcmp(x.key, y.key) != 0 || x.variants.size() != y.variants.size()) return false;
		for (size_t v = 0; v < x.variants.size(); ++v)
			if (std::strcmp(x.variants[v], y.variants[v]) != 0) return false;
	}
	return true;
}

std::string check_table(const std::vector<uint8_t> &original, std::vector<uint8_t> &out) {
	adm::AdmFile parsed = {}, reread = {};
	std::string problem, first, second;
	if (adm::adm_parse_buffer(reinterpret_cast<const char *>(original.data()), original.size(), &parsed) != 0)
		problem = "does not parse";
	else if (adm::adm_write_buffer(&parsed, first) != 0) problem = "does not write";
	else if (adm::adm_parse_buffer(first.data(), first.size(), &reread) != 0) problem = "its rewrite does not parse";
	else if (!same_rows(parsed, reread)) problem = "its rewrite reads back different rows";
	else if (adm::adm_write_buffer(&reread, second) != 0 || second != first) problem = "the write is not a fixed point";
	adm::adm_free(&parsed);
	adm::adm_free(&reread);
	out.assign(first.begin(), first.end());
	return problem;
}

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

} // namespace

int main(int argc, char **argv) {
	const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
	const std::string root = test_paths_fixture("anim");
	std::vector<std::string> names;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::string extension = lower(entry.path().extension().string());
		if (extension == ".bad" || extension == ".adm") names.push_back(entry.path().filename().string());
	}
	std::sort(names.begin(), names.end());

	size_t clips = 0, tables = 0;
	int failures = 0;
	std::vector<Vector> written;
	for (const std::string &name : names) {
		const std::vector<uint8_t> bytes = test_io::read_file(root + "/" + name);
		if (test_io::is_lfs_pointer(bytes)) {
			std::printf("FAIL %s: an unpulled LFS pointer\n", name.c_str());
			++failures;
			continue;
		}
		const bool clip = lower(name).size() > 4 && lower(name).compare(name.size() - 4, 4, ".bad") == 0;
		std::vector<uint8_t> out;
		const std::string problem = clip ? check_clip(bytes, out) : check_table(bytes, out);
		++(clip ? clips : tables);
		if (!problem.empty()) {
			std::printf("FAIL %s: %s\n", name.c_str(), problem.c_str());
			++failures;
		}
		written.push_back(Vector{nullptr, out.size(), fnv1a64(out)});
	}
	if (dump) {
		std::printf("const Vector kExpected[] = {\n");
		for (size_t i = 0; i < names.size(); ++i)
			std::printf("\t\t{\"%s\", %zuu, 0x%016llxull},\n", names[i].c_str(), written[i].len,
					static_cast<unsigned long long>(written[i].hash));
		std::printf("};\n");
		return 0;
	}
	const size_t expected = sizeof(kExpected) / sizeof(kExpected[0]);
	if (expected != names.size()) {
		std::printf("FAIL: fixtures/anim holds %zu clips and tables, %zu vectors are committed (--dump)\n", names.size(),
				expected);
		++failures;
	}
	for (size_t i = 0; i < names.size() && i < expected; ++i) {
		if (names[i] != kExpected[i].name || written[i].len != kExpected[i].len || written[i].hash != kExpected[i].hash) {
			std::printf("FAIL %s: writes %zu bytes hash 0x%016llx, pinned %s %zu bytes 0x%016llx\n", names[i].c_str(),
					written[i].len, static_cast<unsigned long long>(written[i].hash), kExpected[i].name, kExpected[i].len,
					static_cast<unsigned long long>(kExpected[i].hash));
			++failures;
		}
	}
	std::printf("%zu clips, %zu tables, %d failed\n", clips, tables, failures);
	return failures == 0 ? 0 : 1;
}
