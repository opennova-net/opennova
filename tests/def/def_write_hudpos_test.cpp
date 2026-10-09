// hudpos.def's writer (formats/def/def_hudpos_write.h, ADR 0003): a layout read, written and read again is
// the same model, and the text it writes is canonical (written again from its own read it is the same
// bytes, every line ending CR LF as the game's reader ends a line). The synthetic legs author every key the
// writer writes (a quoted name, a fractional ALPHAFADE, the HUDLS block, the stances, the declutter rows,
// two static frames, the icons, vehicle blocks) beside lines the model holds nothing of; the retail legs
// round-trip JO:CA's own hudpos.def, out of the packed install (base and each expansion) and the extracted
// tree.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <formats/def/def.h>
#include <formats/def/def_hudpos_write.h>

#include "common/retail_paths.h"

using namespace opennova::def;

namespace {

int failures = 0;

#define CHECK(cond)                                                                    \
	do {                                                                               \
		if (!(cond)) {                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
			++failures;                                                                \
		}                                                                              \
	} while (0)

struct Parsed {
	DefHudPosFile file{};
	bool ok = false;
	explicit Parsed(const std::string &text) {
		ok = def_parse_hudpos_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &file) == 0;
	}
	~Parsed() { def_free_hudpos(&file); }
	Parsed(const Parsed &) = delete;
	Parsed &operator=(const Parsed &) = delete;
};

// Every line ends CR LF, no CR or LF stands alone, and no line is empty.
bool canonical_line_ends(const std::string &text) {
	if (text.size() < 2 || text.compare(text.size() - 2, 2, "\r\n") != 0) return false;
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')) return false;
		if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) return false;
		if (text[i] == '\n' && i + 2 < text.size() && text[i + 1] == '\r') return false;
	}
	return true;
}

// Read, write, read: the same model; written again from the second read: the same bytes. Returns the text.
std::string round_trip(const std::string &source, const char *what) {
	Parsed first(source);
	CHECK(first.ok);
	const DefWriteResult written = def_write_hudpos(first.file);
	if (!written.ok()) std::printf("FAIL %s: %s\n", what, written.diagnostics[0].message.c_str());
	CHECK(written.ok());
	Parsed second(written.text);
	CHECK(second.ok);
	std::string difference;
	const bool same = def_hudpos_same(first.file.hud, second.file.hud, &difference);
	if (!same) std::printf("FAIL %s: the model read back differs at %s\n", what, difference.c_str());
	CHECK(same);
	const DefWriteResult again = def_write_hudpos(second.file);
	CHECK(again.ok() && again.text == written.text);
	CHECK(canonical_line_ends(written.text));
	return written.text;
}

