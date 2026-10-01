// A menu screen's fonts and textures through a file source
// (engine/runtime/menu/menu_frame_assets.h): each FONT NAME after %VAR% loads its .fnt and
// only a font that loaded is registered, so a widget whose FONT did not load draws with
// its ancestor's [orig: CWnd_GetFontAndColors @ 0x646a70]; textures load by retail's
// dispatch, a missing .tga through its .dds [orig: CTextureManager_LoadOrFindTexture
// @ 0x654980]; what one configure loaded is found again by (file, stamp) with no read,
// a moved stamp reloads that file alone, and what two configures in a row did not use is
// let go; a name that did not load is told apart by whether the source has its file; what a
// configure would read is known before it, and what is loaded ahead of it is found again (S13 V6).

#include <runtime/menu/menu_frame_assets.h>

#include <formats/fnt/fnt.h>
#include <formats/mnu/mnu.h>

#include "common/test_expect.h"
#include "menu/fake_file_source.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using opennova::menu::MenuDependency;
using opennova::menu::MenuFrameAssets;
using opennova::menu::MenuFrameCompiler;
using opennova::menu::MenuTextureFormat;

// A texture file is two bytes: its width and height.
std::vector<uint8_t> texture(uint8_t width, uint8_t height) { return {width, height}; }

// The decoder a headless check would be: the size, and which keys are held.
class SizeDecoder : public opennova::menu::MenuTextureDecoder {
public:
	bool decode(const std::string &key, MenuTextureFormat format, const std::vector<uint8_t> &bytes, int &width,
	            int &height) override {
		++decodes;
		formats[key] = format;
		if (bytes.size() != 2) return false;
		width = bytes[0];
		height = bytes[1];
		held.insert(key);
		return true;
	}
	void release(const std::string &key) override { held.erase(key); }
	int decodes = 0;
	std::set<std::string> held;
	std::map<std::string, MenuTextureFormat> formats;
};

std::vector<uint8_t> font_bytes() {
	opennova::fnt::fnt_font_t font{};
	opennova::fnt::fnt_init_blank(&font, 1, 2);
	std::vector<uint8_t> bytes(opennova::fnt::fnt_calculate_file_size(1));
	size_t written = 0;
	opennova::fnt::fnt_write(&font, bytes.data(), bytes.size(), &written);
	opennova::fnt::fnt_free(&font);
	bytes.resize(written);
	return bytes;
}

// ROOT names its font through a variable and draws logo.tga; CHILD names a font that is
// not there (it draws with ROOT's); OTHER draws fallback.tga, which only a .dds provides.
const char *const kScreen =
        "<SCREEN><NAME>S</NAME>"
        "<WINDOW type=\"window\" name=\"ROOT\">"
        "<APPEARANCE type=\"image\" state=\"default\">logo.tga</APPEARANCE>"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP></POSITION>"
        "<FONT><NAME>%F%</NAME><DEFAULT_FG>FF102030</DEFAULT_FG></FONT>"
        "<WINDOW type=\"static\" name=\"CHILD\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT></POSITION>"
        "<FONT><NAME>nope.fnt</NAME><DEFAULT_FG>FFFFFFFF</DEFAULT_FG></FONT><STRING>Hi</STRING></WINDOW>"
        "<WINDOW type=\"static\" name=\"OTHER\">"
        "<APPEARANCE type=\"image\" state=\"default\">fallback.tga</APPEARANCE>"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP></POSITION></WINDOW>"
        "</WINDOW></SCREEN>";

