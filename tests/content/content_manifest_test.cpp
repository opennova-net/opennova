// The content store's manifest (base/content/content_manifest.h, ADR 0069): the paths a project's
// file may have and the ones refused (each Windows rule, the machine's own folder, broken UTF-8);
// two paths one file on Windows and macOS; the canonical text sorted by path, read back to the same
// files, and every other text of them refused, so a manifest's SHA-256 is its files' one name.

#include <base/content/content_manifest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::content;

namespace {

const std::string kShaA(64, 'a'), kShaB(64, 'b'), kShaC(64, 'c');

bool path_ok(const std::string &path) {
	std::string why;
	return is_content_path(path, why);
}

// Refused, with a reason.
bool path_refused(const std::string &path) {
	std::string why;
	if (is_content_path(path, why)) {
		std::fprintf(stderr, "\"%s\" was taken\n", path.c_str());
		return false;
	}
	return !why.empty();
}

} // namespace

static int test_paths() {
	TEST_EXPECT(path_ok("weapon.def"));
	TEST_EXPECT(path_ok("art/on_pist/on_pist.blend"));
	TEST_EXPECT(path_ok("art/oncrate1_src.png.import"));
	TEST_EXPECT(path_ok(".gitkeep")); // a dot file of the project's own
	TEST_EXPECT(path_ok("missions/m\xc3\xa9t\xc3\xa9o.bms")); // UTF-8
	TEST_EXPECT(path_ok("consoles/con1.txt") && path_ok("com10.txt") && path_ok("lpt.txt"));
	TEST_EXPECT(path_ok(std::string(255, 'x')));

	TEST_EXPECT(path_refused(""));
	TEST_EXPECT(path_refused("/rooted.def") && path_refused("trailing/") && path_refused("a//b"));
	TEST_EXPECT(path_refused("./a") && path_refused("a/../b") && path_refused(".."));
	TEST_EXPECT(path_refused("a\\b") && path_refused("c:/x") && path_refused("a|b") &&
			path_refused("what?"));
	TEST_EXPECT(path_refused("a\tb") && path_refused(std::string("a\0b", 3)));
	TEST_EXPECT(path_refused("dir./x") && path_refused("name "));
	TEST_EXPECT(path_refused("CON") && path_refused("art/nul.txt") && path_refused("Com1.def") &&
			path_refused("lpt9"));
	TEST_EXPECT(path_refused(".opennova/local.json") && path_refused(".OpenNova/build/x"));
	TEST_EXPECT(path_ok("art/.opennova/x")); // only the project's own top folder is the machine's
	TEST_EXPECT(path_refused("bad\xff.def") && path_refused("\xc0\xaf") &&
			path_refused("\xed\xa0\x80"));
	// UTF-8 alone: each length's sequence, and an overlong form, a surrogate, a cut sequence and
	// a code point past U+10FFFF refused.
	TEST_EXPECT(is_utf8("") && is_utf8("ascii") && is_utf8("\xc3\xa9") && is_utf8("\xe2\x82\xac") &&
			is_utf8("\xf0\x9f\x8e\xae"));
	TEST_EXPECT(!is_utf8("\xc0\xaf") && !is_utf8("\xed\xa0\x80") && !is_utf8("\xe2\x82") &&
			!is_utf8("\xf4\x90\x80\x80") && !is_utf8("\x80"));
	TEST_EXPECT(path_refused(std::string(256, 'x')) && path_refused(std::string(1025, 'x')));
	std::printf("paths: the Windows rules, the machine's own folder and UTF-8\n");
	return 0;
}

