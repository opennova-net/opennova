// The particle reader's line walk, as the game's effect system reads a .ptl
// [orig: File_ParseASCIIFile @ 0x53D810 with CEffectWorld_ParseSectionCallback
// @ 0x5ECB40, its line readers CParticleTableDef_ParseScriptLine @ 0x5E92B0
// ([effectdef]), CParticleDef_ParseProperties @ 0x5EA320 ([particledef]) and
// CEffectTableDef_ParseCallback @ 0x5E4010 ([tabledef])]: lines cut at CR LF
// and tokenized, each line in the order written, and what the readers keep
// between lines, blocks and files (D-PTL-32, D-PTL-38).

#include <formats/particle/parser.h>

#include <base/io/strutil.h>

#include <cstdio>
#include <string>

namespace {

using opennova::particle::ParticleFile;
using opennova::particle::ParticleReadState;

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// A text with its lines ended CR LF, read through `state` (a fresh one when none).
ParticleFile read(const char *source, ParticleReadState *state = nullptr, bool crlf = true) {
	const std::string text = crlf ? opennova::strutil::normalized_crlf_line_ends(source) : std::string(source);
	ParticleFile file;
	opennova::particle::ParseError error;
	ParticleReadState fresh;
	opennova::particle::load_particles_from_buffer(text.data(), text.size(), file, state ? *state : fresh, error);
	return file;
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

bool stray_lines_are_skipped() {
	const ParticleFile file = read(kSnippet);
	return expect(file.effects.size() == 2 && file.effects[0].id == "first_effect" &&
						  file.effects[1].id == "second_effect" && file.effects[0].pdefs.size() == 1 &&
						  file.effects[0].pdefs[0] == "blank",
			"the divider comment and the stray lines are skipped, both effects read");
}

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
	tl2 = 9, 9, 9, 9, 9, 9, 9, 9;
}
[TableDef_EditHandles]
{
	TableID = MixedTable;
	HandleCount = 2;
	Tightness = 3;
}
)PTL";

bool mixed_case_tokens_follow_retail_stricmp() {
	// Section headers and keys compare without case (_stricmp); a table's row is a key holding a lowercase 't'
	// (strstr), so TL1 is none; the blend word is found with case (strstr), so "Additive" is Blend [orig:
	// CParticleDefEntry_ParseBlendMode @ 0x5E29F0].
	const ParticleFile file = read(kMixedCaseSnippet);
	if (!expect(file.effects.size() == 1 && file.effects[0].id == "MixedEffect" && file.effects[0].pdefs.size() == 1 &&
						file.effects[0].pdefs[0] == "MixedParticle",
				"effect section and keys compare case-insensitively")) return false;
	if (!expect(file.particles.size() == 1 && file.particles[0].id == "MixedParticle" &&
						file.particles[0].emit_rate == 12.5f && file.particles[0].graphics[0].present &&
						file.particles[0].graphics[0].flip_frames == 3 &&
						file.particles[0].graphics[0].blend_mode == opennova::particle::BlendMode::Blend,
				"particle and graphic keys compare case-insensitively; the blend word does not")) return false;
	if (!expect(file.particles[0].unknown_keys.size() == 1 && file.particles[0].unknown_keys[0].first == "Future_Key" &&
						file.particles[0].unknown_keys[0].second == "PreserveMe",
				"unknown keys preserve authored spelling")) return false;
	if (!expect(file.tables.size() == 1 && file.tables[0].id == "MixedTable" && file.tables[0].rows[0][0] == 9 &&
						file.tables[0].rows[0][7] == 9 && file.tables[0].rows[1][0] == 0,
				"TL1 is no row; tl2 is the first")) return false;
	return expect(file.table_handles.size() == 1 && file.table_handles[0].table_id == "MixedTable" &&
						  file.table_handles[0].handlecount == 2 && file.table_handles[0].tightness == 3,
			"edit-handle section and keys compare case-insensitively");
}

bool the_walk_cuts_lines_at_cr_lf_alone() {
	// A line ends only where a CR is followed by an LF [orig: File_ParseASCIIFile @ 0x53D8DE]: a text of LF line
	// ends is one line, its first word "[effectdef]" and the rest of the text the line's other words, so nothing
	// reads.
	const char *text = "[effectdef]\n{\n\tid = lf;\n\tpdefs = a;\n}\n";
	if (!expect(read(text, nullptr, false).effects.empty(), "an LF text reads nothing")) return false;
	if (!expect(read(text).effects.size() == 1, "the same text in CR LF reads its effect")) return false;
	// The last line with no CR LF loses its last byte to the walk's terminator: a brace there is cut away.
	const ParticleFile tail = read("[effectdef]\r\n{\r\n\tpdefs = a;\r\n}", nullptr, false);
	return expect(tail.effects.empty(), "a closing brace with no line end is lost");
}