// Every way a name fails: a font, a texture and a table the files have that do not parse or
// decode, a texture of an extension the dispatch reads nothing for, and a font, a texture
// and a table the files lack.
const char *const kFailures =
        "<SCREEN><NAME>F</NAME>"
        "<WINDOW type=\"window\" name=\"ROOT\"><TEXT_RSRC>broken.bin</TEXT_RSRC>"
        "<APPEARANCE type=\"image\" state=\"default\">bad.tga</APPEARANCE>"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP></POSITION>"
        "<FONT><NAME>bad.fnt</NAME><DEFAULT_FG>FFFFFFFF</DEFAULT_FG></FONT>"
        "<WINDOW type=\"static\" name=\"A\"><TEXT_RSRC>absent.bin</TEXT_RSRC>"
        "<APPEARANCE type=\"image\" state=\"default\">art.bmp</APPEARANCE>"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP></POSITION>"
        "<FONT><NAME>gone.fnt</NAME><DEFAULT_FG>FFFFFFFF</DEFAULT_FG></FONT></WINDOW>"
        "<WINDOW type=\"static\" name=\"B\">"
        "<APPEARANCE type=\"image\" state=\"default\">gone.tga</APPEARANCE>"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP></POSITION></WINDOW>"
        "</WINDOW></SCREEN>";

const char *const kNoTextures =
        "<SCREEN><NAME>BARE</NAME><WINDOW type=\"window\" name=\"ROOT\">"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW></SCREEN>";

bool parse(const char *xml, opennova::mnu::Document &doc) {
	std::string error;
	return opennova::mnu::parse(xml, doc, error) && !doc.screens.empty();
}

void files_for(FakeFileSource &files) {
	files.put("main.fnt", font_bytes(), 1);
	files.put("logo.tga", texture(64, 32), 1);
	files.put("fallback.dds", texture(8, 4), 1);
}

bool has_dependency(const std::vector<MenuDependency> &dependencies, const std::string &name, uint64_t stamp) {
	return std::any_of(dependencies.begin(), dependencies.end(),
	                   [&](const MenuDependency &d) { return d.name == name && d.stamp == stamp; });
}

} // namespace

static int test_loads_and_no_default_font() {
	opennova::mnu::Document doc;
	TEST_EXPECT(parse(kScreen, doc));
	FakeFileSource files;
	files_for(files);
	SizeDecoder decoder;
	MenuFrameCompiler compiler;
	const std::map<std::string, std::string> vars = {{"F", "main.fnt"}};
	MenuFrameAssets assets;
	TEST_EXPECT(assets.configure(compiler, &doc, &doc.screens[0], files, decoder, vars) == 1);
	TEST_EXPECT(assets.unresolved() == std::vector<std::string>{"nope.fnt"});
	TEST_EXPECT(assets.unreadable().empty() && assets.unreadable_tables().empty());
	TEST_EXPECT(files.reads["main.fnt"] == 1 && files.reads["logo.tga"] == 1 && files.reads["fallback.dds"] == 1);
	TEST_EXPECT(decoder.decodes == 2);
	TEST_EXPECT(decoder.formats.size() == 2);
	TEST_EXPECT(has_dependency(assets.dependencies(), "fallback.tga", 0));
	TEST_EXPECT(has_dependency(assets.dependencies(), "fallback.dds", 1));
	TEST_EXPECT(has_dependency(assets.dependencies(), "nope.fnt", 0));
	// Every texture slot loaded and measured: OTHER is texture-sized from the .dds.
	for (const std::string &key : assets.slot_keys()) TEST_EXPECT(!key.empty());
	opennova::mnu::RectEdges other{};
	opennova::menu::MenuFrameState state;
	TEST_EXPECT(compiler.widget_rect(2, state, &other) && other.right - other.left == 8 && other.bottom - other.top == 4);
	// No default font: CHILD's FONT did not load, so it draws with ROOT's font and colours.
	std::string font;
	uint32_t colors[4] = {};
	TEST_EXPECT(compiler.widget_font(1, &font, colors) && font == "main.fnt" && colors[0] == 0xFF102030u);
	TEST_EXPECT(assets.font_for_slot(compiler, 1) != nullptr && assets.font_for_slot(compiler, 0) == nullptr);

	// A screen whose only FONT does not load draws no text at all.
	FakeFileSource empty;
	TEST_EXPECT(assets.configure(compiler, &doc, &doc.screens[0], empty, decoder, vars) == 4);
	TEST_EXPECT(!compiler.widget_font(1, nullptr, nullptr) && !compiler.widget_font(0, nullptr, nullptr));
	std::printf("test_loads_and_no_default_font passed\n");
	return 0;
}

