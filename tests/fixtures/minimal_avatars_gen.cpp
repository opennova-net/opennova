// Generator + guard for fixtures/avatars/synth_avatars.def: a synthetic avatar
// table written by avatars_write (ADR 0021: from scratch, deterministic) from
// authored rows, in the shape the shipped Avatars.def has — a pool of head /
// body / arms parts (camo variants, a quoted multi-word display name, both
// sexes) and an eight-nationality tree of divisions and combos with the
// trailing `skipdemo` flags at both levels, four good and four evil
// nationalities so the team filter partitions and the packed character ids the
// join profile derives (nat | div | combo | side) stay reachable. No retail
// table is carried: the shipped file is the reference-tree leg of avatars_parse
// and avatars_roundtrip.
//
// Default: rebuild in memory and byte-compare the committed file. `--write`
// (re)writes it.
#include "common/test_paths.h"

#include <formats/avatars/avatars.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

using namespace opennova::avatars;

#define SETSTR(field, val) std::snprintf((field), sizeof(field), "%s", (val))

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

// The part pool: 12 heads, 8 bodies, 6 arms.
constexpr int kHeads = 12;
constexpr int kBodies = 8;
constexpr int kArms = 6;

AvatarPart part(int kind, const char *name, const char *display, const char *graphic, int camo0, int voice,
                int sex) {
	AvatarPart p;
	std::memset(&p, 0, sizeof(p));
	p.kind = kind;
	SETSTR(p.name, name);
	SETSTR(p.display_name, display);
	SETSTR(p.graphic, graphic);
	p.camo[0] = camo0;
	p.voice = voice;
	p.sex = sex;
	return p;
}

std::vector<AvatarPart> make_parts() {
	std::vector<AvatarPart> parts;
	// Heads: the boonie, its camo variant, then ten more; the last two female.
	parts.push_back(part(AVATAR_PART_HEAD, "SYN_HEAD_BOONIE", "AV_SYNTH_BOONIE", "synth_boonie.3di", 0, 1,
	                     AVATAR_SEX_MALE));
	parts.push_back(part(AVATAR_PART_HEAD, "SYN_HEAD_BOONIE_CAMO_1", "AV_SYNTH_BOONIE", "synth_boonie.3di", 3, 1,
	                     AVATAR_SEX_MALE));
	for (int i = 2; i < kHeads; ++i) {
		const std::string name = "SYN_HEAD_" + std::to_string(i);
		const std::string display = "AV_SYNTH_HEAD_" + std::to_string(i);
		const std::string graphic = "synth_head_" + std::to_string(i) + ".3di";
		parts.push_back(part(AVATAR_PART_HEAD, name.c_str(), display.c_str(), graphic.c_str(), 0, 1 + i % 3,
		                     i >= kHeads - 2 ? AVATAR_SEX_FEMALE : AVATAR_SEX_MALE));
	}
	for (int i = 0; i < kBodies; ++i) {
		const std::string name = "SYN_BODY_" + std::to_string(i);
		const std::string display = "AV_SYNTH_BODY_" + std::to_string(i);
		const std::string graphic = "synth_body_" + std::to_string(i) + ".3di";
		parts.push_back(part(AVATAR_PART_BODY, name.c_str(), display.c_str(), graphic.c_str(), 0, 0,
		                     i >= kBodies - 1 ? AVATAR_SEX_FEMALE : AVATAR_SEX_MALE));
	}
	// Arms: the first carries a quoted multi-word display name.
	parts.push_back(part(AVATAR_PART_ARMS, "SYN_ARMS_BARE", "Synth Bare Arms", "synth_arms_bare.3di", 0, 0,
	                     AVATAR_SEX_MALE));
	for (int i = 1; i < kArms; ++i) {
		const std::string name = "SYN_ARMS_" + std::to_string(i);
		const std::string display = "AV_SYNTH_ARMS_" + std::to_string(i);
		const std::string graphic = "synth_arms_" + std::to_string(i) + ".3di";
		parts.push_back(part(AVATAR_PART_ARMS, name.c_str(), display.c_str(), graphic.c_str(), 0, 0,
		                     AVATAR_SEX_MALE));
	}
	return parts;
}

// Storage for the tree (the C structs point into these).
struct Tree {
	std::vector<AvatarNationality> nationalities;
	std::vector<std::vector<AvatarDivision>> divisions;
	std::vector<std::vector<std::vector<AvatarCombo>>> combos;
};

AvatarCombo combo(int id, const char *head, const char *body, const char *arms) {
	AvatarCombo c;
	std::memset(&c, 0, sizeof(c));
	char raw[8];
	std::snprintf(raw, sizeof(raw), "%03d", id);
	SETSTR(c.raw_id, raw);
	c.id = id;
	SETSTR(c.head_name, head);
	SETSTR(c.body_name, body);
	if (arms != nullptr) {
		SETSTR(c.arms_name, arms);
		c.has_arms = 1;
	}
	return c;
}