bool a_line_before_any_section_ends_the_walk() {
	// With no section set the callback answers 1 [orig: @ 0x5ECC3A]: nothing after such a line reads. A reader that
	// read a section before (an earlier file, the same state) goes on in it.
	if (!expect(read("garbage\n[effectdef]\n{\n\tid = e;\n\tpdefs = a;\n}\n").effects.empty(),
				"a word before the first header ends the walk")) return false;
	ParticleReadState state;
	read("[effectdef]\n", &state);
	return expect(read("{\n\tid = carried;\n\tpdefs = a;\n}\n", &state).effects.size() == 1,
			"a file read after an effect section goes on in it");
}

bool the_edithandles_header_ends_the_walk() {
	// [orig: CEffectWorld_ParseSectionCallback @ 0x5ECBEA; File_ParseASCIIFile @ 0x53D942]: the definitions after
	// the first edithandles header never load; the handles themselves are the original editor's, kept.
	const ParticleFile file = read(
			"[effectdef]\n{\n\tid = before;\n\tpdefs = a;\n}\n"
			"[tabledef_edithandles]\n{\n\ttableid = t;\n\thandlecount = 4;\n}\n"
			"[effectdef]\n{\n\tid = after;\n\tpdefs = a;\n}\n");
	return expect(file.effects.size() == 1 && file.effects[0].id == "before" && file.table_handles.size() == 1 &&
						  file.table_handles[0].handlecount == 4,
			"the effect after the handles header does not read");
}

bool the_effect_reader_keeps_its_name_and_builds_at_pdefs() {
	// The name buffer outlives its block; the effect is built at its pdefs line, of the name it holds then, every
	// word of the line a member; one with no pdefs line is none [orig: CParticleTableDef_ParseScriptLine @ 0x5E9339
	// (id), @ 0x5E93CB..0x5E9498 (pdefs), @ 0x5E94E4 (the brace)].
	const ParticleFile file = read(
			"[effectdef]\n{\n\tid = named;\n\tpdefs = a b, c;\n}\n"
			"[effectdef]\n{\n\tpdefs = d;\n\tid = late;\n}\n"
			"[effectdef]\n{\n\tid = idle;\n}\n"
			"[effectdef]\n{\n\tid = empty;\n\tpdefs = ;\n}\n");
	if (!expect(file.effects.size() == 3, "three effects; the one of no pdefs line is none")) return false;
	if (!expect(file.effects[0].id == "named" && file.effects[0].pdefs.size() == 3 && file.effects[0].pdefs[1] == "b",
				"blanks separate members as commas do")) return false;
	if (!expect(file.effects[1].id == "named" && file.effects[1].pdefs.size() == 1,
				"a block whose id follows its pdefs takes the earlier name")) return false;
	return expect(file.effects[2].id == "empty" && file.effects[2].pdefs.size() == 1 && file.effects[2].pdefs[0].empty(),
			"a pdefs line of none reads the tokenizer's empty slot as a member");
}

