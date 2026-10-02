// opennova-3di end to end over paths the Windows ANSI code page cannot hold:
// a model under a folder `Jose` spelled with U+00E9 (which the Western code page
// holds, as a single byte) and under the Japanese word for model,
// each beside a texture it names and a file named in katakana it does not.
// `build` and `scene` must take the paths as arguments (the exe's manifest
// opts into the UTF-8 code page), the unrelated file must not stop `scene`,
// and the `texfile` path the scene text names must be the texture's, in UTF-8
// (the importer reads the text as UTF-8). This test embeds the same manifest,
// so its own std::system call hands the exe the UTF-8 command line.
//
//   o3d_unicode_test <opennova-3di> <scratch dir>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>

#include "../common/run_command.h"

namespace {

using test_cmd::quoted;
using test_cmd::run;

int failures = 0;

void check(bool ok, const std::string &what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what.c_str());
		++failures;
	}
}

const char kScene[] =
		"o3d 1\nmodel UNICODE\nmaterial FF_ST_OP\ntexture skin.tga\nlod 0\npart 0 0 0 0\nstrip 0\n"
		"v 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n";

// UTF-8 names spelled as bytes, so the source stays ASCII whatever code page
// the compiler reads it in: "Jos" U+00E9, the katakana "moderu" (model) and
// "tekusucha.png" (texture).
std::string bytes(std::initializer_list<int> list) {
	std::string s;
	for (int b : list) s += static_cast<char>(b);
	return s;
}

const std::string kJose = "Jos" + bytes({0xC3, 0xA9});
const std::string kModel = bytes({0xE3, 0x83, 0xA2, 0xE3, 0x83, 0x87, 0xE3, 0x83, 0xAB});
const std::string kKatakanaTexture =
		bytes({0xE3, 0x83, 0x86, 0xE3, 0x82, 0xAF, 0xE3, 0x82, 0xB9, 0xE3, 0x83, 0x81, 0xE3, 0x83, 0xA3}) + ".png";

// `name` is UTF-8.
void run_in(const std::string &cli, const std::filesystem::path &scratch, const std::string &name) {
	const std::filesystem::path folder = scratch / std::filesystem::u8path(name);
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	std::ofstream(folder / "in.o3d", std::ios::binary) << kScene;
	// The texture the model's diffuse row names loads its .dds sibling (the
	// row's loader); the other file's name only UTF-8 can spell.
	std::ofstream(folder / "skin.dds", std::ios::binary) << "x";
	std::ofstream(folder / std::filesystem::u8path(kKatakanaTexture), std::ios::binary) << "x";
	const std::string base = (scratch.u8string() + "/" + name + "/");
	check(run(cli + " build " + quoted(base + "in.o3d") + " -o " + quoted(base + "model.3di")) == 0,
			name + ": build takes the path");
	check(run(cli + " scene " + quoted(base + "model.3di") + " -o " + quoted(base + "out.o3d")) == 0,
			name + ": scene takes the path beside a file the ANSI code page cannot spell");
	std::ifstream in(folder / "out.o3d", std::ios::binary);
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const std::string record = "texfile skin.tga 0 ";
	const size_t at = text.find(record);
	const size_t end = at == std::string::npos ? at : text.find_first_of("\r\n", at);
	const std::string got =
			at == std::string::npos ? std::string() : text.substr(at + record.size(), end - at - record.size());
	std::error_code ec;
	check(!got.empty() && std::filesystem::equivalent(std::filesystem::u8path(got), folder / "skin.dds", ec),
			name + ": texfile names the texture in UTF-8 (got '" + got + "')");
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: o3d_unicode_test <opennova-3di> <scratch dir>\n");
		return 2;
	}
	const std::string cli = quoted(argv[1]);
	const std::filesystem::path scratch = std::filesystem::path(argv[2]) / "o3d-unicode";
	run_in(cli, scratch, kJose);
	run_in(cli, scratch, kModel);
	std::printf("o3d_unicode_test: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
