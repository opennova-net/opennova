// hudpos.def's lines as the game's parser is handed them (formats/def/def_hudpos_text.h): the comment
// none, each key's line where it starts; the line the game takes a key from, the last [orig:
// HUD_ParseHudposToken @ 0x59F370, the StaticFrame copy @ 0x5a0a4e..0x5a0a8a], a HUDSTANCE by its id.
#include <cstdio>
#include <string>
#include <vector>

#include <formats/def/def_hudpos_text.h>

#include "common/test_expect.h"

using namespace opennova::def;

namespace {

// A soldier panel's layout, every line CR LF as the game's reader ends them.
const char *const kLayout =
		"// The soldier panel\r\n"
		"fonthud1_hi\tonhudb18.fnt\r\n"
		"fonthud1_lo\tonhud14.fnt\r\n"
		"StaticFrame\told.tga\t0,0\r\n"
		"StaticFrame\tonhframe.tga\t6,586\r\n"
		"AMMOCOUNTPOS\t168,616,0,right\r\n"
		"HUDWEAPONNAME\t14,588,0,left\r\n"
		"HUDSTANCEPOS\t30,639\r\n"
		"HUDSTANCE 0\t0 0 onhstnc0.tga STAND\r\n"
		"HUDSTANCE 1\t0 0 onhstnc1.tga CROUCH\r\n"
		"HUDHEALTH\t25,741,177,751\r\n"
		"HUDCLIP\t14,648\r\n";

int test_lines() {
	// Its lines as the parser is handed them: the comment none, each key's line where it starts.
	const std::string text = kLayout;
	const std::vector<HudLayoutLine> lines = hud_layout_lines(text);
	TEST_EXPECT(lines.size() == 11 && lines[0].key == "fonthud1_hi" && lines[0].first == "onhudb18.fnt");
	TEST_EXPECT(text.compare(lines[0].offset, lines[0].length, "fonthud1_hi\tonhudb18.fnt") == 0);
	// The game takes a key's last line (two StaticFrames: the second), a HUDSTANCE by its id.
	const HudLayoutLine *frame = hud_layout_line(lines, "STATICFRAME");
	TEST_EXPECT(frame && frame->first == "onhframe.tga" && text.compare(frame->offset, 11, "StaticFrame") == 0);
	const HudLayoutLine *crouch = hud_layout_line(lines, "hudstance", "1");
	TEST_EXPECT(crouch && text.compare(crouch->offset, crouch->length, "HUDSTANCE 1\t0 0 onhstnc1.tga CROUCH") == 0);
	TEST_EXPECT(hud_layout_line(lines, "HUDSTANCE", "1.0") == crouch);
	TEST_EXPECT(hud_layout_line(lines, "HUDSTANCE", "4") == nullptr && hud_layout_line(lines, "HUDHEAT") == nullptr);
	// A line cut at LF alone, and the text's last line without a line end.
	const std::vector<HudLayoutLine> loose = hud_layout_lines("HUDHEALTH 25,741,177,751\nHUDCLIP 14,648");
	TEST_EXPECT(loose.size() == 2 && loose[1].key == "HUDCLIP" && loose[1].offset == 25 && loose[1].length == 14);
	std::printf("hudpos lines: the comment none, a key's last line taken, a stance by its id\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_lines();
	if (failures == 0) std::printf("def_hudpos_text: all passed\n");
	return failures == 0 ? 0 : 1;
}
