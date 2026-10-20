#pragma once
// A charattr.def authored in the shape JO:CA's takes (its own words and numbers; the keys and the attribute words
// are the loader's): a comment header (a line of it holding an '=') before any section; the live classes
// [CHARACTER1] and [CHARACTER5]..[CHARACTER9], each its eleven number keys and its ATTRIBUTES word, values ending in
// blanks; then, still inside [CHARACTER9], the legacy block of commented-out classes ("//[CHARACTER?]" lines, which
// open no section, and "//KEY = n" entries, a ConfigFile reader's values all the same: it has no "//" comment), one of
// them "//KEY= n" with no blank before the '=' and one whose value is blanks alone; then the earlier classes, a
// second [CHARACTER1], [CHARACTER2]..[CHARACTER4] (which the loader reads, the first sections of their labels) and a
// second [CHARACTER5], each with a PREF_WEAP list whose "//" note runs on (commas and blanks, a hyphened word), one
// ATTRIBUTES of two words and two of NULL. CR LF line ends. The ConfigFile reader counts 278 values and 288 bytes of
// text values in it, a 320-byte pool: JO:CA's charattr.def's counts, base and jox01 alike, as the reader read the
// shipped file while the suite still held it.
#include <initializer_list>
#include <string>
#include <vector>

namespace editor_test {

inline std::string jo_charattr_text() {
	std::vector<std::string> lines = {"// character classes, an authored file in the shape of the base game's",
	                                  "// setchar n picks a class (n = 1-9)", ""};
	const auto add = [&lines](std::initializer_list<const char *> more) {
		for (const char *line : more) lines.push_back(line);
	};
	const auto live = [&](int n, const char *attribute, int run) {
		lines.push_back("//// class " + std::to_string(n) + " ////");
		lines.push_back("[CHARACTER" + std::to_string(n) + "]");
		add({"STEALTH\t\t\t= 20", "HPBONUS\t\t\t= 3\t", "RECOIL_MUTE \t= 0.5\t", "XHAIR_MUTE\t\t= 2.0\t",
		     "XHAIRDX_MUTE\t= 2.0", "SCOPE_MUTE  \t= 0.0\t", "RELOAD_MUTE  \t= 0.75\t", "JUNGLE_CAMMO\t= 5304",
		     "DESERT_CAMMO\t= 5304", "ARCTIC_CAMMO\t= 5304"});
		lines.push_back("RUN_MODIFIER\t= " + std::to_string(run));
		lines.push_back(std::string("ATTRIBUTES  \t= ") + attribute + " ");
		lines.push_back("");
	};
	live(1, "AutoScope", 0);
	live(5, "Medic", 1);
	live(6, "AutoScope", 0);
	live(7, "AutoScope", -1);
	live(8, "KnifeBonus", 0);
	live(9, "KnifeBonus", 0);
	const auto legacy = [&](const char *label, const char *section, bool alt, const char *attribute) {
		lines.push_back(std::string("//// ") + label + " ////");
		lines.push_back(std::string("//[") + section + "]");
		add({"//STEALTH\t\t\t= 20", "//HPBONUS\t\t\t= 3\t", "//RECOIL_MUTE \t= 0.5\t", "//XHAIR_MUTE\t\t= 2.0\t",
		     "//XHAIRDX_MUTE\t= 2.0", "//SCOPE_MUTE  \t= 0.0\t", "//RELOAD_MUTE  \t= 0.75\t", "//JUNGLE_CAMMO\t= 5304",
		     "//DESERT_CAMMO\t= 5304", "//ARCTIC_CAMMO\t= 5304"});
		if (alt) add({"//JUNGLE_CAMMO_ALT= 5307", "//DESERT_CAMMO_ALT= 5307", "//ARCTIC_CAMMO_ALT= 5307"});
		lines.push_back("//RUN_MODIFIER\t= 0");
		lines.push_back(std::string("//ATTRIBUTES  \t= ") + attribute);
		lines.push_back("");
	};
	lines.push_back("//// the earlier classes ////");
	legacy("close quarters", "CHARACTER?", true, "KnifeBonus ");
	legacy("pilot", "CHARACTER?", true, "\t");
	legacy("class 2", "CHARACTER2", false, "AutoScope ");
	legacy("class 3", "CHARACTER3", false, "AutoScope ");
	legacy("class 4", "CHARACTER4", true, "AutoScope ");
	const auto earlier = [&](int n, const char *prefs, const char *note, const char *attribute) {
		lines.push_back("//class " + std::to_string(n) + ", again");
		lines.push_back("[CHARACTER" + std::to_string(n) + "]");
		add({"STRENGTH\t\t= 30.0", "STEALTH\t\t\t= 20", "HPBONUS\t\t\t= 3", "CRAWL_SPEED \t= 2.5\t", "RUN_SPEED\t\t= 1.0\t",
		     "WALK_SPEED\t\t= 1.0\t", "SWIM_SPEED\t\t= 1.25\t", "RECOIL_MUTE \t= 0.5\t", "XHAIR_MUTE\t\t= 2.0\t",
		     "XHAIRDX_MUTE\t= 2.0", "SCOPE_MUTE  \t= 0.5\t", "RELOAD_MUTE  \t= 1.0\t"});
		lines.push_back(std::string("PREF_WEAP\t\t= ") + prefs + "\t// " + note);
		add({"JUNGLE_CAMMO\t= 5300", "DESERT_CAMMO\t= 5300", "ARCTIC_CAMMO\t= 5300"});
		lines.push_back(std::string("ATTRIBUTES  \t= ") + attribute);
		lines.push_back("");
	};
	earlier(1, "1,2,3,4,5,6,7", "scoped rifles", "AutoScope ");
	earlier(2, "8,9,10,11,12,13,14", "belt fed, heavy, light big, guns", "NULL");
	earlier(3, "1,3,5,7,9,11,13,15,17", "handguns, SMG, PDW, carbine, CQB-2, shotguns", "KnifeBonus");
	earlier(4, "2,4,6,8,10", "handgun, SP-11, UWR rifle", "Medic WaterGirl");
	earlier(5, "16,18,20", "GL-6, launcher arms", "NULL");
	lines.push_back("");
	std::string text;
	for (size_t i = 0; i < lines.size(); ++i) text += lines[i] + (i + 1 < lines.size() ? "\r\n" : "");
	return text + "\r\n";
}

} // namespace editor_test