bool the_particle_reader_reads_in_source_order() {
	using namespace opennova::particle;
	// `id` resets the definition to the game's defaults, losing the lines before it [orig:
	// CParticleSystemDef_InitDefaults @ 0x5E14F0, from @ 0x5EA3DC].
	const ParticleFile defaults = read("[particledef]\n{\n\tspeed = 9;\n\tid = fresh;\n}\n");
	if (!expect(defaults.particles.size() == 1, "the definition reads")) return false;
	const ParticleDef &d = defaults.particles[0];
	if (!expect(d.speed == 0.0f && d.emit_dur == 10.0f && d.emit_rate == 2.0f && d.age == 3.0f && d.scale == 1.0f &&
						d.alpha == 1.0f && d.spread == 10.0f && d.gravity == 50.0f && d.move == move_flag::Normal &&
						d.color1.r == 255 && d.color4.b == 255 && d.orbital_axis.y == 0.0f && d.emit_burst == 1,
				"the game's defaults; the speed before the id is lost")) return false;
	if (!expect(d.graphics[0].present && !d.graphics[1].present && d.graphics[0].scale == 1.0f &&
						d.graphics[0].color2.g == 255 && d.graphics[0].flip_rate == 8 && d.graphics[0].texture.empty(),
				"no graphic line: one layer at the game's defaults")) return false;

	// A graphic line copies the definition as it stands at that line; a key after it reaches no layer; a layer's key
	// goes to the layer the last graphic line took whatever its N; each graphic line sets every layer's flip_frames
	// to 1 [orig: CParticleDefEntry_ParseGraphicProperty @ 0x5E3635..0x5E36EC, @ 0x5E36F0..0x5E3702].
	const ParticleFile order = read(
			"[particledef]\n{\n\tid = ordered;\n\tscale = 2;\n\tscale_func = early;\n"
			"\tgraphic1 = a.tga, additive;\n\tg1_flip_frames = 8;\n\tscale = 3;\n\talpha_func = late;\n"
			"\tgraphic2 = b.tga, blend;\n\tg1_scale = 5;\n}\n");
	const ParticleDef &o = order.particles[0];
	if (!expect(o.graphics[0].scale == 2.0f && o.graphics[0].scale_func.name == "early" &&
						!o.graphics[0].alpha_func.present,
				"layer 1 holds the definition as it stood at graphic1")) return false;
	if (!expect(o.graphics[0].flip_frames == 1, "graphic2 sets layer 1's flip_frames back to 1")) return false;
	if (!expect(o.graphics[1].scale == 5.0f && o.graphics[1].alpha_func.name == "late" && o.scale == 3.0f,
				"g1_scale after graphic2 writes layer 2")) return false;
	if (!expect(o.graphics[0].present && o.graphics[1].present && !o.graphics[2].present,
				"two graphic lines, two layers")) return false;

	// The graphic index outlives the block: a block whose first graphic line is graphic2 takes the layer after the
	// last block's; its count is two (the id's one and this line's), so it draws its first two layers, at the
	// defaults.
	ParticleReadState state;
	read("[particledef]\n{\n\tid = first;\n\tgraphic1 = a.tga, blend;\n\tgraphic2 = b.tga, blend;\n}\n", &state);
	const ParticleFile next = read("[particledef]\n{\n\tid = next;\n\tgraphic2 = c.tga, blend;\n}\n", &state);
	if (!expect(next.particles.size() == 1 && next.particles[0].graphics[2].texture == "c.tga" &&
						next.particles[0].graphics[0].texture.empty() && next.particles[0].graphics[1].present &&
						!next.particles[0].graphics[2].present,
				"graphic2 after a two-layer block writes layer 3; the block draws layers 1 and 2")) return false;

	// The colour channels are the reader's: a colour line of one number keeps green and blue from the last one.
	const ParticleFile colours = read("[particledef]\n{\n\tid = c;\n\tcolor1 = 1, 2, 3;\n\tcolor2 = 300;\n}\n");
	const Color3 &c2 = colours.particles[0].color2;
	if (!expect(c2.r == 44 && c2.g == 2 && c2.b == 3, "color2 = 300: its low byte, the rest carried")) return false;

	// A comma is a separator, never an empty word: the shipped stock particle's `graphic1 =\t, blend;` (stock.ptl)
	// names a texture "blend" [orig: Config_TokenizeLine @ 0x53CB60; CParticleDefEntry_ParseGraphicProperty
	// @ 0x5E35D7, the texture its first word].
	const ParticleFile stock = read("[particledef]\n{\n\tid = stock;\n\tgraphic1 =\t, blend;\n}\n");
	if (!expect(stock.particles.size() == 1 && stock.particles[0].graphics[0].texture == "blend",
				"graphic1 = , blend: the texture is blend")) return false;

	// A value is one word: a name with a blank reads to it, a list stops at its first unknown name, and the flag
	// table's names are found with case [orig: CParticleDefEntry_NextToken @ 0x5DFFB0; FlagTable_ParseFromString @
	// 0x5DF970]. A ';' cuts the line without ending the word it is in, so what follows it rides on the word, the
	// separators stripped out.
	const ParticleFile words = read(
			"[particledef]\n{\n\tid = Buildup dots;\n\tflags = EMITVECTOR junk NEVERAGE ;\n\tchild_id = kid; // c\n}\n"
			"[particledef]\n{\n\tid = lower;\n\tflags = neverage ;\n}\n");
	if (!expect(words.particles.size() == 2 && words.particles[0].id == "Buildup", "a spaced id reads to its blank"))
		return false;
	if (!expect(words.particles[0].flags == particle_flag::EmitVector, "the flag list stops at junk")) return false;
	if (!expect(words.particles[0].child_id == "kid // c", "the comment rides on the word the ';' cut")) return false;
	if (!expect(words.particles[1].flags == 0, "a lowercase flag name sets no bit")) return false;

	// A `}` with no definition open registers nothing (a write through null in retail, @ 0x5EC7F8).
	return expect(read("[particledef]\n{\n\tspeed = 1;\n}\n").particles.empty(), "a block of no id is none");
}

} // namespace

int main() {
	int failures = 0;
	if (!stray_lines_are_skipped()) ++failures;
	if (!mixed_case_tokens_follow_retail_stricmp()) ++failures;
	if (!the_walk_cuts_lines_at_cr_lf_alone()) ++failures;
	if (!a_line_before_any_section_ends_the_walk()) ++failures;
	if (!the_edithandles_header_ends_the_walk()) ++failures;
	if (!the_effect_reader_keeps_its_name_and_builds_at_pdefs()) ++failures;
	if (!the_particle_reader_reads_in_source_order()) ++failures;
	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("particle_lenient_lines: OK\n");
	return 0;
}
