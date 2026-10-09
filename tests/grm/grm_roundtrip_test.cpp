// AUTHORED: fixtures/grm/person.grm is a four-vertex facial texture mesh
// written for these tests, with no retail bytes. Its layout follows the
// witnessed writer @0x588320. Canonical roundtrip compares the complete bytes.
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

	// Unsafe retail memory accesses fail transactionally (D-GRM-1).
	second.base_texture = "unchanged";
	for (const std::string bad : {
			"vertices -1\r\n", "vertices 1\r\nvertex 1 0 0 xxx\r\n",
			"vertices 1\r\nvertex -1 0 0 xxx\r\n", "gestures 1\r\ngesture 2 BAD\r\n",
			"gestures 1\r\nparameters 33\r\n", "parm 0 1 2 mouth\r\n",
			"triangles 1\r\ntri 0 0 1 2\r\n",
			"vertices 1\r\nvertex 0 nan 0 xxx\r\n"}) {
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
	return 0;
}
