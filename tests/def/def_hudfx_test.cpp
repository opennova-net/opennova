// hudfx.def (formats/def/def_hudfx.h; docs/interface/hud-re.md "hudfx.def"): the one line the HUD's init walk
// takes, the first tagged one (a tag any case, its model the second token, a line opening '/' or of no token
// skipped, the walk ending at it), nothing after it in the model; the names its slots hold (a long name running
// into the next slots, the last slot's run cleared); the writer's form of a minted file byte for byte, read back
// as itself; an authored file (a comment, odd spacing, a tag's case, a line of no tag, a second tagged line)
// written from its one line and read back as it; a model no one token holds refused. JO ships no hudfx.def.
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
	// The walk ends at the first tagged line: the model holds it alone.
	const HudFxFile file = parsed("// hud models\r\nfoo bar\r\n3DIPower2\tpow2.3di\r\n/3DIHud skipped.3di\r\n3DIHud hud.3di\r\n");
	TEST_EXPECT(file.line && file.line->slot == 2 && file.line->model == "pow2.3di");
	auto names = hudfx_slot_names(file);
	TEST_EXPECT(names[2] == "pow2.3di" && names[0].empty() && names[3].empty());
	// A file of no tagged line holds none, and its slots no name.
	TEST_EXPECT(!parsed("// nothing\r\nfoo bar\r\n").line && hudfx_slot_names(parsed("")).at(0).empty());
	// A name of 16 or more characters runs into the slots after its own.
	names = hudfx_slot_names(parsed("3DIHud abcdefghijklmnopqrstu.3di\r\n"));
	TEST_EXPECT(names[0] == "abcdefghijklmnopqrstu.3di" && names[1] == "qrstu.3di" && names[2].empty());
	// The last slot's run goes past the nine, into what the init clears.
	names = hudfx_slot_names(parsed("3DIPower8 abcdefghijklmnopqrstu.3di\r\n"));
	TEST_EXPECT(names[8] == "abcdefghijklmnop" && names[7].empty());
	std::printf("tags: the nine tags any case; the first tagged line the one held; a long name's run\n");
	return 0;
}

int written() {
	HudFxFile minted;
	minted.line = HudFxLine{3, "p3.3di"};
	std::string text, error;
	TEST_EXPECT(hudfx_write(minted, text, error) && text == "3DIPower3\tp3.3di\r\n");
	HudFxFile again = parsed(text);
	TEST_EXPECT(again.line && again.line->slot == 3 && again.line->model == "p3.3di");
	// A line of no model is its tag alone, read back so.
	HudFxFile bare;
	bare.line = HudFxLine{5, ""};
	TEST_EXPECT(hudfx_write(bare, text, error) && text == "3DIPower5\r\n");
	again = parsed(text);
	TEST_EXPECT(again.line && again.line->slot == 5 && again.line->model.empty());
	// A file of no line is no text.
	TEST_EXPECT(hudfx_write(HudFxFile{}, text, error) && text.empty());
	minted.line->model = "two words";
	TEST_EXPECT(!hudfx_write(minted, text, error) && error.find("two words") != std::string::npos);

	const std::string file = "// the HUD's models\r\n3dihud   hud.3di   // the frame\r\nnot a tag\r\n\r\n3DIPower1 p1.3di\r\n";
	// An authored file: written from its one line in the writer's form, read back as it.
	const HudFxFile authored = parsed(file);
	TEST_EXPECT(authored.line && hudfx_write(authored, text, error));
	TEST_EXPECT(text == "3DIHud\thud.3di\r\n");
	again = parsed(text);
	TEST_EXPECT(again.line && again.line->slot == 0 && again.line->model == "hud.3di");
	std::printf("written: a minted file in the writer's form; an authored one from its line, read back as it\n");
	return 0;
}

} // namespace

int main() {
	if (tags() || written()) return 1;
	return 0;
}
