#include <particle/parser.h>

#include <cstdio>
#include <sstream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Retail ignores lines its parsers don't claim — top-level comment dividers
// and =-less strays never reject a file [orig: CEffectWorld_ParseSectionCallback
// @ 0x5ecb40]. Our early parser hard-failed the whole file on them, which lost
// RevX02's entire Effect_AmHit* family to a "//====" divider (D-PTL-13).
const char *kSnippet =
		"[effectdef]\n"
		"{\n"
		"\tid = first_effect;\n"
		"\tpdefs = blank;\n"
		"\tthis line has no equals sign and is skipped\n"
		"}\n"
		"\n"
		"//===============v=High Caliber Impacts =v==============\n"
		"\n"
		"stray top-level words are also skipped\n"
		"\n"
		"[effectdef]\n"
		"{\n"
		"\tid = second_effect;\n"
		"\tpdefs = blank;\n"
		"}\n";

const char *kMixedCaseSnippet = R"PTL(
[EffectDef]
{
	ID = MixedEffect;
	PDefs = MixedParticle;
}
[ParticleDef]
{
	ID = MixedParticle;
	Emit_Rate = 12.5;
	Graphic1 = Spark.tga, Additive;
	G1_Flip_Frames = 3;
	Future_Key = PreserveMe;
}
[TableDef]
{
	ID = MixedTable;
	TL1 = 1, 2, 3, 4, 5, 6, 7, 8;
}
[TableDef_EditHandles]
{
	TableID = MixedTable;
	HandleCount = 2;
	Tightness = 3;
}
)PTL";

bool mixed_case_tokens_follow_retail_stricmp() {
	std::istringstream stream{std::string(kMixedCaseSnippet)};
	opennova::particle::ParticleFile file;
	opennova::particle::ParseError error;
	if (!opennova::particle::load_particles(stream, file, error)) {
		std::fprintf(stderr, "FAIL: mixed-case parse error at line %d: %s\n",
				error.line, error.message.c_str());
		return false;
	}
	if (!expect(file.effects.size() == 1 &&
			file.effects[0].id == "MixedEffect" &&
			file.effects[0].pdefs.size() == 1 &&
			file.effects[0].pdefs[0] == "MixedParticle",
			"effect section and keys compare case-insensitively")) return false;
	if (!expect(file.particles.size() == 1 &&
			file.particles[0].id == "MixedParticle" &&
			file.particles[0].emit_rate == 12.5f &&
			file.particles[0].graphics[0].present &&
			file.particles[0].graphics[0].flip_frames == 3,
			"particle and graphic keys compare case-insensitively")) return false;
	if (!expect(file.particles[0].unknown_keys.size() == 1 &&
			file.particles[0].unknown_keys[0].first == "Future_Key",
			"unknown keys preserve authored spelling")) return false;
	if (!expect(file.tables.size() == 1 &&
			file.tables[0].id == "MixedTable" &&
			file.tables[0].rows.size() == 1 &&
			file.tables[0].rows[0][0] == 1 &&
			file.tables[0].rows[0][7] == 8,
			"table section, ID, and TL row compare case-insensitively")) return false;
	return expect(file.table_handles.size() == 1 &&
			file.table_handles[0].table_id == "MixedTable" &&
			file.table_handles[0].handlecount == 2 &&
			file.table_handles[0].tightness == 3,
			"edit-handle section and keys compare case-insensitively");
}

} // namespace

int main() {
	std::istringstream stream{std::string(kSnippet)};

	opennova::particle::ParticleFile file;
	opennova::particle::ParseError error;
	if (!opennova::particle::load_particles(stream, file, error)) {
		std::fprintf(stderr, "FAIL: parse error at line %d: %s\n", error.line, error.message.c_str());
		return 1;
	}

	if (!expect(file.effects.size() == 2, "both effectdefs survive the stray lines")) return 1;
	if (!expect(file.effects[0].id == "first_effect", "effect[0] id")) return 1;
	if (!expect(file.effects[1].id == "second_effect", "the divider comment never aborts the file")) return 1;
	if (!expect(file.effects[0].pdefs.size() == 1 && file.effects[0].pdefs[0] == "blank",
	            "the =-less in-block line is skipped without eating the block")) return 1;
	if (!mixed_case_tokens_follow_retail_stricmp()) return 1;

	std::printf("particle_lenient_lines: OK\n");
	return 0;
}