// Eight nationalities N00..N07 (0..3 good, 4..7 evil); nationality 0 carries
// four divisions (its first flagged skipdemo with four combos, its third
// unflagged), nationality 1 carries the nationality-level skipdemo flag, the
// rest two divisions of two combos each. Combo parts cycle through the pool.
void make_tree(Tree &t, const std::vector<AvatarPart> &parts) {
	static const char *const kNatKeys[8] = {"AV_NAT_SYNTH_ALPHA", "AV_NAT_SYNTH_BRAVO", "AV_NAT_SYNTH_CHARLIE",
	                                        "AV_NAT_SYNTH_DELTA", "AV_NAT_SYNTH_ECHO",  "AV_NAT_SYNTH_FOXTROT",
	                                        "AV_NAT_SYNTH_GOLF",  "AV_NAT_SYNTH_HOTEL"};
	auto head_name = [&](int i) { return parts[static_cast<size_t>(i % kHeads)].name; };
	auto body_name = [&](int i) { return parts[static_cast<size_t>(kHeads + i % kBodies)].name; };
	auto arms_name = [&](int i) { return parts[static_cast<size_t>(kHeads + kBodies + i % kArms)].name; };
	t.nationalities.resize(8);
	t.divisions.resize(8);
	t.combos.resize(8);
	int serial = 0;
	for (int n = 0; n < 8; ++n) {
		const int division_count = n == 0 ? 4 : 2;
		t.divisions[static_cast<size_t>(n)].resize(static_cast<size_t>(division_count));
		t.combos[static_cast<size_t>(n)].resize(static_cast<size_t>(division_count));
		for (int d = 0; d < division_count; ++d) {
			const int combo_count = (n == 0 && d == 0) ? 4 : (n == 0 && d == 1) ? 3 : 2;
			std::vector<AvatarCombo> &combos = t.combos[static_cast<size_t>(n)][static_cast<size_t>(d)];
			for (int c = 0; c < combo_count; ++c) {
				// The first combo of the first division wears the boonie set.
				if (n == 0 && d == 0 && c == 0)
					combos.push_back(combo(1, "SYN_HEAD_BOONIE", "SYN_BODY_0", "SYN_ARMS_1"));
				else
					combos.push_back(combo(c + 1, head_name(serial), body_name(serial), arms_name(serial)));
				++serial;
			}
			AvatarDivision &div = t.divisions[static_cast<size_t>(n)][static_cast<size_t>(d)];
			std::memset(&div, 0, sizeof(div));
			char raw[8];
			std::snprintf(raw, sizeof(raw), "D%02d", d);
			SETSTR(div.raw_id, raw);
			div.id = d;
			const std::string key = "AV_DIV_SYNTH_" + std::to_string(n) + "_" + std::to_string(d);
			SETSTR(div.name_key, key.c_str());
			if (d == 0 || d == 3) SETSTR(div.flags, "skipdemo");
			div.combos = combos.data();
			div.combos_count = combos.size();
		}
		AvatarNationality &nat = t.nationalities[static_cast<size_t>(n)];
		std::memset(&nat, 0, sizeof(nat));
		char raw[8];
		std::snprintf(raw, sizeof(raw), "N%02d", n);
		SETSTR(nat.raw_id, raw);
		nat.id = n;
		SETSTR(nat.name_key, kNatKeys[n]);
		if (n == 1) SETSTR(nat.flags, "skipdemo");
		nat.has_alignment = 1;
		nat.alignment = n < 4 ? AVATAR_ALIGN_GOOD : AVATAR_ALIGN_EVIL;
		nat.divisions = t.divisions[static_cast<size_t>(n)].data();
		nat.divisions_count = t.divisions[static_cast<size_t>(n)].size();
	}
}

size_t total_combos(const AvatarsFile &f) {
	size_t n = 0;
	for (size_t i = 0; i < f.nationalities_count; ++i)
		for (size_t j = 0; j < f.nationalities[i].divisions_count; ++j) n += f.nationalities[i].divisions[j].combos_count;
	return n;
}

// avatars_write's bytes for the table, parsed back once and re-written to prove
// the writer is lossless and deterministic over it.
bool build(std::vector<uint8_t> &bytes, std::string &err) {
	std::vector<AvatarPart> parts = make_parts();
	Tree tree;
	make_tree(tree, parts);
	AvatarsFile file;
	std::memset(&file, 0, sizeof(file));
	file.parts = parts.data();
	file.parts_count = parts.size();
	file.nationalities = tree.nationalities.data();
	file.nationalities_count = tree.nationalities.size();

	char *data = nullptr;
	size_t size = 0;
	if (avatars_write(&file, &data, &size) != 0) {
		err = "avatars_write failed";
		return false;
	}
	AvatarsFile back;
	if (avatars_parse_memory(data, size, &back) != 0) {
		avatars_free_buffer(data);
		err = "the written table does not parse";
		return false;
	}
	char *again = nullptr;
	size_t again_size = 0;
	const bool wrote_again = avatars_write(&back, &again, &again_size) == 0;
	const bool stable = wrote_again && again_size == size && std::memcmp(again, data, size) == 0;
	const bool shape_ok = back.parts_count == static_cast<size_t>(kHeads + kBodies + kArms) &&
	                      back.nationalities_count == 8 && total_combos(back) == 4 + 3 + 2 + 2 + 7 * 4 &&
	                      back.diagnostics_count == 0;
	bytes.assign(data, data + size);
	avatars_free(&back);
	avatars_free_buffer(data);
	if (wrote_again) avatars_free_buffer(again);
	if (!stable) {
		err = "write(parse(write(table))) is not byte-stable";
		return false;
	}
	if (!shape_ok) {
		err = "the re-parsed table lost a pinned field";
		return false;
	}
	return true;
}

using test_io::read_file;

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/avatars/synth_avatars.def";

	std::vector<uint8_t> bytes;
	std::string err;
	if (!expect(build(bytes, err), ("synth_avatars.def: " + err).c_str())) return 1;
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		if (!expect(static_cast<bool>(o), ("cannot open for writing: " + path).c_str())) return 1;
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str())) return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str()))
		return 1;
	std::printf("OK: fixtures/avatars/synth_avatars.def byte-reproducible (%zu bytes)\n", bytes.size());
	return 0;
}