static int test_files() {
	std::string why;
	TEST_EXPECT(check_files({ { "a.def", kShaA, 1 }, { "b.def", kShaB, 2 } }, why));
	TEST_EXPECT(!check_files({ { "Art/A.tga", kShaA, 1 }, { "art/a.tga", kShaB, 2 } }, why) &&
			why.find("one file on Windows") != std::string::npos);
	TEST_EXPECT(!check_files({ { "a.def", kShaA, 1 }, { "a.def", kShaA, 1 } }, why) &&
			why.find("listed twice") != std::string::npos);
	TEST_EXPECT(!check_files({ { "a.def", "ABC", 1 } }, why));
	TEST_EXPECT(!check_files({ { "a.def", std::string(64, 'A'), 1 } }, why)); // upper case
	TEST_EXPECT(!check_files({ { "a.def", kShaA, kMaxFileBytes + 1 } }, why));
	// One SHA-256 at one size: the same bytes under two paths, never two sizes.
	TEST_EXPECT(check_files({ { "a.def", kShaA, 5 }, { "b.def", kShaA, 5 } }, why));
	TEST_EXPECT(!check_files({ { "a.def", kShaA, 5 }, { "b.def", kShaA, 999 } }, why) &&
			why.find("another size") != std::string::npos);
	// A file is no folder of another path's, folded or not.
	TEST_EXPECT(!check_files({ { "maps", kShaA, 1 }, { "maps/a.bms", kShaB, 2 } }, why) &&
			why.find("is a file, and a folder of") != std::string::npos);
	TEST_EXPECT(!check_files({ { "Maps", kShaA, 1 }, { "maps/x/a.bms", kShaB, 2 } }, why));
	TEST_EXPECT(check_files({ { "maps.txt", kShaA, 1 }, { "maps/a.bms", kShaB, 2 } }, why));
	TEST_EXPECT(total_bytes({ { "a", kShaA, 3 }, { "b", kShaB, 4 } }) == 7);
	std::printf("files: one file per path once folded, a SHA-256 each, 4 GiB at most\n");
	return 0;
}

static int test_canonical_form() {
	const std::vector<ContentFile> files = { { "weapon.def", kShaB, 8650 },
		{ "art/a.png", kShaA, 12 }, { "Zed.txt", kShaC, 0 } };
	const std::string text = encode_manifest(files);
	// Sorted by the paths' bytes: upper case before lower.
	TEST_EXPECT(text.find("Zed.txt") < text.find("art/a.png") &&
			text.find("art/a.png") < text.find("weapon.def"));
	TEST_EXPECT(encode_manifest({ files[2], files[0], files[1] }) == text);
	std::vector<ContentFile> read;
	std::string error;
	TEST_EXPECT(decode_manifest(text, read, error));
	TEST_EXPECT(read.size() == 3 && read[0].path == "Zed.txt" && read[2].size == 8650 &&
			read[1].sha256 == kShaA);
	TEST_EXPECT(manifest_sha256(text).size() == 64 &&
			manifest_sha256(text) == manifest_sha256(encode_manifest(read)));
	TEST_EXPECT(decode_manifest(encode_manifest({}), read, error) && read.empty());

	// The same files in any other text: refused.
	std::string spaced = text;
	spaced.insert(spaced.find('\n'), " ");
	TEST_EXPECT(
			!decode_manifest(spaced, read, error) && error.find("canonical") != std::string::npos);
	// json_write's layout to the byte, its two files out of order.
	const std::string unsorted =
			"{\n  \"files\": [\n    {\n      \"path\": \"b\",\n      \"sha256\": \"" + kShaA +
			"\",\n      \"size\": 1\n    },\n    {\n      \"path\": \"a\",\n      \"sha256\": \"" +
			kShaA + "\",\n      \"size\": 1\n    }\n  ],\n  \"schema_version\": 1\n}\n";
	TEST_EXPECT(!decode_manifest(unsorted, read, error) &&
			error.find("canonical") != std::string::npos);

	// Not a manifest.
	TEST_EXPECT(!decode_manifest("[]", read, error));
	TEST_EXPECT(!decode_manifest("{\"files\": [], \"schema_version\": 2}", read, error));
	TEST_EXPECT(
			!decode_manifest("{\"files\": [], \"schema_version\": 1, \"extra\": 0}", read, error));
	TEST_EXPECT(!decode_manifest("{\"files\": [{\"path\": \"a\", \"sha256\": \"x\", \"size\": 1}], "
								 "\"schema_version\": 1}",
			read, error));
	TEST_EXPECT(!decode_manifest(
			"{\"files\": [{\"path\": \"a\", \"size\": 1}], \"schema_version\": 1}", read, error));
	TEST_EXPECT(!decode_manifest(
			encode_manifest({ { ".opennova/local.json", kShaA, 1 } }), read, error));
	TEST_EXPECT(!decode_manifest("not json", read, error));
	std::printf("canonical form: sorted by path, read back whole, every other text refused\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_paths();
	failures += test_files();
	failures += test_canonical_form();
	if (failures == 0)
		std::printf("content_manifest: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
