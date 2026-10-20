// AUTHORED: fixtures/grm/person.grm is a four-vertex facial texture mesh
// written for these tests, with no retail bytes. Its layout follows the
// witnessed writer @0x588320. Canonical roundtrip compares the complete bytes.
// MINTED: fixtures/grm/face108.grm (tests/fixtures/minimal_grm_gen.cpp) is a
// face at the scale of the ones Black Hawk Down ships (JO ships none; BHD's
// loader is JO's, docs/world/world-wac-ai-re.md, the BHD faces), whose 13 faces
// this row parsed and wrote back byte for byte until 2026-10-19: Delta01's
// counts and its tool's absolute texture paths, pinned below.
#include <formats/grm/grm.h>
#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <string>
#include <vector>

using namespace opennova;

namespace {

bool parse_text(const std::string &text, grm::File &file, std::string &error) {
	return grm::parse(reinterpret_cast<const uint8_t *>(text.data()), text.size(), file, error);
}

// The BHD-scale face parses and writes back byte for byte; Delta01's counts pin the parse.
int bhd_scale_face() {
	const std::vector<uint8_t> bytes = test_io::read_file(test_paths_fixture("grm/face108.grm"));
	grm::File file;
	std::string error;
	if (!grm::parse(bytes.data(), bytes.size(), file, error)) {
		std::fprintf(stderr, "face108.grm: %s\n", error.c_str());
		return 1;
	}
	std::vector<uint8_t> encoded;
	TEST_EXPECT(grm::write(file, encoded, error));
	TEST_EXPECT(encoded == bytes);
	TEST_EXPECT(file.vertices.size() == 108 && file.triangles.size() == 143);
	TEST_EXPECT(file.gestures.size() == 9 && file.gestures[0].name == "Sad");
	TEST_EXPECT(file.gestures[0].parameters.size() == 22);
	TEST_EXPECT(file.saved.author == "OpenNova" && file.saved.year == 2002);
	// The tool's absolute texture paths: the game opens the bare name.
	TEST_EXPECT(file.base_texture == "C:\\faces\\face01.tga");
	TEST_EXPECT(grm::texture_load_name(file.base_texture, grm::kTextureExtension) == "face01.TGA");
	std::printf("grm_roundtrip: the BHD-scale face rewrites byte for byte\n");
	return 0;
}

} // namespace

