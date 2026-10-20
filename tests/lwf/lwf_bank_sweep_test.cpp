// The sound-profile banks as the authoring tools leave them, swept as every shipped .lwf / .pwf once
// was: the minted fixtures/lwf/menu.lwf (our writer's bytes) and fixtures/lwf/stale.lwf (a bank whose
// bytes hold the tools' stale memory in the four places JO:CA's banks hold it, 18 of its 22:
// tests/fixtures/minimal_lwf_gen.cpp). Each parses, is written from its fields and read back as the same
// bank, our writer's bytes a fixed point, and carries the structural facts the reader's contract states:
// 'LWF1' magic, single_count == the singles table, at least one set/layer/member, every member's single
// index in range. Its write differs from its bytes in the stale bytes alone (ADR 0003): the stale bank's
// in each of the four places, the writer's own in none. The writer's bytes of each are pinned
// (FNV-1a64): a change to a vector is a change to the bytes the writer writes, which needs an [orig]
// witness or a D-SND entry (`lwf_bank_sweep_test --dump` prints the table).

#include <formats/lwf/lwf.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/test_paths.h"

namespace {

using opennova::lwf::File;

// The bank as its fields hold it: every record's fields but the offsets the writer computes.
bool same_bank(const File &a, const File &b) {
	if (a.header.magic != b.header.magic || a.trigger_count != b.trigger_count ||
			a.singles.size() != b.singles.size() || a.multis.size() != b.multis.size() ||
			a.playlists.size() != b.playlists.size() || a.sndparms.size() != b.sndparms.size())
		return false;
	for (size_t i = 0; i < a.singles.size(); ++i) {
		const auto &x = a.singles[i], &y = b.singles[i];
		if (x.name != y.name || x.volume != y.volume || x.path != y.path ||
				x.entry_word28 != y.entry_word28)
			return false;
	}
	for (size_t i = 0; i < a.multis.size(); ++i) {
		const auto &x = a.multis[i], &y = b.multis[i];
		if (x.name != y.name || x.pitch_base != y.pitch_base ||
				x.pitch_random_range != y.pitch_random_range ||
				x.playlist_indices != y.playlist_indices || x.target_id != y.target_id ||
				x.set_flags != y.set_flags)
			return false;
	}
	for (size_t i = 0; i < a.playlists.size(); ++i) {
		const auto &x = a.playlists[i], &y = b.playlists[i];
		if (x.falloff_radius != y.falloff_radius || x.min_distance != y.min_distance ||
				x.flags != y.flags || x.sndparm_indices != y.sndparm_indices ||
				x.anchor != y.anchor)
			return false;
	}
	for (size_t i = 0; i < a.sndparms.size(); ++i) {
		const auto &x = a.sndparms[i], &y = b.sndparms[i];
		if (x.single_index != y.single_index || x.pitch_scaled != y.pitch_scaled ||
				x.random_pitch_scaled != y.random_pitch_scaled || x.volume != y.volume ||
				x.clamp_volume != y.clamp_volume)
			return false;
	}
	return true;
}

// The bank's bytes with what the writer writes zero or computes as it writes them: a name's or a
// path's bytes past its terminator (in a bank not of the 'LWF1' magic, a name of 28 characters or
// fewer keeps the dword at +28), the header's +24 and the multi header's +12 and +16, a single's
// runtime words (+34..+47) and its +48 path pointer (its pool slot; zero in a bank of another
// magic), the trigger records, a set's and a layer's offset slots past their count, a layer's +12
// but the anchor where flag 0x100 is set, a member's two trailing words, and in a bank of another
// magic the whole pool. Every other byte the writer must write as the file holds it: the layer's
// word at +2 the engine's sequential cursor reads, a set's entry size, a zero slot inside a count,
// pool bytes past the singles' slots.
std::vector<uint8_t> stale_masked(const std::vector<uint8_t> &bytes, const File &file) {
	std::vector<uint8_t> m = bytes;
	const auto u32 = [&m](size_t at) {
		uint32_t v = 0;
		if (at + 4 <= m.size())
			std::memcpy(&v, m.data() + at, 4);
		return v;
	};
	const auto put = [&m](size_t at, uint32_t v) {
		if (at + 4 <= m.size())
			std::memcpy(m.data() + at, &v, 4);
	};
	const auto zero = [&m](size_t from, size_t to) {
		for (size_t i = from; i < to && i < m.size(); ++i)
			m[i] = 0;
	};
	const bool lwf1 = file.header.magic == opennova::lwf::kMagic;
	const uint32_t pool = u32(20);
	zero(24, 28);
	const uint32_t multi_header = u32(16);
	zero(multi_header + 12, multi_header + 20);
	for (size_t i = 0; i < file.singles.size(); ++i) {
		const size_t at = 28 + i * 52;
		const size_t n = file.singles[i].name.size();
		if (n < 32)
			zero(at + n + 1, at + (!lwf1 && n <= 28 ? 28 : 32));
		zero(at + 34, at + 48);
		put(at + 48, lwf1 ? pool + uint32_t(i) * 256 : 0);
		if (lwf1) {
			const size_t slot = pool + i * 256, path = file.singles[i].path.size();
			if (path < 256)
				zero(slot + path + 1, slot + 256);
		}
	}
	if (!lwf1)
		zero(pool, pool + file.singles.size() * 256);
	const size_t triggers = 28 + file.singles.size() * 52;
	zero(triggers, triggers + size_t(file.trigger_count) * 12);
	const uint32_t multi_table = u32(multi_header + 8);
	for (size_t i = 0; i < file.multis.size(); ++i) {
		const size_t at = multi_table + i * 80;
		const size_t n = file.multis[i].name.size();
		if (n < 24)
			zero(at + 4 + n + 1, at + 28);
		zero(at + 40 + std::min<size_t>(u32(at + 36), 8) * 4, at + 72);
	}
	const size_t layers = multi_table + file.multis.size() * 80;
	for (size_t i = 0; i < file.playlists.size(); ++i) {
		const size_t at = layers + i * 48;
		zero((u32(at + 8) & 0x100u) ? at + 14 : at + 12, at + 16);
		zero(at + 16 + std::min<size_t>(u32(at) & 0xFFFFu, 8) * 4, at + 48);
	}
	const size_t members = layers + file.playlists.size() * 48;
	for (size_t i = 0; i < file.sndparms.size(); ++i)
		zero(members + i * 28 + 20, members + i * 28 + 28);
	return m;
}

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
	{"menu.lwf", 1440u, 0xde196f08e1bafac4ull},
	{"stale.lwf", 3912u, 0x02111cd47f543f76ull},
};

