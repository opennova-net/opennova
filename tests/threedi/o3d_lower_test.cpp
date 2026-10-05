// The `.o3d` lowering to the retail target (formats/threedi/threedi_o3d_lower.h):
// a mesh of any size splits into the strips a 3DI3 holds (21,845 triangles, a
// skinned palette of 16 parts by OED's first-fit), influences past four reduce
// to the primary and the heaviest three, authored tangent frames ride along
// with derived ones beside them, and every name, count, buffer size and
// fixed-point extent the text leaves free is checked at its line
// (docs/threedi/o3d-scene-format.md, "Lowering to retail").
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_o3d_lower.h>
#include <formats/threedi/threedi_o3d_read.h>
#include <runtime/renderer/model_target.h>

using namespace opennova::threedi;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what.c_str());
		++failures;
	}
}

// Build `text` for the retail target: the model's bytes, read back into `m`
// (freed by the caller) when `m` is given.
bool build(const std::string &text, std::vector<SceneFinding> &findings, Threedi3di3 *m = nullptr) {
	std::istringstream in(text);
	std::vector<uint8_t> bytes;
	findings.clear();
	if (!threedi_o3d_build(in, opennova::renderer::retail_model_target(), bytes, findings)) return false;
	return m == nullptr || threedi_3di3_read_memory(bytes.data(), bytes.size(), m) == 0;
}

// Whether a finding (an error when `error`) at `line` (any line when 0) says `words`.
bool said(const std::vector<SceneFinding> &findings, bool error, int line, const std::string &words) {
	for (const SceneFinding &f : findings)
		if (f.error == error && (line == 0 || f.line == line) && f.message.find(words) != std::string::npos) return true;
	return false;
}

std::string dump(const std::vector<SceneFinding> &findings) {
	std::string s;
	for (const SceneFinding &f : findings) s += " [" + std::to_string(f.line) + (f.error ? " error: " : " note: ") + f.message + "]";
	return s;
}

// The refusal `words` at `line` for `text`.
void refuses_at(const std::string &name, const std::string &text, int line, const std::string &words) {
	std::vector<SceneFinding> findings;
	check(!build(text, findings), name + ": refused");
	check(said(findings, true, line, words), name + ": says \"" + words + "\" at line " + std::to_string(line) + dump(findings));
}

std::string num(int v) { return std::to_string(v); }

// A rigid header: one material, one LOD of one part, one mesh open.
const std::string kRigid = "o3d 2\nmodel LOWER\nmaterial FF_ST_OP\ntexture lower.tga\nlod 0\npart 0 0 0 0\nmesh 0\n";

void test_rigid_split() {
	// 21,846 triangles over 1,000 groups of three vertices: a strip holds
	// 21,845 triangles (STRP's u16 index count), so the last one starts
	// another, each strip holding the vertices its triangles use. One vertex
	// no triangle uses goes, and a note says so.
	std::string text = kRigid;
	text.reserve(1024 * 1024);
	const int tris = 21846, groups = 1000;
	for (int g = 0; g < groups; ++g)
		for (int k = 0; k < 3; ++k)
			text += "v " + num(g % 100) + " " + num(g / 100) + " " + num(k) + " 0 0 1 0 0\n";
	text += "v 9 9 9 0 0 1 0 0\n";
	for (int t = 0; t < tris; ++t) {
		const int g = 3 * (t % groups);
		text += "t " + num(g) + " " + num(g + 1) + " " + num(g + 2) + "\n";
	}
	std::vector<SceneFinding> findings;
	Threedi3di3 m{};
	check(build(text, findings, &m), "rigid-split: builds" + dump(findings));
	if (m.lod_count == 1) {
		const ThreediLod &lod = m.lods[0];
		check(lod.strip_count == 2 && lod.strips[0].num_triangles == 21845 && lod.strips[1].num_triangles == 1,
				"rigid-split: 21,845 triangles, then one");
		check(lod.strip_count == 2 && lod.strips[0].num_vertices == 3000 && lod.strips[1].num_vertices == 3,
				"rigid-split: each strip holds the vertices its triangles use");
		check(lod.render_objects[0].num_strips == 2, "rigid-split: both strips the part's");
	}
	threedi_3di3_free(&m);
	check(said(findings, false, 7, "1 vertices no triangle uses are left out: the mesh splits into 2 strips"),
			"rigid-split: the dropped vertex is noted at the mesh" + dump(findings));

	// One strip holds the mesh: its vertices stay as authored, the one no
	// triangle uses included, with no note.
	std::vector<SceneFinding> whole;
	check(build(kRigid + "v 0 0 0 0 0 1 0 0\nv 9 9 9 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 2 3\n", whole, &m) &&
					m.lods[0].strips[0].num_vertices == 4 && m.lods[0].vertices.items[1].position[2] == 9.0f,
			"rigid-whole: the authored vertices, in order" + dump(whole));
	threedi_3di3_free(&m);
	check(whole.empty(), "rigid-whole: nothing noted" + dump(whole));
}

