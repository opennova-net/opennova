// base/vfs/file_stamps.h: the files something made from a FileSource read, each once (a name
// compared without case) with its stamp then; a move found among them, or among all but some;
// the stamps taken again; and StampedFiles noting every read and stamp through it.
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/file_stamps.h>

#include "common/test_expect.h"

using namespace opennova;

namespace {

// Files by name (without case), each with its bytes and a stamp the test moves.
class FakeFiles : public FileSource {
public:
	struct Entry {
		std::vector<uint8_t> bytes;
		uint64_t stamp = 0;
	};
	std::map<std::string, Entry> files; // lower-cased names

	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files.find(strutil::to_lower(name));
		if (found == files.end()) return false;
		out = found->second.bytes;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = files.find(strutil::to_lower(name));
		return found == files.end() ? 0 : found->second.stamp;
	}
};

} // namespace

static int test_note_add_and_moves() {
	FakeFiles files;
	files.files["menu.css"] = {{1}, 10};
	files.files["font.fnt"] = {{2}, 20};
	FileStamps stamps;
	TEST_EXPECT(stamps.empty());
	TEST_EXPECT(stamps.note("Menu.CSS", 10));
	TEST_EXPECT(!stamps.note("menu.css", 99)); // a name read before keeps its first stamp
	TEST_EXPECT(stamps.note("font.fnt", 20));
	TEST_EXPECT(stamps.files().size() == 2 && stamps.files()[0].name == "Menu.CSS" && stamps.files()[0].stamp == 10);
	TEST_EXPECT(!stamps.moved(files));

	FileStamps other;
	other.note("FONT.FNT", 5);
	TEST_EXPECT(!stamps.add(other) && stamps.files().size() == 2);
	other.note("gone.tga", 0);
	TEST_EXPECT(stamps.add(other) && stamps.files().size() == 3 && stamps.files()[2].name == "gone.tga");
	TEST_EXPECT(!stamps.moved(files)); // a name that does not resolve stays at 0

	files.files["menu.css"].stamp = 11;
	TEST_EXPECT(stamps.moved(files));
	TEST_EXPECT(!stamps.moved_but(files, {"MENU.css"}));
	TEST_EXPECT(stamps.moved_but(files, {"font.fnt"}));
	stamps.restamp(files);
	TEST_EXPECT(!stamps.moved(files) && stamps.files()[0].stamp == 11);
	files.files["gone.tga"] = {{3}, 30};
	TEST_EXPECT(stamps.moved(files));

	stamps.clear();
	TEST_EXPECT(stamps.empty() && !stamps.moved(files));
	return 0;
}

static int test_stamped_files() {
	auto files = std::make_shared<FakeFiles>();
	files->files["model.3di"] = {{7, 8}, 3};
	StampedFiles stamped(files);
	std::vector<uint8_t> out;
	TEST_EXPECT(stamped.read("MODEL.3DI", out) && out == std::vector<uint8_t>({7, 8}));
	TEST_EXPECT(!stamped.read("missing.tga", out));
	TEST_EXPECT(stamped.stamp("other.bad") == 0 && stamped.stamp("model.3di") == 3);
	const std::vector<FileStamp> &read = stamped.stamps().files();
	TEST_EXPECT(read.size() == 3 && read[0].name == "MODEL.3DI" && read[0].stamp == 3 && read[1].name == "missing.tga" &&
	            read[1].stamp == 0 && read[2].name == "other.bad");

	StampedFiles empty(nullptr);
	TEST_EXPECT(!empty.read("a", out) && empty.stamp("b") == 0 && empty.stamps().files().size() == 2);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_note_add_and_moves();
	failures += test_stamped_files();
	if (failures == 0) std::printf("file_stamps: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