const char *const kEveryKey =
		"// every key the writer writes\r\n"
		"fonthud1_hi\tonhudb18.fnt\r\n"
		"fonthud1_lo\t\"my font.fnt\"\r\n"
		"MRCLIPPYNORMAL 8,120,64,108\r\n"
		"MRCLIPPYALTERNATE 8,340,64,108\r\n"
		"HUDHEALTH 25,741,177,751\r\n"
		"HUDHEAT 172,592,176,662\r\n"
		"HUDPOWERBAR 14,576,160,6\r\n"
		"STARTTIMER 262,180,500,80\r\n"
		"HUDHEALTHBORDER 90,200,201,202\r\n"
		"HUDHEATBORDER 160,200,206,190\r\n"
		"hud_textcolor 246,202,70\r\n"
		"weapon_textcolor 246,202,70,128\r\n"
		"tagcolor_blueteam 70,120,255\r\n"
		"tagcolor_redteam 235,60,50\r\n"
		"tagcolor_good 92,214,72\r\n"
		"tagcolor_middle 236,176,40\r\n"
		"tagcolor_bad 210,48,36\r\n"
		"stanceicon_color 0,255,255,255\r\n"
		"stancecolor_good 200,92,214,72\r\n"
		"stancecolor_middle 200,236,176,40\r\n"
		"stancecolor_bad 200,210,48,36\r\n"
		"DESTAGLCOLOR 96,60,220,60\r\n"
		"AGLCOLOR 72,96,230,96\r\n"
		"HUDSPINMAPX1 802\r\nHUDSPINMAPX2 1002\r\nHUDSPINMAPY1 546\r\nHUDSPINMAPY2 746\r\n"
		"SPINMAPWPDISTOFF 1\r\n"
		"HUDFLAGCARRIER 8,8,1,left\r\n"
		"GAMEINFO 1012,410,0,right\r\n"
		"HUDWPDINFO 1012,486,0,right\r\n"
		"ZONEINFO 1012,386,Center\r\n"
		"EXPPOINTS 8,96,0,200\r\n"
		"CONNECTSTATUS 1012,10,right\r\n"
		"HUDTEAMXY 1012,30,0,right\r\n"
		"HUDPLAYERCOUNT 1012,70,0,right\r\n"
		"AMMOCOUNTPOS 168,616,0,right\r\n"
		"HUDWEAPONNAME 14,588,0,left\r\n"
		"MAPCOORDS 1012,506\r\n"
		"HUDTIMECLOCK 1012,50\r\n"
		"BREATHTIME 512,84,center\r\n"
		"HUDLS_SYSTEM 1\r\nHUDLS_BRACKET ls_brack.tga\r\nHUDLS_KEYOFST 4,-6\r\n"
		"HUDLS_MOREAV ls_more.tga 300 -2\r\nHUDLS_SLOT 6 100,700\r\nHUDLS_SLOT 10 180 700\r\n"
		"HUDTITLEX 1012\r\nHUDTITLEY 330\r\nHUDPINGX 10\r\nHUDPINGY 10\r\nHUDPINGRIGHT 1\r\n"
		"HUDORDERS 1012,150\r\nSPECMODE_LABEL 512,700\r\nLFP_FLAGS 1016,96\r\nLFP_TAKEOVERDLG 200,600\r\n"
		"cargopos 190,700\r\nPAUSEDPOS 980 12\r\nNETWORKINDICATOR 6,5 30,5 70,5\r\nroomtkpos 20,400\r\n"
		"roomtktxtpos 12,466\r\nHUDSTANCEPOS 30,639\r\nHUDVEHSTANCEPOS 6,300\r\nHUDGeartext 14,640\r\n"
		"HUDWPNICON 14,606\r\nHUDCLIP 14,648\r\nHUDSCOPERANGEXY 180,300\r\nHUDSCOPEZEROXY 180,320\r\n"
		"HUDSCOPEMAGXY 180,340\r\nShowImpactDistPos 512,430\r\nHUDCHATTEXT 196,712\r\nHUDSYSTEXT 8,24\r\n"
		"HUDCHLINE 8\r\nHUDAGLRADIUS 12\r\nHUDROCLEN 40\r\n"
		"alphafade 40 70.25 1.5\r\n"
		"HUDAGLTLRX 120,124\r\nHUDAGLYLEN 440,90\r\n"
		"HUDSTANCE 0\t0 0 onhstnc0.tga STAND\r\n"
		"HUDSTANCE 1\t2 -3 onhstnc1.tga CROUCH\r\n"
		"HUDDECLUT_WPNGRP 1 1 1 0\r\n"
		"HUDDECLUT_CTAPE 1 1 0 0\r\n"
		"HUDDECLUT_WPNGRP 0 0 0 1\r\n"
		"StaticFrame old.tga 0,0\r\n"
		"StaticFrame onhframe.tga 6,586\r\n"
		"ParachuteIcon H_pchute.tga 10,659\r\n"
		"ArmorIcon H_armr.tga 10,659\r\n"
		"VEHICLE_HUD\r\n  sid Dapche1\r\n  interface h_apche.tga\r\n  driver 51,103\r\n  emplace 1,51,78\r\nVEHICLE_END\r\n"
		"VEHICLE_HUD\r\n  sid Dmrk51\r\n  icon i.tga\r\n  statictexture s.tga\r\n"
		"  seats 4,14,146,14,162,43,162,43,146\r\n  emplace 4,14,179,14,203,43,179,43,203\r\nVEHICLE_END\r\n"
		"NOSUCHKEY 1 2 3\r\n";