// A skinned model of `parts` parts (part 0 the root) with one mesh open.
std::string skinned_head(int parts) {
	std::string s = "o3d 2\nmodel PALETTE\nskinned 1\nmaterial VS_SKBASIC\nlod 0\n";
	for (int p = 0; p < parts; ++p) s += "part 0 0 0 " + num(p) + "\n";
	return s + "mesh 0\n";
}

// A triangle whose corners ride parts a, b, c wholly, its three vertices new.
std::string rides(int &next, int a, int b, int c) {
	std::string s;
	for (const int p : {a, b, c}) s += "v " + num(next % 7) + " " + num(next / 7) + " " + num(p) + " 0 0 1 0 0 " + num(p) + " 1\n";
	s += "t " + num(next) + " " + num(next + 1) + " " + num(next + 2) + "\n";
	next += 3;
	return s;
}

void test_palette() {
	// Five triangles name parts 0..14. A triangle bringing part 15 on two
	// corners counts it twice (OED's rule: each corner's parts anew where the
	// table lacks them), 15 + 2 past the 16 a palette holds, so it starts a
	// strip (JNTOPSB2's 15 and 2); the next triangle, all of whose parts the
	// first table holds, goes back to it (first fit).
	std::string text = skinned_head(17);
	int next = 0;
	for (int t = 0; t < 5; ++t) text += rides(next, 3 * t, 3 * t + 1, 3 * t + 2);
	text += rides(next, 15, 15, 0);
	text += rides(next, 1, 2, 3);
	std::vector<SceneFinding> findings;
	Threedi3di3 m{};
	check(build(text, findings, &m), "palette-15+2: builds" + dump(findings));
	if (m.lod_count == 1 && m.lods[0].strip_count == 2) {
		const ThreediTriangleStrip &a = m.lods[0].strips[0], &b = m.lods[0].strips[1];
		check(a.bone_table_length == 15 && a.num_triangles == 6, "palette-15+2: the first strip keeps 15 parts, 6 triangles");
		// The table in the order the stored corners (a c b) name the parts.
		check(b.bone_table_length == 2 && b.bone_table[0] == 15 && b.bone_table[1] == 0 && b.num_triangles == 1,
				"palette-15+2: the second strip holds parts 15 and 0");
	} else {
		check(false, "palette-15+2: two strips");
	}
	threedi_3di3_free(&m);

	// Part 15 on one corner fits: 15 + 1.
	text = skinned_head(17);
	next = 0;
	for (int t = 0; t < 5; ++t) text += rides(next, 3 * t, 3 * t + 1, 3 * t + 2);
	text += rides(next, 15, 1, 0);
	check(build(text, findings, &m) && m.lods[0].strip_count == 1 && m.lods[0].strips[0].bone_table_length == 16,
			"palette-16: one strip of 16 parts" + dump(findings));
	threedi_3di3_free(&m);
}

void test_influences() {
	// Six influences: the primary (part 0, the bone a lit shader lights the
	// vertex by, kept whatever its weight) and the three heaviest of the rest
	// (parts 1, 3, 4), in the author's order, renormalized; a note says so.
	std::string text = skinned_head(6);
	text += "v 0 0 0 0 0 1 0 0 0 0.1 1 0.3 2 0.05 3 0.25 4 0.2 5 0.1\n"
			"v 1 0 0 0 0 1 1 0 2 0 1 1\nv 0 1 0 0 0 1 0 1 5 1\nt 0 1 2\n";
	std::vector<SceneFinding> findings;
	Threedi3di3 m{};
	check(build(text, findings, &m), "influences: builds" + dump(findings));
	check(said(findings, false, 12, "1 vertices blend more than 4 influences: each keeps its first"),
			"influences: noted at the mesh" + dump(findings));
	if (m.lod_count == 1 && m.lods[0].strip_count == 1) {
		const ThreediTriangleStrip &st = m.lods[0].strips[0];
		const ThreediVertex &v = m.lods[0].vertices.items[0];
		ThreediSkinInfluence in[4];
		threedi_skin_influences(&v, st.bone_table, st.bone_table_length, in);
		const double total = 0.1 + 0.3 + 0.25 + 0.2;
		check(in[0].part == 0 && in[1].part == 1 && in[2].part == 3 && in[3].part == 4,
				"influences: the primary, then the heaviest three in the author's order");
		check(v.bone_weights[0] == static_cast<float>(0.1 / total) && v.bone_weights[1] == static_cast<float>(0.3 / total) &&
						v.bone_weights[2] == static_cast<float>(0.25 / total),
				"influences: renormalized in double");
		check(std::fabs(in[3].weight - 0.2 / total) < 1e-6, "influences: slot 3 takes the rest");
		// A primary of weight 0 stays the first slot (vertex 1: part 2 at 0).
		const ThreediVertex &zero = m.lods[0].vertices.items[1];
		threedi_skin_influences(&zero, st.bone_table, st.bone_table_length, in);
		check(in[0].part == 2 && zero.bone_weights[0] == 0.0f && in[1].part == 1 && zero.bone_weights[1] == 1.0f,
				"influences: a weightless primary keeps slot 0");
	} else {
		check(false, "influences: one strip");
	}
	threedi_3di3_free(&m);
}