int main() {
	const auto path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/grm/person.grm";
	const std::vector<uint8_t> bytes = test_io::read_file(path);
	TEST_EXPECT(!bytes.empty());
	grm::File file;
	std::string error;
	TEST_EXPECT(grm::parse(bytes.data(), bytes.size(), file, error));
	TEST_EXPECT(error.empty());
	TEST_EXPECT(file.base_texture == "face.tga");
	TEST_EXPECT(file.eye_textures[0] == "iris.tga");
	TEST_EXPECT(file.eye_textures[1] == "iris_alpha.tga");
	TEST_EXPECT(file.saved.year == 2026 && file.saved.minute == 5);
	TEST_EXPECT(file.saved.author == "OpenNova");
	TEST_EXPECT(file.vertices.size() == 4 && file.triangles.size() == 2);
	TEST_EXPECT(file.vertices[1].group == "brow" && file.triangles[1][2] == 3);
	TEST_EXPECT(file.gestures.size() == 3);
	TEST_EXPECT(file.gestures[1].parameters[1].offset.y == -0.2f);
	TEST_EXPECT(file.eye_centers[1].x == 0.65f);
	std::vector<uint8_t> encoded;
	TEST_EXPECT(grm::write(file, encoded, error));
	TEST_EXPECT(encoded == bytes);
	grm::File second;
	TEST_EXPECT(grm::parse(encoded.data(), encoded.size(), second, error));
	std::vector<uint8_t> again;
	TEST_EXPECT(grm::write(second, again, error));
	TEST_EXPECT(again == encoded);

	// Rebuild after a semantic edit; the writer does not echo input bytes.
	file.vertices[1].uv.x = 0.75f;
	file.gestures[1].parameters[0].offset.y = 0.25f;
	TEST_EXPECT(grm::write(file, encoded, error));
	TEST_EXPECT(encoded != bytes);
	TEST_EXPECT(grm::parse(encoded.data(), encoded.size(), second, error));
	TEST_EXPECT(second.vertices[1].uv.x == 0.75f);
	TEST_EXPECT(second.gestures[1].parameters[0].offset.y == 0.25f);

	// Quoted comma/space names, decimal-prefix numbers, mixed-case keys,
	// ignored structural keywords/comments and the original CRLF boundary.
	TEST_EXPECT(parse_text("/ ignored\r\nvertices,1trailing\r\nVeRtEx 0 .25 .5 \"lower lip\"\r\n"
			"eyes\r\neyesize .2 .3\r\nunknown anything\r\n", second, error));
	TEST_EXPECT(second.vertices.size() == 1 && second.vertices[0].group == "lower lip");
	TEST_EXPECT(second.eye_size.x == 0.2f && second.eye_size.y == 0.3f);
	TEST_EXPECT(second.eye_limits.x == 0.03f);
	TEST_EXPECT(parse_text("basetexture face.tga", second, error));
	TEST_EXPECT(second.base_texture == "face.tg"); // unterminated final-byte drop
	TEST_EXPECT(parse_text("basetexture face.tga\neyesize .2 .3\n", second, error));
	TEST_EXPECT(second.eye_size.x == 0.04f); // LF alone does not split lines
	TEST_EXPECT(second.base_texture == "face.tga\neyesize");
	// The coordinates are the CRT atof's [orig: FaceAnimConfig_ParseProperty @ 0x5886A0 -> _atof]:
	// the locale's white space before the number (0xA0 on cp1252), a d exponent, and no NaN
	// spelling, so "nan" reads 0.0 and is no non-finite coordinate (D-NET-389).
	TEST_EXPECT(parse_text("vertices 1\r\nvertex 0 nan \xA0" "0.5 xxx\r\neyesize 1d1 0\r\n", second, error));
	TEST_EXPECT(second.vertices.size() == 1 && second.vertices[0].uv.x == 0.0f && second.vertices[0].uv.y == 0.5f);
	TEST_EXPECT(second.eye_size.x == 10.0f);

	// Unsafe retail memory accesses fail transactionally (D-GRM-1).
	second.base_texture = "unchanged";
	for (const std::string bad : {
			"vertices -1\r\n", "vertices 1\r\nvertex 1 0 0 xxx\r\n",
			"vertices 1\r\nvertex -1 0 0 xxx\r\n", "gestures 1\r\ngesture 2 BAD\r\n",
			"gestures 1\r\nparameters 33\r\n", "parm 0 1 2 mouth\r\n",
			"triangles 1\r\ntri 0 0 1 2\r\n",
			"vertices 1\r\nvertex 0 1e999 0 xxx\r\n"}) {
		TEST_EXPECT(!parse_text(bad, second, error));
		TEST_EXPECT(!error.empty());
		TEST_EXPECT(second.base_texture == "unchanged");
	}

	// The texture files the game opens for the names written: path stripped, the extension from the last '.'
	// made .TGA (the base's twin .MDT) [orig: Shadow_DecalLoadTextures @ 0x588040].
	TEST_EXPECT(grm::texture_load_name("face.bmp", grm::kTextureExtension) == "face.TGA");
	TEST_EXPECT(grm::texture_load_name("face.bmp", grm::kTextureTwinExtension) == "face.MDT");
	TEST_EXPECT(grm::texture_load_name("chars\\faces/old.face.tga", grm::kTextureExtension) == "old.face.TGA");
	TEST_EXPECT(grm::texture_load_name("eye1", grm::kTextureExtension) == "eye1.TGA");
	// A person's model's face: the model's name made .GRM.
	TEST_EXPECT(grm::face_file_name("chars\\soldier.3di") == "soldier.GRM" && grm::face_file_name("Boonie") == "Boonie.GRM");
	return bhd_scale_face();
}