static int test_kept_by_stamp_and_swept() {
	opennova::mnu::Document doc, bare;
	TEST_EXPECT(parse(kScreen, doc) && parse(kNoTextures, bare));
	FakeFileSource files;
	files_for(files);
	SizeDecoder decoder;
	MenuFrameCompiler compiler;
	const std::map<std::string, std::string> vars = {{"F", "main.fnt"}};
	MenuFrameAssets assets;
	assets.configure(compiler, &doc, &doc.screens[0], files, decoder, vars);
	const uint64_t serial = assets.font_for_slot(compiler, 1)->serial;

	// Unchanged stamps: a second configure reads and decodes nothing.
	files.reads.clear();
	decoder.decodes = 0;
	assets.configure(compiler, &doc, &doc.screens[0], files, decoder, vars);
	TEST_EXPECT(files.total_reads() == 0 && decoder.decodes == 0);
	TEST_EXPECT(assets.font_for_slot(compiler, 1)->serial == serial);

	// A moved stamp reloads that file alone; its old load stays one more configure.
	files.put("logo.tga", texture(16, 16), 2);
	assets.configure(compiler, &doc, &doc.screens[0], files, decoder, vars);
	TEST_EXPECT(files.reads["logo.tga"] == 1 && files.total_reads() == 1 && decoder.decodes == 1);
	TEST_EXPECT(decoder.held.size() == 3 && assets.kept_textures() == 3);
	assets.configure(compiler, &doc, &doc.screens[0], files, decoder, vars);
	TEST_EXPECT(decoder.held.size() == 2 && assets.kept_textures() == 2); // two configures without it

	// A screen that uses none of them: the loads stay one configure, then go.
	assets.configure(compiler, &bare, &bare.screens[0], files, decoder, vars);
	TEST_EXPECT(decoder.held.size() == 2 && assets.font_alive(serial));
	assets.configure(compiler, &bare, &bare.screens[0], files, decoder, vars);
	TEST_EXPECT(decoder.held.empty() && assets.kept_textures() == 0 && assets.kept_fonts() == 0);
	TEST_EXPECT(!assets.font_alive(serial));

	// clear() lets everything go and configures the compiler over nothing.
	assets.configure(compiler, &doc, &doc.screens[0], files, decoder, vars);
	TEST_EXPECT(!decoder.held.empty() && compiler.widget_count() > 0);
	assets.clear(compiler, decoder);
	TEST_EXPECT(decoder.held.empty() && assets.kept_fonts() == 0 && compiler.widget_count() == 0);

	// A texture a marquee asks about loads (and is kept) like the screen's.
	TEST_EXPECT(assets.texture_loads("logo.tga", files, decoder) && !assets.texture_loads("none.tga", files, decoder));
	std::printf("test_kept_by_stamp_and_swept passed\n");
	return 0;
}