void test_tangents() {
	// A tangent-space shader lays out tangents. The first mesh gives its
	// frames (`vt`, mission axes) and keeps them; the second derives them
	// by the OED rule, as a model without frames does.
	const std::string head = "o3d 2\nmodel TAN\nmaterial VS_PHONGT\nlod 0\npart 0 0 0 0\nmesh 0\n";
	const std::string given = "v 0 0 0 0 0 1 0 0\nvt 0 1 0 0 0 -1\nv 1 0 0 0 0 1 1 0\nvt 0 1 0 0 0 -1\n"
			"v 0 -1 0 0 0 1 0 1\nvt 0 1 0 0 0 -1\nt 0 1 2\n";
	const std::string derived = "mesh 0\nv 0 0 1 0 0 1 0 0\nv 1 0 1 0 0 1 1 0\nv 0 -1 1 0 0 1 0 1\nt 0 1 2\n";
	std::vector<SceneFinding> findings;
	Threedi3di3 m{}, plain{};
	check(build(head + given + derived, findings, &m), "tangents: builds" + dump(findings));
	check(build(head + derived.substr(7), findings, &plain), "tangents: the derived mesh alone builds" + dump(findings));
	if (m.lod_count == 1 && m.lods[0].vertices.count == 6 && plain.lod_count == 1) {
		const ThreediVertex &v = m.lods[0].vertices.items[0];
		// Mission (0 1 0) in model axes (-y, z, x) is (-1 0 0); (0 0 -1) is (0 -1 0).
		check(v.tangent[0] == -1.0f && v.tangent[1] == 0.0f && v.tangent[2] == 0.0f && v.bitangent[1] == -1.0f,
				"tangents: the given frame, in model axes");
		const ThreediVertex &d = m.lods[0].vertices.items[3];
		const ThreediVertex &e = plain.lods[0].vertices.items[0];
		check(std::memcmp(d.tangent, e.tangent, sizeof(d.tangent)) == 0 &&
						std::memcmp(d.bitangent, e.bitangent, sizeof(d.bitangent)) == 0 && d.tangent[2] > 0.999f,
				"tangents: the mesh without frames derives them by the OED rule");
	} else {
		check(false, "tangents: six vertices");
	}
	threedi_3di3_free(&m);
	threedi_3di3_free(&plain);
	// Every vertex of a mesh or none.
	refuses_at("tangents-some", head + "v 0 0 0 0 0 1 0 0\nvt 0 1 0 0 0 -1\nv 1 0 0 0 0 1 1 0\nv 0 -1 0 0 0 1 0 1\nt 0 1 2\n", 6,
			"the mesh gives 'vt' on 1 of its 3 vertices");
	refuses_at("tangents-twice", head + "v 0 0 0 0 0 1 0 0\nvt 0 1 0 0 0 -1\nvt 0 1 0 0 0 -1\n", 9, "a vt follows its own v");
	// No layout reads them: built, noted.
	check(build(kRigid + given, findings) && said(findings, false, 9, "the tangent frames ('vt') go unused"),
			"tangents-unused: noted" + dump(findings));
}

