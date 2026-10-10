// The reader's places of a .ptl's blocks and keys (formats/particle ParticleFile::block_places, key_places) and
// its key table (key_rows, key_row): each block of the four kinds at its section line and closing brace, each
// key's value as the reader takes it at its place (blanks and ';'s around it left out); a value replaced at its
// place reads back as that key changed and nothing else; every key the table names the reader reads (none kept
// as unknown), lod the one it does not, and a key outside it none.
#include <formats/particle/parser.h>
#include <formats/particle/particle.h>

#include <cstdio>
#include <string>

#include "common/test_expect.h"

using namespace opennova;
using particle::BlockKind;
using particle::KeyValueKind;

namespace {

particle::ParticleFile parsed(const std::string &text) {
	particle::ParticleFile file;
	particle::ParseError error;
	particle::load_particles_from_buffer(text.data(), text.size(), file, error);
	return file;
}

const char *kText = "[tabledef]\r\n{\r\n\tid = fade;\r\n\ttl1 = 0,1,2,3,4,5,6,7;\r\n}\r\n"
                    "[particledef]\r\n{\r\n\tid = spark;\r\n\tgraphic1 = spark.tga, additive;\r\n"
                    "\tg1_flip_frames =  4 ;;\r\n\temit_burst = 0;\r\n\tcolor1 = 255, 128, 0;\r\n\tlod = 1.0;\r\n"
                    "\tmystery = 5;\r\n}\r\n"
                    "[effectdef]\r\n{\r\n\tid = Hit;\r\n\tpdefs = spark;\r\n}\r\n"
                    "[tabledef_edithandles]\r\n{\r\n\ttableid = fade;\r\n\thandlecount = 3;\r\n}\r\n";

const particle::KeyPlace *place_of(const particle::ParticleFile &file, BlockKind block, const char *key) {
	for (const particle::KeyPlace &place : file.key_places)
		if (place.block == block && place.key == key) return &place;
	return nullptr;
}

int test_places() {
	const std::string text = kText;
	const particle::ParticleFile file = parsed(text);
	TEST_EXPECT(file.block_places.size() == 4 && file.key_places.size() == 13);
	if (file.block_places.size() != 4) return 1;
	const BlockKind kinds[4] = {BlockKind::Table, BlockKind::Particle, BlockKind::Effect, BlockKind::Handles};
	const int first[4] = {1, 6, 16, 21}, last[4] = {5, 15, 20, 25};
	for (size_t i = 0; i < 4; ++i) {
		const particle::BlockPlace &block = file.block_places[i];
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
		const particle::KeyPlace *place = place_of(file, e.block, e.key);
		TEST_EXPECT(place && place->index == 0 && place->line == e.line &&
		            text.substr(place->offset, place->length) == e.value);
	}
	// A value replaced at its place: that key alone changes.
	const particle::KeyPlace *frames = place_of(file, BlockKind::Particle, "g1_flip_frames");
	TEST_EXPECT(frames != nullptr);
	if (!frames) return 1;
	std::string edited = text;
	edited.replace(frames->offset, frames->length, "9");
	const particle::ParticleFile again = parsed(edited);
	TEST_EXPECT(again.particles.size() == 1 && file.particles.size() == 1);
	if (again.particles.size() != 1 || file.particles.size() != 1) return 1;
	const particle::ParticleDef &before = file.particles[0], &after = again.particles[0];
	TEST_EXPECT(before.graphics[0].flip_frames == 4 && after.graphics[0].flip_frames == 9);
	TEST_EXPECT(after.emit_burst == before.emit_burst && after.color1.r == before.color1.r &&
	            after.graphics[0].texture == before.graphics[0].texture && again.key_places.size() == file.key_places.size());
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
		const particle::ParticleFile file = parsed(text);
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
	TEST_EXPECT(key_row(BlockKind::Particle, "collide_sound19") && !key_row(BlockKind::Particle, "collide_sound20") &&
	            !key_row(BlockKind::Particle, "collide_sound"));
	TEST_EXPECT(key_row(BlockKind::Table, "TL32") && !key_row(BlockKind::Table, "tl33") && !key_row(BlockKind::Table, "tl0"));
	TEST_EXPECT(!key_row(BlockKind::Particle, "mystery") && !key_row(BlockKind::Effect, "flags"));
	const particle::KeyRow *lod = key_row(BlockKind::Particle, "lod");
	TEST_EXPECT(lod && !lod->read && lod->value == KeyValueKind::Real);
	const particle::KeyRow *frames = key_row(BlockKind::Particle, "g1_flip_frames");
	TEST_EXPECT(frames && frames->clamped && frames->min == 1 && frames->max == particle::kMaxParticleFlipFrames);
	const particle::KeyRow *burst = key_row(BlockKind::Particle, "emit_burst");
	TEST_EXPECT(burst && burst->clamped && burst->min == 1);
	return 0;
}

} // namespace

int main() {
	int failures = test_places();
	failures += test_rows();
	if (failures == 0) std::printf("particle_key_places: all passed\n");
	return failures == 0 ? 0 : 1;
}