int synthetic_every_key() {
	const std::string text = round_trip(kEveryKey, "every key");
	Parsed read(text);
	const DefHudPosDef &hud = read.file.hud;
	// The values as the game reads them, through the writer's form.
	CHECK(std::strcmp(hud.font_lo, "my font.fnt") == 0);
	CHECK(hud.alpha_fade[1] == 70.25 && hud.alpha_fade[2] == 1.5);
	CHECK(hud.hudls_moreav_off[0] == 44 && hud.hudls_moreav_off[1] == -2);
	CHECK(hud.zone_info[2] == 2 && hud.network_indicator_present == 1 && hud.network_indicator[4] == 70);
	CHECK(hud.stances_count == 2 && hud.declutter_count == 3 && hud.static_frames_count == 2 &&
	      hud.vehicle_huds_count == 2 && hud.vehicle_huds[1].seat_count == 4);
	// The borders are a, r, g, b: the first field the alpha [orig: HUDHEALTHBORDER @0x5A13E9..0x5A143A].
	CHECK(hud.health_border.a == 90 && hud.health_border.r == 200 && hud.health_border.b == 202);
	// The writer's own form: a tab after the key, commas between the values, the alignment as its word, a
	// name with a space quoted; what the model holds nothing of (the comment, NOSUCHKEY) left out.
	CHECK(text.find("AMMOCOUNTPOS\t168,616,0,right\r\n") != std::string::npos);
	CHECK(text.find("ZONEINFO\t1012,386,center\r\n") != std::string::npos);
	CHECK(text.find("EXPPOINTS\t8,96,0,left\r\n") != std::string::npos);
	CHECK(text.find("fonthud1_lo\t\"my font.fnt\"\r\n") != std::string::npos);
	CHECK(text.find("alphafade\t40,70.25,1.5\r\n") != std::string::npos);
	CHECK(text.find("HUDHEALTHBORDER\t90,200,201,202\r\n") != std::string::npos);
	CHECK(text.find("weapon_textcolor\t246,202,70,128\r\n") != std::string::npos);
	CHECK(text.find("HUDLS_SLOT\t10,180,700\r\n") != std::string::npos);
	CHECK(text.find("HUDSTANCE\t1,2,-3,onhstnc1.tga,CROUCH\r\n") != std::string::npos);
	CHECK(text.find("VEHICLE_HUD\r\nsid\tDapche1\r\ninterface\th_apche.tga\r\ndriver\t51,103\r\nemplace\t1,51,78\r\n"
	                "VEHICLE_END\r\n") != std::string::npos);
	CHECK(text.find("//") == std::string::npos && text.find("NOSUCHKEY") == std::string::npos);
	// HUDORDERS is unauthored here at its BSS 0 / 0: left out, it reads 0 / 0 again.
	Parsed bare("HUDCLIP 5,6\r\n");
	const std::string bare_text = def_write_hudpos(bare.file).text;
	CHECK(bare_text == "HUDCLIP\t5,6\r\n");
	std::printf("every key: read, written, read the same; written again the same bytes\n");
	return 0;
}

int synthetic_key_values() {
	Parsed read(kEveryKey);
	std::vector<std::string> values;
	// A key's values whether the model authors it or not; a stance and a slot by their first value; a
	// declutter row's the last of its name; the static frame's the last.
	CHECK(hudpos_key_values(read.file.hud, "ammocountpos", "", values) &&
	      values == std::vector<std::string>({ "168", "616", "0", "right" }));
	Parsed empty("");
	CHECK(hudpos_key_values(empty.file.hud, "HUDORDERS", "", values) &&
	      values == std::vector<std::string>({ "0", "0" }));
	CHECK(hudpos_key_values(read.file.hud, "HUDSTANCE", "1", values) && values[1] == "2" && values[3] == "onhstnc1.tga");
	CHECK(!hudpos_key_values(read.file.hud, "HUDSTANCE", "4", values));
	CHECK(hudpos_key_values(read.file.hud, "HUDLS_SLOT", "6", values) &&
	      values == std::vector<std::string>({ "6", "100", "700" }));
	CHECK(hudpos_key_values(read.file.hud, "HUDDECLUT_WPNGRP", "", values) &&
	      values == std::vector<std::string>({ "0", "0", "0", "1" }));
	CHECK(hudpos_key_values(read.file.hud, "HUDDECLUT_SPINMAP", "", values) &&
	      values == std::vector<std::string>({ "0", "0", "0", "0" }));
	CHECK(hudpos_key_values(read.file.hud, "STATICFRAME", "", values) && values[0] == "onhframe.tga");
	CHECK(!hudpos_key_values(read.file.hud, "VEHICLE_HUD", "", values));
	// A name the tokenizer cannot carry is refused: the model would not read back.
	DefHudPosFile odd{};
	std::strcpy(odd.hud.font_hi, "a\"b.fnt");
	CHECK(!def_write_hudpos(odd).ok());
	std::printf("key values: each key's as the writer writes it; a name it cannot carry refused\n");
	return 0;
}

