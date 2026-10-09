// The reader's places (formats/particle EffectDef::first_line .. last_line, id_line, id_column): each
// effect's block, from its [effectdef] header to its closing brace, and its id's place, the last id read
// naming the effect; and the effect a line is in (ParticleFile::effect_at_line).
#include <formats/particle/parser.h>
#include <formats/particle/particle.h>

#include <cstdio>
#include <string>

#include "common/test_expect.h"

using namespace opennova;

namespace {

particle::ParticleFile parsed(const std::string &text) {
	particle::ParticleFile file;
	particle::ParseError error;
	particle::load_particles_from_buffer(text.data(), text.size(), file, error);
	return file;
}

int test_places() {
	const std::string text = "; a line the reader passes over\r\n[effectdef]\r\n{\r\n\tid =   First;\r\n\tpdefs = a;\r\n}\r\n"
	                         "\r\n[effectdef]\r\n{\r\nid=Second;\r\npdefs = b;\r\nid = Second again;\r\n};\r\n";
	const particle::ParticleFile file = parsed(text);
	TEST_EXPECT(file.effects.size() == 2);
	if (file.effects.size() != 2) return 1;
	const particle::EffectDef &first = file.effects[0], &second = file.effects[1];
	TEST_EXPECT(first.first_line == 2 && first.last_line == 6 && first.id_line == 4 && first.id_column == 9);
	// The last id read names the effect, and its place is the one recorded.
	TEST_EXPECT(second.id == "Second again" && second.first_line == 8 && second.last_line == 13 && second.id_line == 12 &&
	            second.id_column == 6);
	TEST_EXPECT(file.effect_at_line(4) == 0 && file.effect_at_line(6) == 0 && file.effect_at_line(7) == std::string::npos &&
	            file.effect_at_line(10) == 1 && file.effect_at_line(1) == std::string::npos);
	// An effect not read from a text holds no line.
	particle::ParticleFile made;
	made.effects.push_back(particle::EffectDef());
	TEST_EXPECT(made.effect_at_line(1) == std::string::npos);
	std::printf("places: First's id at 4:9, the block 2..6; Second's last id at 12:6\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_places();
	if (failures == 0) std::printf("particle_effect_places: all passed\n");
	return failures == 0 ? 0 : 1;
}