struct Bank {
	const char *name;
	bool stale;  // its bytes hold the tools' stale memory, so its write differs
};
const Bank kBanks[] = {{"menu.lwf", false}, {"stale.lwf", true}};

int check(const Bank &bank, const Vector &pin, bool dump) {
	const std::string name = bank.name;
	const std::vector<uint8_t> original =
			test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/lwf/" + name);
	if (original.empty()) {
		std::printf("FAIL: %s unreadable or empty\n", name.c_str());
		return 1;
	}
	opennova::lwf::File file;
	std::string error;
	if (!opennova::lwf::parse_lwf_buffer(original.data(), original.size(), file, error)) {
		std::printf("FAIL: %s parse: %s\n", name.c_str(), error.c_str());
		return 1;
	}
	int failures = 0;
	if (file.header.magic != opennova::lwf::kMagic) { std::printf("FAIL: %s magic\n", name.c_str()); ++failures; }
	if (file.header.single_count != file.singles.size()) { std::printf("FAIL: %s single_count\n", name.c_str()); ++failures; }
	if (file.multis.empty() || file.playlists.empty() || file.sndparms.empty()) {
		std::printf("FAIL: %s has no sets/layers/members\n", name.c_str());
		++failures;
	}
	for (const auto &sp : file.sndparms) {
		if (sp.single_index >= file.singles.size()) {
			std::printf("FAIL: %s member single index %u out of range\n", name.c_str(), sp.single_index);
			++failures;
			break;
		}
	}
	std::vector<uint8_t> encoded;
	if (!opennova::lwf::encode_lwf(file, encoded, error)) {
		std::printf("FAIL: %s encode: %s\n", name.c_str(), error.c_str());
		return failures + 1;
	}
	if (dump) {
		std::printf("\t{\"%s\", %zuu, 0x%016llxull},\n", name.c_str(), encoded.size(),
				(unsigned long long)fnv1a64(encoded));
		return 0;
	}
	opennova::lwf::File again;
	std::vector<uint8_t> twice;
	if (!opennova::lwf::parse_lwf_buffer(encoded.data(), encoded.size(), again, error) ||
			!same_bank(file, again)) {
		std::printf("FAIL: %s does not read back as the bank it was written from\n", name.c_str());
		++failures;
	} else if (!opennova::lwf::encode_lwf(again, twice, error) || twice != encoded) {
		std::printf("FAIL: %s written again is not the same bytes\n", name.c_str());
		++failures;
	}
	if (encoded != stale_masked(original, file)) {
		std::printf("FAIL: %s differs from its bytes past the stale ones\n", name.c_str());
		++failures;
	}
	if ((encoded != original) != bank.stale) {
		std::printf("FAIL: %s's write %s its bytes\n", name.c_str(), bank.stale ? "is" : "is not");
		++failures;
	}
	if (name != pin.name || encoded.size() != pin.len || fnv1a64(encoded) != pin.hash) {
		std::printf("FAIL: %s written: %zu bytes, hash 0x%016llx; pinned %s, %zu bytes, 0x%016llx\n", name.c_str(),
				encoded.size(), (unsigned long long)fnv1a64(encoded), pin.name, pin.len,
				(unsigned long long)pin.hash);
		++failures;
	}
	return failures;
}

} // namespace

int main(int argc, char **argv) {
	const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;
	if (dump) std::printf("const Vector kExpected[] = {\n");
	int failures = 0;
	for (size_t i = 0; i < sizeof(kBanks) / sizeof(kBanks[0]); ++i)
		failures += check(kBanks[i], kExpected[i], dump);
	if (dump) {
		std::printf("};\n");
		return 0;
	}
	std::printf("lwf_bank_sweep: %zu banks, %d failures\n", sizeof(kBanks) / sizeof(kBanks[0]), failures);
	return failures == 0 ? 0 : 1;
}
