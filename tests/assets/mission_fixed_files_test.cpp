// The fixed names a running mission opens (runtime/assets/mission_fixed_files.h): each set the runtime
// opens, by the runtime's own constants, every name once as an archive keys it; the HUD's table one name
// a slot; the sets the port does not open stay out.
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include <formats/pff/pff.h>
#include <runtime/assets/mission_fixed_files.h>
#include <runtime/hud/hud_texture_names.h>

#include "common/test_expect.h"

using opennova::assets::MissionFixedFile;
using opennova::assets::mission_fixed_files;
// The name as an archive keys it (upper case, trailing spaces trimmed).
using opennova::pff::normalized_logical_name;

static int test_mission_fixed_files() {
	namespace hud = opennova::hud;
	std::map<std::string, std::string> what;
	for (const MissionFixedFile &file : mission_fixed_files())
		TEST_EXPECT(what.emplace(normalized_logical_name(file.name), file.what).second && !file.what.empty());
	const auto has = [&what](const std::string &name) { return what.count(normalized_logical_name(name)) == 1; };
	std::set<int32_t> slots;
	for (const hud::HudFixedTexture &texture : hud::kHudFixedTextures)
		TEST_EXPECT(slots.insert(texture.slot).second && has(texture.name) &&
		            std::string(hud::hud_fixed_texture_name(texture.slot)) == texture.name);
	TEST_EXPECT(!hud::hud_fixed_texture_name(hud::kHudTexCrosshair) && !hud::hud_fixed_texture_name(hud::kHudTexFrame));
	for (int style = hud::kHudCrosshairStyleMin; style <= hud::kHudCrosshairStyleMax; ++style)
		TEST_EXPECT(has(hud::hud_crosshair_texture_name(style)));
	TEST_EXPECT(hud::hud_crosshair_texture_name(0) == "cross01.tga" && hud::hud_crosshair_texture_name(24) == "cross25.tga");
	for (const char *name : {"compring.tga", "TSDicon.tga", "WPIndctr.tga", "dmgslice.tga", "JO_LFP.tga", "dirguide.tga",
	                         "border.tga", "neticon2.tga", "k_tip.tga", "H_flag.tga", "H_docmnt.tga", "Binoculr.tga",
	                         "BNumbers.tga", "NVGScale.tga", "vignette.tga", "eraindrp.tga", "jsnwflk.tga", "smoktest.pcx",
	                         "wake5.tga", "wakegrad.tga", "scorch1.tga", "scorch4.tga", "bhole1.tga", "trscrch1.tga",
	                         "qburn01.tga", "overcast.def", "helo1.aip", "H_BHawkN.aip", "default.adm", "DltB086C.wav"})
		TEST_EXPECT(has(name));
	// The sets the port does not open stay out: the MFD, the glass and the 3rd-person models.
	for (const char *name : {"MFD1.PCX", "brkglsa.tga", "scopexh.tga", "comacent.tga"}) TEST_EXPECT(!has(name));
	return 0;
}

int main() {
	const int failures = test_mission_fixed_files();
	if (failures == 0) std::printf("mission_fixed_files_test OK\n");
	return failures;
}
