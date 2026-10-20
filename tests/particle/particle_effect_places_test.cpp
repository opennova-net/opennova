// The reader's places (formats/particle EffectDef::first_line .. last_line, id_line, id_column): each
// effect's block, from the [effectdef] header its pdefs line follows to its closing brace, and the place of
// the id line whose name the pdefs line took (the name buffer as it stood then: an id after the pdefs line
// renames nothing); and the effect a line is in (ParticleFile::effect_at_line) [orig:
// CParticleTableDef_ParseScriptLine @ 0x5e92b0].
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
	                         "\r\n[effectdef]\r\n{\r\nid = Second;\r\npdefs = b;\r\nid = Second again;\r\n}\r\n";
	const particle::ParticleFile file = parsed(text);
	TEST_EXPECT(file.effects.size() == 2);
	if (file.effects.size() != 2) return 1;
	const particle::EffectDef &first = file.effects[0], &second = file.effects[1];
	TEST_EXPECT(first.first_line == 2 && first.last_line == 6 && first.id_line == 4 && first.id_column == 9);
	// The id before the pdefs line names the effect (one word: "again" is the line's next token), its place recorded.
	TEST_EXPECT(second.id == "Second" && second.first_line == 8 && second.last_line == 13 && second.id_line == 10 &&
	            second.id_column == 6);
	TEST_EXPECT(file.effect_at_line(4) == 0 && file.effect_at_line(6) == 0 && file.effect_at_line(7) == std::string::npos &&
	            file.effect_at_line(10) == 1 && file.effect_at_line(1) == std::string::npos);
	// An effect not read from a text holds no line.
	particle::ParticleFile made;
	made.effects.push_back(particle::EffectDef());
	TEST_EXPECT(made.effect_at_line(1) == std::string::npos);
	// `};` is no closing brace to the effect reader (stricmp of the line's first token, "};", with "}"), and an id
	// whose '=' touches it is no id line ("id=x;" is one token): the effect is never registered.
	TEST_EXPECT(parsed("[effectdef]\r\n{\r\nid = x;\r\npdefs = a;\r\n};\r\n").effects.empty());
	TEST_EXPECT(parsed("[effectdef]\r\n{\r\nid = x;\r\nid=y;\r\npdefs = a;\r\n}\r\n").effects.at(0).id == "x");
	std::printf("places: First's id at 4:9, the block 2..6; Second's id at 10:6\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_places();
	if (failures == 0) std::printf("particle_effect_places: all passed\n");
	return failures == 0 ? 0 : 1;
}
