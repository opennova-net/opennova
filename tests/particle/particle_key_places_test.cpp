// The reader's places of a .ptl's blocks and keys (formats/particle load_particles_with_places, ParticlePlaces) and
// its key table (key_rows, key_row): each block of the four kinds at its section line and closing brace, each
// key's value as the reader takes it at its place (blanks and ';'s around it left out; a key of no value at its
// line's end); a value replaced at its place reads back as that key changed and nothing else; every particle key
// the table names the reader reads (none kept as unknown), lod and the handles the ones the game does not, a
// table's rows by the game's rule, and a key outside it none. The _retail leg: every effect file of the install
// (base and each expansion), each key's value at its place, none of no value and none the table lacks.
#include <formats/particle/parser.h>
#include <formats/particle/particle.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"

using namespace opennova;
using particle::BlockKind;
using particle::KeyValueKind;

namespace {

struct Parsed {
	particle::ParticleFile file;
	particle::ParticlePlaces places;
	bool ok = false;
};

Parsed parsed(const std::string &text) {
	Parsed out;
	particle::ParseError error;
	out.ok = particle::load_particles_with_places(text.data(), text.size(), out.file, out.places, error);
	return out;
}

const char *kText = "[tabledef]\r\n{\r\n\tid = fade;\r\n\ttl1 = 0,1,2,3,4,5,6,7;\r\n}\r\n"
                    "[particledef]\r\n{\r\n\tid = spark;\r\n\tgraphic1 = spark.tga, additive;\r\n"
                    "\tg1_flip_frames =  4 ;;\r\n\temit_burst = 0;\r\n\tcolor1 = 255, 128, 0;\r\n\tlod = 1.0;\r\n"
                    "\tmystery = 5;\r\n}\r\n"
                    "[effectdef]\r\n{\r\n\tid = Hit;\r\n\tpdefs = spark;\r\n}\r\n"
                    "[tabledef_edithandles]\r\n{\r\n\ttableid = fade;\r\n\thandlecount = 3;\r\n}\r\n";

const particle::KeyPlace *place_of(const particle::ParticlePlaces &places, BlockKind block, const char *key) {
	for (const particle::KeyPlace &place : places.keys)
		if (place.block == block && place.key == key) return &place;
	return nullptr;
}

int test_places() {
	const std::string text = kText;
	const Parsed read = parsed(text);
	const particle::ParticleFile &file = read.file;
	const particle::ParticlePlaces &places = read.places;
	TEST_EXPECT(read.ok && places.blocks.size() == 4 && places.keys.size() == 13);
	if (places.blocks.size() != 4) return 1;
	// A plain read keeps none.
	particle::ParticleFile plain;
	particle::ParseError error;
	TEST_EXPECT(particle::load_particles_from_buffer(text.data(), text.size(), plain, error) &&
	            plain.particles.size() == file.particles.size());
	const BlockKind kinds[4] = {BlockKind::Table, BlockKind::Particle, BlockKind::Effect, BlockKind::Handles};
	const int first[4] = {1, 6, 16, 21}, last[4] = {5, 15, 20, 25};
	for (size_t i = 0; i < 4; ++i) {
		const particle::BlockPlace &block = places.blocks[i];
		TEST_EXPECT(block.kind == kinds[i] && block.index == 0 && block.first_line == first[i] && block.last_line == last[i]);
		TEST_EXPECT(text.compare(block.close_offset, 3, "}\r\n") == 0);
	}
	// Each value as the reader takes it, at its place.
	struct Expect {
		BlockKind block;
		const char *key;
		const char *value;
		int line;
	};
	const Expect expects[] = {
			{BlockKind::Table, "tl1", "0,1,2,3,4,5,6,7", 4},  {BlockKind::Particle, "graphic1", "spark.tga, additive", 9},
			{BlockKind::Particle, "g1_flip_frames", "4", 10}, {BlockKind::Particle, "color1", "255, 128, 0", 12},
			{BlockKind::Particle, "mystery", "5", 14},        {BlockKind::Effect, "pdefs", "spark", 19},
			{BlockKind::Handles, "handlecount", "3", 24},
	};
	for (const Expect &e : expects) {
		const particle::KeyPlace *place = place_of(places, e.block, e.key);
		TEST_EXPECT(place && place->index == 0 && place->line == e.line &&
		            text.substr(place->offset, place->length) == e.value);
	}
	// A value replaced at its place: that key alone changes.
	const particle::KeyPlace *frames = place_of(places, BlockKind::Particle, "g1_flip_frames");
	TEST_EXPECT(frames != nullptr);
	if (!frames) return 1;
	std::string edited = text;
	edited.replace(frames->offset, frames->length, "9");
	const Parsed again = parsed(edited);
	TEST_EXPECT(again.file.particles.size() == 1 && file.particles.size() == 1);
	if (again.file.particles.size() != 1 || file.particles.size() != 1) return 1;
	const particle::ParticleDef &before = file.particles[0], &after = again.file.particles[0];
	TEST_EXPECT(before.graphics[0].flip_frames == 4 && after.graphics[0].flip_frames == 9);
	TEST_EXPECT(after.emit_burst == before.emit_burst && after.color1.r == before.color1.r &&
	            after.graphics[0].texture == before.graphics[0].texture && again.places.keys.size() == places.keys.size());
	return 0;
}

// A key of no value at the end of its line: its place is that line's end, before its line end, so a value written
// there is that key's and the next line's key reads as it did.
int test_empty_value() {
	const std::string text = "[particledef]\r\n{\r\n\tid = a;\r\n\tchild_id =\r\n\temit_rate = 5;\r\n}\r\n";
	const Parsed read = parsed(text);
	const particle::KeyPlace *child = place_of(read.places, BlockKind::Particle, "child_id");
	TEST_EXPECT(read.ok && child && child->line == 4 && child->length == 0);
	if (!child) return 1;
	TEST_EXPECT(text.compare(child->offset, 2, "\r\n") == 0);
	std::string edited = text;
	edited.insert(child->offset, "spark");
	const Parsed again = parsed(edited);
	TEST_EXPECT(again.file.particles.size() == 1);
	if (again.file.particles.size() != 1) return 1;
	const particle::ParticleDef &def = again.file.particles[0];
	TEST_EXPECT(def.child_id == "spark" && def.emit_rate == 5.0f && def.unknown_keys.empty());
	// With a ';' after the blank, the place is the ';' (the value's own start).
	const std::string semi = "[particledef]\r\n{\r\n\tchild_id = ;\r\n}\r\n";
	const Parsed with_semi = parsed(semi);
	const particle::KeyPlace *at = place_of(with_semi.places, BlockKind::Particle, "child_id");
	TEST_EXPECT(at && at->length == 0 && semi[at->offset] == ';');
	return 0;
}

// What the reader makes of a value of each kind, written so it reads (a name, a number, three bytes, flags).
const char *value_of(KeyValueKind kind) {
	switch (kind) {
	case KeyValueKind::Real: return "1.5";
	case KeyValueKind::Whole: return "2";
	case KeyValueKind::Color: return "1, 2, 3";
	case KeyValueKind::Vector: return "1, 2, 3";
	case KeyValueKind::Flags: return "NEVERAGE";
	case KeyValueKind::Move: return "NORMAL";
	case KeyValueKind::Curve: return "fade reverse";
	case KeyValueKind::Graphic: return "a.tga, additive";
	default: return "x";
	}
}

int test_rows() {
	// Every particle key the table names, its pattern's digits filled, is one the reader reads (none unknown).
	size_t particle_rows = 0;
	for (const particle::KeyRow &row : particle::key_rows()) {
		if (row.block != BlockKind::Particle) continue;
		++particle_rows;
		std::string key = row.key;
		for (char &c : key) {
			if (c == '#') c = '2';
			if (c == '*') c = '7';
		}
		const std::string text = "[particledef]\r\n{\r\n\t" + key + " = " + value_of(row.value) + ";\r\n}\r\n";
		const particle::ParticleFile file = parsed(text).file;
		TEST_EXPECT(file.particles.size() == 1);
		if (file.particles.size() != 1) return 1;
		if (!file.particles[0].unknown_keys.empty()) std::fprintf(stderr, "unread: %s\n", key.c_str());
		TEST_EXPECT(file.particles[0].unknown_keys.empty());
		TEST_EXPECT(particle::key_row(BlockKind::Particle, key) == &row);
	}
	TEST_EXPECT(particle_rows == 68);
	// The rows' patterns and their bounds.
	using particle::key_row;
	TEST_EXPECT(key_row(BlockKind::Particle, "G2_COLOR3") && !key_row(BlockKind::Particle, "g5_alpha") &&
	            !key_row(BlockKind::Particle, "g2alpha"));
	// A collision sound's slot is atol of what follows the prefix, under 20: the prefix alone slot 0, a word after
	// the digits ignored, the parser taking each so.
	TEST_EXPECT(key_row(BlockKind::Particle, "collide_sound19") && !key_row(BlockKind::Particle, "collide_sound20") &&
	            key_row(BlockKind::Particle, "collide_sound") && key_row(BlockKind::Particle, "COLLIDE_SOUND7x") &&
	            !key_row(BlockKind::Particle, "collide_sound-1"));
	{
		const particle::ParticleFile sounds =
				parsed("[particledef]\r\n{\r\n\tcollide_sound = a;\r\n\tcollide_sound7x = b;\r\n"
				       "\tcollide_sound20 = c;\r\n}\r\n").file;
		TEST_EXPECT(sounds.particles.size() == 1);
		if (sounds.particles.size() != 1) return 1;
		const particle::ParticleDef &def = sounds.particles[0];
		TEST_EXPECT(def.collide_sounds[0] == "a" && def.collide_sounds[7] == "b" && def.unknown_keys.size() == 1);
	}
	// A table's rows by the game's rule: `id` without case, any other key holding a 't' (with case) a row.
	TEST_EXPECT(key_row(BlockKind::Table, "ID") == key_row(BlockKind::Table, "id") && key_row(BlockKind::Table, "id"));
	const particle::KeyRow *tl = key_row(BlockKind::Table, "tl1");
	TEST_EXPECT(tl && tl->value == KeyValueKind::TableRow && key_row(BlockKind::Table, "tl33") == tl &&
	            key_row(BlockKind::Table, "tl0") == tl && key_row(BlockKind::Table, "t") == tl &&
	            !key_row(BlockKind::Table, "TL32") && !key_row(BlockKind::Table, "row"));
	TEST_EXPECT(!key_row(BlockKind::Particle, "mystery") && !key_row(BlockKind::Effect, "flags"));
	const particle::KeyRow *lod = key_row(BlockKind::Particle, "lod");
	TEST_EXPECT(lod && !lod->read && lod->value == KeyValueKind::Real);
	// The handles the game never reaches.
	for (const char *key : {"tableid", "handlecount", "tightness", "handle0", "HANDLE20"}) {
		const particle::KeyRow *handle = key_row(BlockKind::Handles, key);
		TEST_EXPECT(handle && !handle->read);
	}
	TEST_EXPECT(!key_row(BlockKind::Handles, "handle") && !key_row(BlockKind::Handles, "handle1x"));
	// flip_frames' bound is OpenNova's, emit_burst's the game's.
	const particle::KeyRow *frames = key_row(BlockKind::Particle, "g1_flip_frames");
	TEST_EXPECT(frames && frames->clamped && frames->min == 1 && frames->max == particle::kMaxParticleFlipFrames &&
	            frames->port_bound && std::strcmp(frames->port_bound, "D-PTL-19") == 0);
	const particle::KeyRow *burst = key_row(BlockKind::Particle, "emit_burst");
	TEST_EXPECT(burst && burst->clamped && burst->min == 1 && !burst->port_bound);
	return 0;
}

// The value the reader takes on a key's line: after the '=' and its blanks, before the ';'s and blanks ending it.
std::string value_on_line(const std::string &text, size_t offset) {
	size_t start = text.rfind('\n', offset == 0 ? 0 : offset - 1);
	start = start == std::string::npos ? 0 : start + 1;
	size_t end = text.find('\n', offset);
	if (end == std::string::npos) end = text.size();
	std::string line = text.substr(start, end - start);
	const size_t eq = line.find('=');
	if (eq == std::string::npos) return "<no =>";
	std::string value = line.substr(eq + 1);
	while (!value.empty() && (value.back() == ';' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
		value.pop_back();
	size_t lead = 0;
	while (lead < value.size() && (value[lead] == ' ' || value[lead] == '\t')) ++lead;
	return value.substr(lead);
}

// Every effect file the install serves (base, then each expansion's own or changed), through the packed archives
// as the game reads them: each parses, each block's close a '}' line, each key's value at its place exactly the
// value on its line, none of no value, every key one the table names; the counts pinned.
int test_retail() {
	const std::string install = retail::install();
	std::map<std::string, std::vector<uint8_t>> base;
	size_t files = 0, blocks = 0, keys = 0;
	size_t by_kind[4] = {0, 0, 0, 0};
	std::vector<std::string> mounts{std::string()};
	for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
	for (const std::string &expansion : mounts) {
		opennova::Vfs vfs;
		TEST_EXPECT(vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed));
		for (const auto &location : vfs.list_files()) {
			const std::string name = retail::lower_ascii(location.logical_name);
			if (name.size() < 4) continue;
			const std::string ext = name.substr(name.size() - 4);
			if (ext != ".ptl" && ext != ".ptu" && ext != ".ptg") continue;
			std::vector<uint8_t> bytes;
			if (!vfs.read_file(location.logical_name, bytes)) continue;
			if (expansion.empty()) {
				base[name] = bytes;
			} else {
				const auto held = base.find(name);
				if (held != base.end() && held->second == bytes) continue; // the base's file, counted there
			}
			++files;
			const std::string text(bytes.begin(), bytes.end());
			const Parsed read = parsed(text);
			if (!read.ok) std::fprintf(stderr, "%s does not parse\n", name.c_str());
			TEST_EXPECT(read.ok);
			for (const particle::BlockPlace &block : read.places.blocks) {
				++blocks;
				TEST_EXPECT(block.close_offset < text.size() && text[block.close_offset] == '}');
			}
			for (const particle::ParticleDef &def : read.file.particles) TEST_EXPECT(def.unknown_keys.empty());
			for (const particle::KeyPlace &key : read.places.keys) {
				++keys;
				++by_kind[static_cast<size_t>(key.block)];
				const std::string at = text.substr(key.offset, key.length);
				if (at != value_on_line(text, key.offset) || at.empty() || !particle::key_row(key.block, key.key))
					std::fprintf(stderr, "%s:%d %s: \"%s\"\n", name.c_str(), key.line, key.key.c_str(), at.c_str());
				TEST_EXPECT(at == value_on_line(text, key.offset) && !at.empty());
				TEST_EXPECT(particle::key_row(key.block, key.key) != nullptr);
			}
		}
	}
	std::printf("  %zu effect files, %zu blocks, %zu keys (%zu effect, %zu particle, %zu table, %zu handles)\n", files,
	            blocks, keys, by_kind[0], by_kind[1], by_kind[2], by_kind[3]);
	TEST_EXPECT(files == 85 && blocks == 2895 && keys == 123369);
	TEST_EXPECT(by_kind[0] == 1098 && by_kind[1] == 104627 && by_kind[2] == 16467 && by_kind[3] == 1177);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = test_places();
	failures += test_empty_value();
	failures += test_rows();
	if (retail::install().empty())
		retail::skip_leg("OPENNOVA_JO_DIR (every effect file the install serves)");
	else
		failures += test_retail();
	if (failures == 0) std::printf("particle_key_places: all passed\n");
	return failures == 0 ? 0 : 1;
}