void test_limits() {
	// Each name, count and extent the text leaves free, past the retail
	// target's limit, refused at its line.
	refuses_at("model-name", "o3d 2\nmodel SIXTEEN_CHARS_XY\nlod 0\n", 2, "the model name 'SIXTEEN_CHARS_XY' is 16 characters");
	refuses_at("lod-type", "o3d 2\nmodel L\nlod 0 gnrcx\n", 3, "the LOD type 'gnrcx' is 5 characters");
	refuses_at("lod-threshold", "o3d 2\nmodel L\nlod 40000\n", 3, "the LOD threshold is 40000");
	refuses_at("shader", "o3d 2\nmodel L\nmaterial SHADER_TAG_OF_THIRTY_THREE_BYTES!\nlod 0\n", 3, "is 33 characters");
	refuses_at("register", "o3d 2\nmodel L\nregister TWENTY_FIVE_CHARACTERS_XX\nlod 0\n", 3, "is 25 characters");
	refuses_at("texture-name", "o3d 2\nmodel L\nmaterial FF_ST_OP\ntexture seventeen_chars.t\nlod 0\n", 4,
			"texture name 'seventeen_chars.t' is 17 bytes");
	{
		std::string rows = "o3d 2\nmodel L\nmaterial FF_ST_OP\n";
		for (int i = 0; i < 25; ++i) rows += "texture t" + num(i) + ".tga\n";
		refuses_at("texture-rows", rows + "lod 0\n", 28, "the material's texture row 25");
	}
	refuses_at("flipbook", "o3d 2\nmodel L\nmaterial FF_ST_OP\ntexanim 256 0 1\nlod 0\n", 4, "the flipbook's 256 frames");
	{
		std::string parts = "o3d 2\nmodel L\nlod 0\n";
		for (int i = 0; i < 256; ++i) parts += "part 0 0 0 0\n";
		refuses_at("parts", parts, 259, "the LOD holds 256 parts");
	}
	refuses_at("user-point-name", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\nuserpoint SIXTEEN_CHARS_XY 0 0 0 0 0 1 0\n", 5,
			"the user point name 'SIXTEEN_CHARS_XY' is 16 characters");
	refuses_at("user-point-extent", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\nuserpoint far 40000 0 0 0 0 1 0\n", 5,
			"the user point's position is 40000: a 16.16 word");
	refuses_at("section-offset", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\ncobj 0 0 0 -33000\n", 5,
			"collision section 0's offset is -33000");
	refuses_at("collision-extent", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\ncobj 0\ncv 0 128 0\n", 6,
			"the collision vertex's coordinate is 128");
	refuses_at("volume-box", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\ncobj 0\ncvol 1 0 0 0 0 1 1 40000\n", 6,
			"the volume's box is 40000");
	refuses_at("cxlt", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\ncobj 0\ncxlt 0 0 50000\n", 6, "the CXLT row is 50000");
	refuses_at("light-part", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\nlight 0 0 0 0 0 4 0 0 0 1 1 1 1 1 1 0\n"
			"light 256 0 0 0 0 4 0 0 0 1 1 1 1 1 1 0\n", 6, "names part 256");
	refuses_at("light-rate", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\nlight 0 0 0 0 0 4 0 300 0 1 1 1 1 1 1 0\n", 5,
			"the light's rate is 300");
	refuses_at("light-cone", "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\nlight 0 0 0 0 0 4 0 0 0 1 1 1 1 1 1 0x48 0 0 -1 300\n", 5,
			"the light's cone half-angle is 300");
	{
		std::string occ = "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\nocc 0 0 0\n";
		for (int i = 0; i < 129; ++i) occ += "ov " + num(i) + " 0 0\n";
		refuses_at("occlusion-vertices", occ, 5, "the occlusion record holds 129 vertices");
		occ = "o3d 2\nmodel L\nlod 0\npart 0 0 0 0\nocc 0 0 0\nov 0 0 0\n";
		for (int i = 0; i < 33; ++i) occ += "op 0 0 1 " + num(i) + "\n";
		refuses_at("occlusion-planes", occ, 5, "the occlusion record holds 33 planes");
	}
	// A render vertex 32,768 m out overflows GHDR's 16.16 radius.
	refuses_at("model-radius", kRigid + "v 40000 0 0 0 0 1 0 0\nv 0 1 0 0 0 1 0 1\nv 0 0 1 0 0 1 0 1\nt 0 1 2\n", 8,
			"the vertex's distance from the origin is 40000");
	// The version-1 text fails on its first line, saying so.
	refuses_at("version-1", "o3d 1\nmodel L\nlod 0\n", 1, "this is version 1 of the scene text");
}

void test_lod_buffers() {
	// A LOD's vertices and indices go whole into one buffer of the game's GPU
	// pools: 2 MiB of vertices (40 bytes a rigid one, 64 with tangents), 512
	// KiB of u16 indices. The buffer bounds the LOD, not a u16: 52,428 rigid
	// vertices fit, one more does not.
	const auto lod_of = [](const std::string &shader, int vertices) {
		std::string text = "o3d 2\nmodel BIG\nmaterial " + shader + "\nlod 0\npart 0 0 0 0\nmesh 0\n";
		text.reserve(static_cast<size_t>(vertices) * 32);
		for (int i = 0; i < vertices; ++i) text += "v " + num(i % 200) + " " + num(i / 200) + " 0 0 0 1 0 0\n";
		return text;
	};
	std::vector<SceneFinding> findings;
	check(build(lod_of("FF_ST_OP", 52428), findings), "lod-vertex-bytes: 2,097,120 bytes build" + dump(findings));
	refuses_at("lod-vertex-bytes", lod_of("FF_ST_OP", 52429), 4,
			"the LOD's vertex buffer takes 2,097,160 bytes (52,429 vertices of 40): the game uploads it whole into one "
			"GPU pool buffer (at most 2,097,152)");
	check(build(lod_of("VS_PHONGT", 32768), findings), "lod-vertex-bytes: 32,768 tangent vertices build" + dump(findings));
	refuses_at("lod-vertex-bytes-tangents", lod_of("VS_PHONGT", 32769), 4, "(32,769 vertices of 64)");
	// 87,381 triangles hold 262,143 indices; one more triangle passes 512 KiB.
	std::string tris = "o3d 2\nmodel IDX\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nmesh 0\n"
			"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\n";
	tris.reserve(87382 * 8 + tris.size());
	for (int t = 0; t < 87381; ++t) tris += "t 0 1 2\n";
	check(build(tris, findings), "lod-index-bytes: 524,286 bytes build" + dump(findings));
	refuses_at("lod-index-bytes", tris + "t 0 1 2\n", 4, "the LOD's index buffer takes 524,292 bytes (262,146 indices)");
	// Eight LODs fill the loader's tables; a ninth is refused at its line.
	std::string lods = "o3d 2\nmodel LODS\n";
	for (int i = 0; i < 8; ++i) lods += "lod 0\n";
	check(build(lods, findings), "lods: eight build" + dump(findings));
	refuses_at("lods", lods + "lod 0\n", 11, "the model holds 9 LODs: the loader's LOD tables hold that many (at most 8)");
	// No count bounds the materials (one allocation of MTRL's count).
	std::string mats = "o3d 2\nmodel MATS\n";
	for (int i = 0; i < 64; ++i) mats += "material FF_ST_OP\n";
	check(build(mats + "lod 0\n", findings) && !said(findings, false, 0, "materials"),
			"materials: 64 build with no note" + dump(findings));
}

void test_parse() {
	// The parse keeps the text's terms: a vertex's pairs as given (no limit,
	// no reorder), names of any length; the reader knows no target.
	const std::string text = "o3d 2\nmodel A_MODEL_NAME_OF_ANY_LENGTH\nskinned 1\nmaterial VS_SKBASIC\nlod 0\n"
			"part 0 0 0 0\npart 0 0 0 1\npart 1 0 0 2\npart 2 0 0 3\npart 3 0 0 4\npart 4 0 0 5\nmesh 0\n"
			"v 0 0 0 0 0 1 0 0 5 0.1 4 0.1 3 0.2 2 0.2 1 0.2 0 0.2\nv 1 0 0 0 0 1 1 0 1 1\nt 0 1 0\n";
	ThreediO3dModel parsed;
	std::vector<SceneFinding> findings;
	std::istringstream in(text);
	check(!threedi_o3d_read(in, parsed, findings) && said(findings, true, 15, "t needs three vertices") == false &&
					said(findings, true, 15, "t repeats a vertex"),
			"parse: a triangle repeating a vertex" + dump(findings));
	findings.clear();
	std::istringstream good(text.substr(0, text.size() - 8) + "t 0 1 1\n");
	ThreediO3dModel again;
	check(!threedi_o3d_read(good, again, findings), "parse: still refused (vertex 1 twice)");
	findings.clear();
	std::istringstream fine(text.substr(0, text.size() - 8) + "v 0 1 0 0 0 1 0 1 0 1\nt 0 1 2\n");
	check(threedi_o3d_read(fine, again, findings) && findings.empty() &&
					again.name.name == "A_MODEL_NAME_OF_ANY_LENGTH" &&
					again.lods[0].parts[5].meshes[0].vertices[0].influence_count == 6 &&
					again.lods[0].parts[5].meshes[0].influences[0].part == 5,
			"parse: six pairs, the primary first, a long name" + dump(findings));
}

} // namespace

int main() {
	test_rigid_split();
	test_palette();
	test_influences();
	test_tangents();
	test_limits();
	test_lod_buffers();
	test_parse();
	std::printf("o3d_lower_test: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