static int test_absent_or_unreadable() {
	opennova::mnu::Document doc;
	TEST_EXPECT(parse(kFailures, doc));
	FakeFileSource files;
	files.put_text("bad.fnt", "not a font", 1);
	files.put("bad.tga", {1, 2, 3}, 1); // SizeDecoder takes two bytes only
	files.put("art.bmp", texture(4, 4), 1);
	files.put_text("broken.bin", "not a table", 1);
	SizeDecoder decoder;
	MenuFrameCompiler compiler;
	MenuFrameAssets assets;
	TEST_EXPECT(assets.configure(compiler, &doc, &doc.screens[0], files, decoder, {}) == 5);
	auto sorted = [](std::vector<std::string> names) {
		std::sort(names.begin(), names.end());
		return names;
	};
	TEST_EXPECT(sorted(assets.unresolved()) ==
	            (std::vector<std::string>{"art.bmp", "bad.fnt", "bad.tga", "gone.fnt", "gone.tga"}));
	TEST_EXPECT(sorted(assets.unreadable()) == (std::vector<std::string>{"art.bmp", "bad.fnt", "bad.tga"}));
	TEST_EXPECT(assets.missing_tables() == std::vector<std::string>{"absent.bin"});
	TEST_EXPECT(assets.unreadable_tables() == std::vector<std::string>{"broken.bin"});
	// The extension the dispatch reads nothing for is a dependency: were it replaced, the
	// holder configures again.
	TEST_EXPECT(has_dependency(assets.dependencies(), "art.bmp", 1));

	// Kept as they are, a second configure reads nothing and says the same.
	files.reads.clear();
	assets.configure(compiler, &doc, &doc.screens[0], files, decoder, {});
	TEST_EXPECT(files.total_reads() == 0);
	TEST_EXPECT(sorted(assets.unreadable()) == (std::vector<std::string>{"art.bmp", "bad.fnt", "bad.tga"}));
	TEST_EXPECT(assets.unreadable_tables() == std::vector<std::string>{"broken.bin"});

	// clear() forgets them.
	assets.clear(compiler, decoder);
	TEST_EXPECT(assets.unresolved().empty() && assets.unreadable().empty() && assets.unreadable_tables().empty());
	std::printf("test_absent_or_unreadable passed\n");
	return 0;
}

// S13 V6: what a configure would decode is known before it (texture_kept), and what is loaded
// ahead of it (texture_loads) is found again by it, so the editor's menu device spreads a screen's
// first configure over its steps. Nothing to decode (a file the source lacks, an extension the
// dispatch reads nothing for) is kept; a moved stamp is not.
static int test_loaded_ahead() {
	opennova::mnu::Document doc;
	TEST_EXPECT(parse(kScreen, doc));
	FakeFileSource files;
	files_for(files);
	files.put("art.bmp", texture(4, 4), 1);
	SizeDecoder decoder;
	MenuFrameCompiler compiler;
	const std::map<std::string, std::string> vars = {{"F", "main.fnt"}};
	MenuFrameAssets assets;
	TEST_EXPECT(!assets.texture_kept("logo.tga", files) && !assets.texture_kept("fallback.tga", files));
	TEST_EXPECT(assets.texture_kept("none.tga", files) && assets.texture_kept("art.bmp", files) &&
	            assets.texture_kept("", files));
	TEST_EXPECT(files.total_reads() == 0 && decoder.decodes == 0);

	// Loaded ahead, one at a time: each kept, then the configure reads and decodes none of them
	// (it reads its font alone).
	TEST_EXPECT(assets.texture_loads("logo.tga", files, decoder) && assets.texture_kept("logo.tga", files));
	TEST_EXPECT(assets.texture_loads("fallback.tga", files, decoder) && assets.texture_kept("fallback.tga", files));
	TEST_EXPECT(decoder.decodes == 2 && files.total_reads() == 2);
	files.reads.clear();
	decoder.decodes = 0;
	TEST_EXPECT(assets.configure(compiler, &doc, &doc.screens[0], files, decoder, vars) == 1);
	TEST_EXPECT(decoder.decodes == 0 && files.total_reads() == 1 && files.reads["main.fnt"] == 1);
	for (const std::string &key : assets.slot_keys()) TEST_EXPECT(!key.empty());

	// A moved stamp: no longer kept, until loaded again.
	files.put("logo.tga", texture(16, 16), 2);
	TEST_EXPECT(!assets.texture_kept("logo.tga", files) && assets.texture_kept("fallback.tga", files));
	TEST_EXPECT(assets.texture_loads("logo.tga", files, decoder) && assets.texture_kept("logo.tga", files));
	std::printf("test_loaded_ahead passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_loads_and_no_default_font();
	failures += test_kept_by_stamp_and_swept();
	failures += test_absent_or_unreadable();
	failures += test_loaded_ahead();
	return failures == 0 ? 0 : 1;
}
