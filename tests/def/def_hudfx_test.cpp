// hudfx.def (formats/def/def_hudfx.h; docs/interface/hud-re.md "hudfx.def"): the tagged lines the HUD's init
// walk would take (a tag any case, its model the second token, a line opening '/' or of no token skipped), the
// one the game reads (the first: its callback ends the walk), the names its slots hold (a long name running into
// the next slots, the last slot's run cleared); the writer's form of a minted file; a file read with its layout
// and written again as it was (its comment, its spacing, a line of no tag); one model changed its one line; a
// line added after the last, the lines swapped; a model no one token holds refused. JO ships no hudfx.def.
#include <formats/def/def_hudfx.h>

#include "common/test_expect.h"

#include <cstdio>
#include <string>

using namespace opennova;
using namespace opennova::def;

namespace {

HudFxFile parsed(const std::string &text) { return hudfx_parse(reinterpret_cast<const uint8_t *>(text.data()), text.size()); }

int tags() {
	TEST_EXPECT(hudfx_slot_of("3dihud") == 0 && hudfx_slot_of("3DIPOWER8") == 8 && hudfx_slot_of("3DIPower9") == -1);
	TEST_EXPECT(std::string(hudfx_tag(0)) == "3DIHud" && std::string(hudfx_tag(5)) == "3DIPower5" && !hudfx_tag(9));
	const HudFxFile file = parsed("// hud models\r\nfoo bar\r\n3DIPower2\tpow2.3di\r\n/3DIHud skipped.3di\r\n3DIHud hud.3di\r\n");
	TEST_EXPECT(file.lines.size() == 2 && file.lines[0].slot == 2 && file.lines[0].model == "pow2.3di" &&
	            file.lines[1].slot == 0 && file.lines[1].model == "hud.3di");
	// The game reads the first tagged line alone.
	auto names = hudfx_slot_names(file);
	TEST_EXPECT(names[2] == "pow2.3di" && names[0].empty() && names[3].empty());
	// A name of 16 or more characters runs into the slots after its own.
	names = hudfx_slot_names(parsed("3DIHud abcdefghijklmnopqrstu.3di\r\n"));
	TEST_EXPECT(names[0] == "abcdefghijklmnopqrstu.3di" && names[1] == "qrstu.3di" && names[2].empty());
	// The last slot's run goes past the nine, into what the init clears.
	names = hudfx_slot_names(parsed("3DIPower8 abcdefghijklmnopqrstu.3di\r\n"));
	TEST_EXPECT(names[8] == "abcdefghijklmnop" && names[7].empty());
	std::printf("tags: the nine tags any case; the first tagged line the one read; a long name's run\n");
	return 0;
}

int written() {
	HudFxFile minted;
	minted.lines.push_back({0, "hud.3di", 0});
	minted.lines.push_back({3, "p3.3di", 0});
	std::string text, error;
	TEST_EXPECT(hudfx_write(minted, nullptr, text, error) && text == "3DIHud\thud.3di\r\n3DIPower3\tp3.3di\r\n");
	HudFxFile again = parsed(text);
	TEST_EXPECT(again.lines.size() == 2 && again.lines[1].slot == 3 && again.lines[1].model == "p3.3di");
	minted.lines[0].model = "two words";
	TEST_EXPECT(!hudfx_write(minted, nullptr, text, error) && error.find("two words") != std::string::npos);

	const std::string file = "// the HUD's models\r\n3dihud   hud.3di   // the frame\r\nnot a tag\r\n\r\n3DIPower1 p1.3di\r\n";
	textlayout::Notes notes;
	HudFxFile noted = hudfx_parse(reinterpret_cast<const uint8_t *>(file.data()), file.size(), notes);
	bool rewritten = true;
	TEST_EXPECT(hudfx_write(noted, &notes, text, error, &rewritten) && !rewritten && text == file);
	noted.lines[0].model = "frame2.3di";
	TEST_EXPECT(hudfx_write(noted, &notes, text, error, &rewritten) && !rewritten);
	TEST_EXPECT(text == "// the HUD's models\r\n3dihud   frame2.3di   // the frame\r\nnot a tag\r\n\r\n3DIPower1 p1.3di\r\n");
	noted.lines[0].model = "hud.3di";
	noted.lines.push_back({4, "p4.3di", 0});
	TEST_EXPECT(hudfx_write(noted, &notes, text, error, &rewritten) && !rewritten && text == file + "3DIPower4\tp4.3di\r\n");
	noted.lines.pop_back();
	std::swap(noted.lines[0], noted.lines[1]);
	TEST_EXPECT(hudfx_write(noted, &notes, text, error, &rewritten) && !rewritten);
	again = parsed(text);
	TEST_EXPECT(again.lines.size() == 2 && again.lines[0].slot == 1 && again.lines[1].slot == 0);
	std::printf("written: a minted file in the writer's form; an authored one again as it was, a model, a line "
	            "added and a swap\n");
	return 0;
}

} // namespace

int main() {
	if (tags() || written()) return 1;
	return 0;
}