// A name as one token (quoted where a blank, a tab, a comma, ';' or "//" would cut it) and the
// channels of a colour key by its arm (any case; 0 for a key that is no colour).
int name_tokens_and_colour_channels() {
	CHECK(hudpos_name_token("HudFont.fnt") == "HudFont.fnt");
	CHECK(hudpos_name_token("my font.fnt") == "\"my font.fnt\"");
	CHECK(hudpos_name_token("a,b") == "\"a,b\"" && hudpos_name_token("a;b") == "\"a;b\"" &&
	      hudpos_name_token("a\tb") == "\"a\tb\"" && hudpos_name_token("a//b") == "\"a//b\"");
	CHECK(hudpos_name_token("a/b") == "a/b" && hudpos_name_token("").empty());
	for (const char *rgb : { "hud_textcolor", "WEAPON_TEXTCOLOR", "tagcolor_blueteam", "TagColor_Bad" })
		CHECK(hudpos_color_channels(rgb) == 3);
	for (const char *argb : { "STANCEICON_COLOR", "stancecolor_good", "HUDHEALTHBORDER", "AGLCOLOR", "destaglcolor" })
		CHECK(hudpos_color_channels(argb) == 4);
	for (const char *other : { "HUDHEALTH", "fonthud1_hi", "ALPHAFADE", "nonsense", "" })
		CHECK(hudpos_color_channels(other) == 0);
	return 0;
}

int retail_legs() {
	int ran = 0;
	auto leg = [&](const std::vector<uint8_t> &bytes, const std::string &what) {
		const std::string text(bytes.begin(), bytes.end());
		Parsed source(text);
		CHECK(source.ok && source.file.hud.stances_count > 0);
		round_trip(text, what.c_str());
		std::printf("%s: read, written, read the same; the writer's bytes canonical\n", what.c_str());
		++ran;
	};
	const std::string install = retail::install();
	if (!install.empty()) {
		std::vector<std::string> mounts{ std::string() };
		for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
		for (const std::string &expansion : mounts) {
			opennova::Vfs vfs;
			std::vector<uint8_t> bytes;
			if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) {
				std::printf("FAIL mount_game(%s, %s): %s\n", install.c_str(), expansion.c_str(), vfs.last_error().c_str());
				++failures;
			} else if (!vfs.read_file("hudpos.def", bytes) || bytes.empty()) {
				std::printf("FAIL hudpos.def is not on the install mount %s %s\n", install.c_str(), expansion.c_str());
				++failures;
			} else {
				leg(bytes, "the install's hudpos.def" + (expansion.empty() ? std::string() : " (" + expansion + ")"));
			}
		}
	} else {
		retail::skip_leg("OPENNOVA_JO_DIR (the packed install's hudpos.def)");
	}
	for (const std::string &path : { retail::asset_file("hudpos.def"), retail::reference_fixture("def/hudpos.def") }) {
		if (path.empty()) continue;
		std::FILE *file = std::fopen(path.c_str(), "rb");
		std::vector<uint8_t> bytes;
		if (file) {
			uint8_t buffer[4096];
			size_t got = 0;
			while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.insert(bytes.end(), buffer, buffer + got);
			std::fclose(file);
		}
		CHECK(!bytes.empty());
		leg(bytes, path);
	}
	if (ran == 0 && retail::assets().empty()) retail::skip_leg("OPENNOVA_JO_ASSETS (the extracted hudpos.def)");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	synthetic_every_key();
	synthetic_key_values();
	name_tokens_and_colour_channels();
	retail_legs();
	if (failures == 0) std::printf("def_write_hudpos: all passed\n");
	return failures == 0 ? 0 : 1;
}
