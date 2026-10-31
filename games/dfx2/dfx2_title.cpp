#include "dfx2.h"

#include <formats/gamecfg/game_cfg.h>
#include <runtime/controls/controls.h>

namespace opennova::games {

namespace {

// The files DFX2's stock data never ships, which its program still reads unchecked: its archives
// hold no cmap.mnu and no vehicle.mnu (resource.pff, localres.pff, language.pff and med.pff of
// the Steam install, read 2026-10-26), and dfx2.exe loads them where JO does
// [orig: Game_StartMission @0x521530 (@0x5234a5, @0x5234c1) and Input_HandleActionBinding
// @0x49ad50 (@0x49b988) for cmap.mnu; Input_HandleActionBinding @0x49ad50 (@0x49b8ff) and
// @0x4dd4c0 (@0x4ddbf3) for vehicle.mnu (dfx2.exe)]: the commander key and a vehicle's loadout
// open nothing, as the game ships. Nor does it ship nw_cdata.coo, the menu browser's cookie jar
// (in no archive, nor beside dfx2.exe in the Steam install), which it loads where JO does, its
// HRESULT ignored, and writes itself [orig: Menu_InitShellResources @0x54f750 (@0x54f887),
// UI_SaveCookieFileIfDirty @0x554d40 (dfx2.exe)].
constexpr const char *kAbsentResourceRoles[] = { "cmap_menu", "vehicle_menu", "nw_cdata", nullptr };

// dfx2.exe's HUD label fonts: Joint Operations' three tiers, then Arial20b / 20n to 1280 pixels,
// Arial28b / 28n to 1600 and Arial32b / 32n above, over 1280 / 1600 / 1920; Impac44b and Impac76b
// over 1600 (DFX2-80).
// [orig: HUD_InitAllFonts @0x51BD70 (dfx2.exe) - the widths @0x51BDCE / @0x51BDFE / @0x51BE2E /
//  @0x51BE5B / @0x51BE88, the names @0x51BDDA..0x51BECD, the scale @0x51BEE1, Impac44b @0x51BF0D
//  and Impac76b @0x51BF40 over 1600 @0x51BF2E / @0x51BF53, the divide's magic 0x51EB851F
//  @0x51BF12..0x51BF21 shifting by 9 (`sar edx, 9` @0x51BF19)]
constexpr TitleHudFontTier kHudFontTiers[] = {
	{640, "Arial14n.fnt", "Arial12b.fnt", 640},
	{800, "Arial14n.fnt", "Arial14b.fnt", 800},
	{1024, "Arial16n.fnt", "Arial16b.fnt", 1024},
	{1280, "Arial20n.fnt", "Arial20b.fnt", 1280},
	{1600, "Arial28n.fnt", "Arial28b.fnt", 1600},
	{0, "Arial32n.fnt", "Arial32b.fnt", 1920},
};
constexpr TitleHudFonts kHudFonts{kHudFontTiers, "Impac44b.fnt", "Impac76b.fnt", 1600};

// dfx2.exe's static kit pages, each a null-terminated list of the page's weapon names (every entry
// "<name> -1 -1 -1").
// The single-player page: WPN_MK23 where Jointops.exe's names WPN_colt45; WPN_M4 is on the page and
// absent from DFX2's data, which retail drops the same way (docs/titles/dfx2-vs-jo-re.md DFX2-33).
// [orig: Buffer_CopyUntilDoubleNull(restrictionData, 0x832368, 2048) @0x52188f / @0x54ec2a, the
//  profile's copy @0x548f67 (dfx2.exe)]
constexpr const char *kSinglePlayerKit[] = { "WPN_M4AUTO", "WPN_M4", "WPN_MK23", "WPN_AT4",
	"WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM", "WPN_KNIFE", nullptr };
// DFX2-29: a fresh profile's class pages. On each side classes 7 and 8 take the XM4AUTO and M4AUTO
// pages (each side its own copy of the same names), classes 5, 6 and 9 an empty page: static
// buffers nothing writes, each read by the copy alone. Jointops.exe gives every class its own page
// per side (playersav::default_kit_page).
// [orig: PlayerProfile_InitDefaults @0x548db0 - side 0 @0x548ff9..0x549051: byte_254D468,
//  byte_254C468, the page at 0x832b68, the page at 0x833b68, byte_254A468; side 1
//  @0x548f94..0x548fec: byte_254CC68, byte_254BC68, the page at 0x833368, the page at 0x834368,
//  byte_2549C68 (dfx2.exe)]
constexpr const char *kXm4AutoKit[] = { "WPN_XM4AUTO", "WPN_XM4", "WPN_XMK23", "WPN_XAT4",
	"WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM", "WPN_KNIFE", nullptr };
constexpr const char *kM4AutoKit[] = { "WPN_M4AUTO", "WPN_MK23", "WPN_SATCHEL_CHARGE",
	"WPN_GRENADEFB", "WPN_GRENADEHE", "WPN_GRENADESM", "WPN_KNIFE", nullptr };
constexpr const char *kEmptyKit[] = { nullptr };
// Side 0's classes 5..9, then side 1's (Title::profile_class_kits).
constexpr const char *const *kProfileClassKits[] = {
	kEmptyKit, kEmptyKit, kXm4AutoKit, kM4AutoKit, kEmptyKit,
	kEmptyKit, kEmptyKit, kXm4AutoKit, kM4AutoKit, kEmptyKit,
};

// dfx2.exe's F12 map legend: 20 records, no medic, personmedicable, lfp, farp or armoury entry and
// a pwrweap one, 52 design units a row (DFX2-89).
// [orig: sub_497490 @0x497490 (dfx2.exe) - the records from aHudIconPerson @0x8144F8 to the empty
//  name @0x814868, the row step `mov [esp+8Ch], 34h` @0x497534]
constexpr TitleHudMapLegendIcon kHudMapLegendIcons[] = {
	{"hud_icon_person", 3, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_persondead", 8, 1.0f, 0x7F2F3F7Fu},
	{"hud_icon_waypoint", 1, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_psp", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_emplaced", 4, 0.8f, 0x7F7F6200u},
	{"hud_icon_emplaced2", 12, 0.8f, 0x7F7F6200u},
	{"hud_icon_vehicle", 10, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_watervehicle", 15, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_helo", 11, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_motorcycle", 25, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_bldg", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_bridge", 9, 1.0f, 0x7F207F20u},
	{"hud_icon_enemyloc", 24, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_recentspeaker", 28, 0.8f, 0x7F7F7F7Fu},
	{"hud_icon_ridewanted", 23, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrweap", 29, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrmed", 16, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrammo", 17, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_flag", 2, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_flagbay", 6, 1.0f, 0x7F7F7F7Fu},
};
constexpr TitleHudMapLegend kHudMapLegend{
		kHudMapLegendIcons, static_cast<int>(sizeof(kHudMapLegendIcons) / sizeof(kHudMapLegendIcons[0])),
		52};

// DFX2-63: dfx2.exe's loading screen: both fonts Arial20b.fnt under 1280 pixels of background and
// Arial40b.fnt from it, the label in 6CBF00, the band to the width less 139 over a custom
// background and less 18 over the stock one, and the stage's progress lines 20 (40) apart.
// [orig: sub_51EC60 @0x51EC60 - the width tests @0x51EE41 / @0x51EEBD, "Arial40b.fnt" @0x51EE48 /
//  @0x51EEC4, "Arial20b.fnt" @0x51EE4F / @0x51EECB, the label "<c6CBF00>%s:\r\n<cFFFFFF>"
//  @0x51F008, the band @0x51EF69..0x51EF73, the lines' font and pitch @0x51F0E6..0x51F108
//  (dfx2.exe)]
constexpr TitleLoadingScreen kLoadingScreen{"Arial20b.fnt", "Arial20b.fnt", 1280, "Arial40b.fnt",
		"Arial40b.fnt", 0x6CBF00u, 139, 18, true, 20, 40};

// DFX2-110: dfx2.exe's static binding catalog, every row as the boot reads it (controls::ActionDef:
// row, token, marker-stripped name, class, keys, flags, slot-1 modifier, mouse, joystick, their
// modifiers, modes, help id, code): Jointops.exe's rows but the weapon, Objectives, EmoteToggle,
// view and cheat rows below, each row at 0x8148A0 + 108 * row.
// [orig: the table @0x8148A0, sorted to aNull @0x828CA0 by KeyBinding_SortBySequentialId @0x498270;
//  KeyBinding_BuildFilteredTable @0x549530 (dfx2.exe)]
using controls::ActionClass;
constexpr controls::ActionDef kControlRows[] = {
	{0, "turn_left_abs", "Absolute Turn Left", ActionClass::Movement, 0x00, 0x00, 0x0C100425, 0, 0x0, 0x0, 0, 0, 3, 0, 166},
	{1, "lookpitch", "Look Pitch", ActionClass::Movement, 0x00, 0x00, 0x0C200425, 0, 0x0, 0x0, 0, 0, 3, 0, 164},
	{2, "move_forward", "Forward", ActionClass::Movement, 0x57, 0x26, 0x0C002C05, 0, 0x0, 0x0, 0, 0, 3, 1001, 152},
	{3, "move_back", "Back", ActionClass::Movement, 0x53, 0x28, 0x0C000C05, 0, 0x0, 0x0, 0, 0, 3, 1003, 151},
	{4, "strafe_left", "StrafeLeft", ActionClass::Movement, 0x41, 0x25, 0x0C001C05, 0, 0x0, 0x0, 0, 0, 3, 1005, 156},
	{5, "strafe_right", "StrafeRight", ActionClass::Movement, 0x44, 0x27, 0x0C000C05, 0, 0x0, 0x0, 0, 0, 3, 1007, 157},
	{6, "LeanRoll_left", "Lean/Roll Left", ActionClass::Movement, 0x51, 0x24, 0x0C004C05, 0, 0x0, 0x0, 0, 0, 3, 1009, 148},
	{7, "LeanRoll_right", "Lean/Roll Right", ActionClass::Movement, 0x45, 0x21, 0x0C000C05, 0, 0x0, 0x0, 0, 0, 3, 1011, 147},
	{8, "move_jump", "Jump", ActionClass::Movement, 0x20, 0x2D, 0x0C000C05, 0, 0x0, 0x0, 0, 0, 1, 1013, 153},
	{9, "Prone", "Prone", ActionClass::Movement, 0x5A, 0x22, 0x0C000C01, 0, 0x10, 0x0, 0, 0, 3, 1015, 170},
	{10, "Crouch", "Crouch", ActionClass::Movement, 0x58, 0x23, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 3, 1017, 169},
	{11, "Stand", "Stand", ActionClass::Movement, 0x43, 0x2E, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 3, 1019, 172},
	{12, "look_down", "LookDown", ActionClass::Movement, 0xBE, 0x00, 0x0C000C05, 0, 0x0, 0x82, 0, 0, 3, 1021, 154},
	{13, "look_up", "LookUp", ActionClass::Movement, 0x50, 0x00, 0x0C000C05, 0, 0x0, 0x81, 0, 0, 3, 1022, 155},
	{14, "turn_left", "TurnLeft", ActionClass::Movement, 0x4C, 0x00, 0x0C008C05, 0, 0x0, 0x83, 0, 0, 3, 1023, 158},
	{15, "turn_right", "TurnRight", ActionClass::Movement, 0xBA, 0x00, 0x0C000C05, 0, 0x0, 0x84, 0, 0, 3, 1024, 159},
	{16, "Last_move", "Last_move", ActionClass::Movement, 0x91, 0x00, 0x0C000405, 0, 0x0, 0x0, 0, 0, 3, 1025, 425},
	{17, "seat1", "Seat1", ActionClass::Movement, 0x31, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2031, 182},
	{18, "seat2", "Seat2", ActionClass::Movement, 0x32, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2032, 183},
	{19, "seat3", "Seat3", ActionClass::Movement, 0x33, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2033, 184},
	{20, "seat4", "Seat4", ActionClass::Movement, 0x34, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2034, 185},
	{21, "seat5", "Seat5", ActionClass::Movement, 0x35, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2035, 186},
	{22, "seat6", "Seat6", ActionClass::Movement, 0x36, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2036, 187},
	{23, "seat7", "Seat7", ActionClass::Movement, 0x37, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2037, 188},
	{24, "seat8", "Seat8", ActionClass::Movement, 0x38, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2038, 189},
	{25, "seat9", "Seat9", ActionClass::Movement, 0x39, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2039, 190},
	{26, "seat10", "Seat10", ActionClass::Movement, 0x30, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 2040, 191},
	{27, "showhud", "Hide Gun", ActionClass::Weapons, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 1, 0, 14},
	{28, "Knife", "Knife", ActionClass::Weapons, 0x31, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2001, 201},
	{29, "Secondary", "Sidearm", ActionClass::Weapons, 0x32, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2002, 202},
	{30, "Primary", "Primary", ActionClass::Weapons, 0x33, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2003, 203},
	// The weapon rows, each firing its category (code - 200): Grenades 204 and Accessory 205
	// where Jointops.exe's Flashbang / FragGrenade / SmokeGrenade (204..206) and Accessory (207)
	// stand, the PowerUp rows 206..209 and 200 (key 0), and Detonator 210 (JO's 208) on Backspace;
	// no medpack, ToSpecial, dotsize, ScopeZeroDec or ScopeZeroInc row.
	// [orig: rows 31..38 @0x8155B4..0x8158A8 (dfx2.exe)]
	{31, "Grenades", "Grenades", ActionClass::Weapons, 0x34, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2004, 204},
	{32, "Accessory", "Accessory", ActionClass::Weapons, 0x35, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2005, 205},
	{33, "Powerup6", "PowerUp6", ActionClass::Weapons, 0x36, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2006, 206},
	{34, "Powerup7", "PowerUp7", ActionClass::Weapons, 0x37, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2007, 207},
	{35, "Powerup8", "PowerUp8", ActionClass::Weapons, 0x38, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2008, 208},
	{36, "Powerup9", "PowerUp9", ActionClass::Weapons, 0x39, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2009, 209},
	{37, "Powerup0", "PowerUp0", ActionClass::Weapons, 0x30, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2010, 200},
	{38, "Detonator", "Detonator", ActionClass::Weapons, 0x08, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2011, 210},
	{39, "cycleweaponP", "Cycle Weapon Prev", ActionClass::Weapons, 0xDB, 0x00, 0x0C000C01, 0, 0x400, 0x0, 0, 0, 1, 2017, 212},
	{40, "cycleweaponN", "Cycle Weapon Next", ActionClass::Weapons, 0xDD, 0x00, 0x0C000C01, 0, 0x800, 0x0, 0, 0, 1, 2016, 214},
	{41, "attack_1", "Fire Weapon", ActionClass::Weapons, 0x00, 0x00, 0x8C400C01, 0, 0x1, 0x1, 0, 0, 1, 2013, 149},
	{42, "useitem", "UseItem", ActionClass::Weapons, 0x10, 0x00, 0x0C000805, 0, 0x0, 0x0, 0, 0, 1, 2031, 177},
	{43, "nvggainup", "Increase NVG Gain", ActionClass::Weapons, 0xBB, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 3, 5014, 56},
	{44, "nvggaindown", "Decrease NVG Gain", ActionClass::Weapons, 0xBD, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 3, 5015, 57},
	{45, "magazine", "Reload", ActionClass::Weapons, 0x52, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2012, 211},
	{46, "radarout", "Radar Zoom Out", ActionClass::Map, 0xBD, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 4003, 361},
	{47, "radarin", "Radar Zoom In", ActionClass::Map, 0xBB, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 4002, 360},
	{48, "huddetail", "Hud Detail", ActionClass::Map, 0x75, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 4004, 19},
	{49, "NextWaypoint", "Next Waypoint", ActionClass::Map, 0x76, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 3, 4005, 23},
	{50, "nextflag", "Next Flag", ActionClass::Map, 0x77, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 3, 4006, 32},
	{51, "Briefing", "Briefing", ActionClass::Communications, 0x49, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5000, 53},
	{52, "Goals", "Goals", ActionClass::Communications, 0x47, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5001, 31},
	// Objectives (code 58, key O) where Jointops.exe's commander_menu row stands (none here).
	// [orig: row 53 @0x815EFC (dfx2.exe)]
	{53, "Objectives", "Objectives", ActionClass::Communications, 0x4F, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5002, 58},
	{54, "OldMessages", "Recent Messages", ActionClass::Communications, 0x4A, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5003, 29},
	{55, "talk", "Chat", ActionClass::Communications, 0x0D, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5004, 112},
	{56, "ltalk", "Local Chat", ActionClass::Communications, 0x54, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5005, 111},
	{57, "gtalk", "Global Chat", ActionClass::Communications, 0x54, 0x00, 0x05000800, 0x11, 0x0, 0x0, 0, 0, 3, 5006, 101},
	{58, "stalk", "Team Chat", ActionClass::Communications, 0x59, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5007, 100},
	{59, "sqtalk", "SQChat", ActionClass::Communications, 0x59, 0x00, 0x05000800, 0x11, 0x0, 0x0, 0, 0, 3, 5008, 110},
	{60, "ctalk", "Crew Chat", ActionClass::Communications, 0x55, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 1, 5009, 109},
	{61, "playerlist_alt", "PlayerList", ActionClass::Communications, 0x09, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 5012, 102},
	{62, "MedicReq", "MedicRequest", ActionClass::Communications, 0x39, 0x00, 0x04000040, 0, 0x0, 0x0, 0, 0, 1, 5010, 217},
	{63, "null", "Null", ActionClass::Null, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 0, 0},
	// Row 64 (code 36, the empty-token "!Null") is left out (controls::Catalog)
	// [orig: row 64 @0x8163A0 (dfx2.exe)].
	{65, "escape", "Escape", ActionClass::System, 0x1B, 0x00, 0x05000000, 0, 0x0, 0x0, 0, 0, 3, 10005, 18},
	{66, "respawn", "Respawn", ActionClass::System, 0x52, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 5013, 55},
	{67, "PrintScreen", "Screen Shot", ActionClass::System, 0x7A, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 10010, 2},
	{68, "pause", "Pause Game", ActionClass::System, 0x13, 0x48, 0x05000800, 0, 0x0, 0x9, 0, 0, 3, 10000, 25},
	{69, "command", "Command Promptx", ActionClass::System, 0xC0, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 10001, 1},
	{70, "command2", "Command Promptx", ActionClass::System, 0xC0, 0x00, 0x05000800, 0x11, 0x0, 0x0, 0, 0, 3, 10002, 20},
	{71, "helpmap", "Help Map Screen", ActionClass::System, 0x7B, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 10004, 234},
	{72, "scopescale", "Scope Scale", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10011, 49},
	{73, "Verbose", "Verbose", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10012, 37},
	{74, "hudcolor", "Hud Color", ActionClass::System, 0x75, 0x00, 0x04000000, 0x11, 0x0, 0x0, 0, 0, 3, 10013, 10},
	{75, "exit", "Exit Mission", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10014, 3},
	{76, "quit", "Exit Game", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10007, 4},
	{77, "flipmouse", "Flip Mouse", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10015, 9},
	{78, "restart", "Restart Mission", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10006, 12},
	{79, "lights", "Lights", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10016, 16},
	{80, "mousescale", "Mouse Scale", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10017, 17},
	{81, "ToggleServer", "Server Screen", ActionClass::System, 0xDC, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 0, 11},
	{82, "lockgame", "Lock Game", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6012, 119},
	{83, "PuntCRC", "Punt CRC", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6002, 47},
	{84, "PuntLog", "Punt Log", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6003, 48},
	{85, "Punt", "Punt", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6001, 34},
	{86, "savescores", "Save Scores", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 1, 6006, 38},
	{87, "netdelay", "Net Delay", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 1, 6007, 103},
	{88, "talkred", "Indonesian Talk", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6008, 104},
	{89, "talkblue", "Joint Ops Talk", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6009, 105},
	{90, "resetgames", "Reset Game", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6010, 106},
	{91, "Ban", "Ban", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6005, 107},
	{92, "LastGame", "Last Game", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6011, 108},
	{93, "tod", "TimeOfDay", ActionClass::Server, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 6013, 438},
	{94, "todrate", "TimeOfDayRate", ActionClass::Server, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 6014, 497},
	{95, "FreeLook", "FreeLook", ActionClass::Camera, 0x00, 0x00, 0x2C000C05, 0, 0x2, 0x0, 0, 0, 1, 0, 176},
	// Second keys F4 (Map), F5 (Friendly Tags, its first F), F2 (Binoculars) and F3 (Night
	// Vision); Show Score, Toggle Scope (its mouse button kept), gun view and Chase View ship
	// with no key, Help on '/'.
	// [orig: rows 96..107 @0x817120..0x8175C4 (dfx2.exe)]
	{96, "map_toggle", "Map", ActionClass::Map, 0x4D, 0x73, 0x05000801, 0, 0x0, 0x0, 0, 0, 3, 4001, 28},
	{97, "ShowScore", "Show Score", ActionClass::System, 0x00, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 10009, 422},
	{98, "ShowFriendly", "Friendly Tags", ActionClass::Communications, 0x46, 0x74, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 3, 5011, 30},
	// EmoteToggle (code 59), the emotes / radio menus' cycle, on no key.
	// [orig: row 99 @0x817264 (dfx2.exe)]
	{99, "EmoteToggle", "EmoteToggle", ActionClass::Communications, 0x00, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 1, 5013, 59},
	{100, "AudioEmote", "AudioEmote", ActionClass::Communications, 0x78, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 1, 5013, 33},
	{101, "RadioMacro", "RadioMacro", ActionClass::Communications, 0x79, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 1, 5014, 54},
	{102, "binoculars", "Binoculars", ActionClass::Weapons, 0x42, 0x71, 0x0C000801, 0, 0x0, 0x0, 0, 0, 3, 2019, 26},
	{103, "NVG", "Night Vision", ActionClass::Weapons, 0x4E, 0x72, 0x0C000801, 0, 0x0, 0x0, 0, 0, 3, 2020, 41},
	{104, "scope", "Toggle Scope", ActionClass::Weapons, 0x00, 0x00, 0x4C000C01, 0, 0x2, 0x0, 0, 0, 1, 2015, 6},
	{105, "help", "Help Screen", ActionClass::System, 0xBF, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 10003, 8},
	{106, "viewwithgun", "gun view", ActionClass::Camera, 0x00, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 3002, 401},
	{107, "viewchase", "Chase View", ActionClass::Camera, 0x00, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 1, 3003, 402},
	// "1st Person Toggle" (code 412) on F1, ahead of 1st Person View on Ctrl+F1.
	// [orig: rows 108 @0x817630, 109 @0x81769C (dfx2.exe)]
	{108, "viewtoggle", "1st Person Toggle", ActionClass::Camera, 0x70, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 3001, 412},
	{109, "view1st", "1st Person View", ActionClass::Camera, 0x70, 0x00, 0x0C000801, 0x11, 0x0, 0x0, 0, 0, 1, 3001, 400},
	{110, "CycleSpectatorMode", "Cycle Spectator Mode", ActionClass::Spectator, 0x20, 0x00, 0x04000800, 0, 0x10, 0x0, 0, 0, 2, 80001, 500},
	{111, "IncSpectatorTarget", "Spectator Target +", ActionClass::Spectator, 0x00, 0x00, 0x04000800, 0, 0x1, 0x0, 0, 0, 2, 80002, 501},
	{112, "DecSpectatorTarget", "Spectator Target -", ActionClass::Spectator, 0x00, 0x00, 0x04000800, 0, 0x2, 0x0, 0, 0, 2, 80003, 502},
	// The cheat rows, Jointops.exe's codes and texts over their own command CRCs (the token
	// field's first dword, little-endian).
	// [orig: rows 113..117 @0x81784C..0x8179FC, the dwords @+0x4B (dfx2.exe)]
	{113, "\x9F&\xEF\xAF", "FragGrenade", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 120},
	{114, "\x04*=|", "SmokeGrenade", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 121},
	{115, ".5\xDB\xDC", "Detonator", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 122},
	{116, "!\xBD~\xBA", "Medpack", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 123},
	{117, "\x8F\xB3\xA4\x16", "ToSpecial", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 40},
	{118, "fps", "Framerate", ActionClass::Debug, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 0, 74},
};
constexpr controls::Catalog kControls{kControlRows, sizeof(kControlRows) / sizeof(kControlRows[0])};

Title make() {
	Title t;
	t.code = "dfx2";
	t.display_name = "Delta Force Xtreme 2";
	// [orig: Game_ParseCommandLineAndInit @0x4A7360 (dfx2.exe) - off_8144D8's literal into
	//  g_TitleText @0x4A7D60..0x4A7D6C, the twin of Game_ParseCommandLineAndInit @0x4A7310 (the
	//  title sprintf @0x4A7D30..0x4A7D3C)]
	t.title_text = "DeltaForce: Xtreme 2";
	// The program the DFX2 install ships beside its archives (docs/titles/dfx2-vs-jo-re.md §6.1,
	// DFX2-5).
	t.executable = "dfx2.exe";
	// The command file the Steam DFX2 install's UPDATE.WIZ names (CMD=DFX2.CMD,
	// PRODUCT_LOGIN=DFX2).
	t.command_file = "DFX2.CMD";
	// dfx2.exe's cfg table, one row longer than Jointops.exe's: the mpdfxcategory its host lists
	// print as DFXCATEGORY1 (gamecfg::config_var_rows, DFX2-7).
	t.config_table = gamecfg::ConfigTable::Dfx2;
	t.absent_resource_roles = kAbsentResourceRoles;
	// DFX2-8: a session of at most 250, 251 with a dedicated host's slot, at every site that holds
	// JO's 64 / 65; the host screen's cap word 100 (which its NovaWorld screen replaces with the
	// browser variable NWHL when the service sets one, a screen a headless host lacks).
	// [orig: Config_SetDefaults @0x54a2c0 (@0x54a33c..0x54a343); Game_ApplySessionSettingsToGlobals
	//  @0x54e750 (@0x54ed93..0x54ed9a); CNapiGameSession_ConnectOrHost @0x4d5100
	//  (@0x4d5255..0x4d526f); CNapiGameSession_BuildAndCreateSession @0x567707..0x56770e;
	//  Server_AllocatePlayerSlotTable @0x5191b0 (@0x5191c5) (dfx2.exe)]
	// [orig: g_HostMaxPlayersCap @0x835f0c, read by HostDialog_ReadSettings @0x553af0 (@0x553dd0),
	//  UI_MapSliderIndexToMaxPlayers @0x5540b0 and the populate's reads @0x5544cd / @0x55450a; the
	//  NWHL replace @0x5575ce in UI_ProcessLANSessionStateMachine @0x556ff0 (dfx2.exe)]
	t.max_players = 250;
	t.max_players_host_screen = 100;
	// [orig: HostDialog_PopulateWeaponRestrictionList @0x5547b0 (@0x5547c1);
	//  HostDialog_PopulateClassListCombo @0x5548b0 (@0x5548c2); @0x5562c0 (each restriction
	//  control's registration, @0x556318..0x55640b) (dfx2.exe)]
	t.host_screen = "NW_MULTI_PLAYER";
	// DFX2-36: the EXPPOINTS element and its 0x81 fade.
	// [orig: sub_5A6D20 @0x5A6D20 (@0x5A6EDE..0x5A6EE7); sub_59B7E0 @0x59B7E0; sub_59B730 called
	//  @0x4223C5 (dfx2.exe)]
	t.hud_exp_points = true;
	// DFX2-91: the lone-gunner panel [orig: sub_5BEEA0 @0x5BEFBA..0x5BEFEC; sub_5B8050 @0x5B8050
	//  (dfx2.exe)].
	t.hud_lone_gunner_panel = true;
	// DFX2-92: the waypoint's tint is the active colour halved, at the level band
	// [orig: sub_58EDF0 @0x58EDF0 (@0x58EDFE..0x58EE0A) (dfx2.exe)].
	t.hud_waypoint_active_tint = true;
	// DFX2-93: a done objective's mark is a green square [orig: sub_5B9890 @0x5B9890
	//  (@0x5B9AFA..0x5B9B12) (dfx2.exe)].
	t.hud_objective_done_fill = true;
	// DFX2-94: no pre-round armory line [orig: sub_5BCD20 @0x5BCD20 (@0x5BCDAA..0x5BCE30)
	//  (dfx2.exe)].
	t.hud_drops_armory_prompt = true;
	// DFX2-95: the end-game screen's footer in the text box's corners [orig: sub_5BBF20
	//  @0x5BBF20 (@0x5BCC3D..0x5BCCF0) (dfx2.exe)].
	t.hud_end_game_footer_corners = true;
	// DFX2-41 / DFX2-38: the DEATH screen's activation fills the three category lists, its close
	// on the triggers' clear applies them, and the serializer writes every grenade position
	// [orig: sub_550690 @0x550690 (Armory_ResolveSelectedClass @0x550ae0,
	//  UI_PopulateThreeCategoryLists @0x550ae5), run by UI_DispatchScreenEvent @0x54c231;
	//  sub_550BA0 @0x550ba0, called @0x5c9c09; WeaponLoadout_SerializeSelectionsToBuffer
	//  @0x564850 (@0x564b5c..0x564c20) (dfx2.exe)]. Jointops.exe's close calls nothing there and
	// its serializer tests each GRENADE_AMMO control [orig: nullsub_137 @0x5cac97;
	//  WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 (@0x565bcf..0x565be5)].
	t.death_screen_loadout = true;
	t.hud_fonts = &kHudFonts;
	// DFX2-22: the catalog sorted by title [orig: MissionList_ScanAndBuildFromFiles @0x5620d0 -
	//  the qsort @0x562550 over CompareFunction @0x561840, _stricmp on entry+1044 (dfx2.exe)].
	t.mission_list_order = MissionListOrder::Title;
	// DFX2-4: the profile files' magic, "2XFD" [orig: PlayerProfile_SaveToFiles @0x549080 - the
	//  header @0x5490ae; PlayerProfile_LoadAllFromDisk @0x54c720 - edi = 0x44465832 @0x54c7dc,
	//  the player.sav gate @0x54c800, the weapon.sav gate @0x54c93f (dfx2.exe)].
	t.profile_magic = 0x44465832u;
	// Record 0's fresh name, "Bravo 1", where Jointops.exe reads the account's [orig:
	//  PlayerProfile_LoadAllFromDisk @0x54c720 - the eight bytes @0x7d2d84 copied over the name
	//  @0x54c770..0x54c78a; no GetUserNameA call (dfx2.exe)].
	t.profile_first_name = "Bravo 1";
	// A fresh record's +1396 is 1 [orig: PlayerProfile_InitDefaults @0x548db0 - `mov [esi+574h],
	//  ebp` @0x548ece, between the +1524 seed @0x548ec8 and the joystick test @0x548eda; the
	//  word's one reference in the executable (dfx2.exe)].
	t.profile_word_1396 = 1;
	// No `/maxplayers` switch: the walk goes from "/noreload" to "/version" [orig:
	//  Game_ParseCommandLineAndInit @0x4a7360 - @0x4a7727, @0x4a773f], and the host file's
	//  MaxPlayers stores its atol alone [orig: ServerConfig_ApplyHostSetting @0x4a6060 -
	//  @0x4a6289..0x4a6296 (dfx2.exe)].
	t.max_players_switch = false;
	// DFX2-29: a fresh profile's kit pages (above); the single-player page is the rules' spawn kit
	// too (DFX2-33, dfx2_rules.cpp).
	t.profile_single_player_kit = kSinglePlayerKit;
	t.profile_class_kits = kProfileClassKits;
	// DFX2-58: the map views' ceiling 6.0 (flt_7CC0C8): the CMAP window's [orig:
	// CMapWindow_HandleEvent @0x546a60 - @0x5454e6, @0x546fcc] and the DEATH map pane's [orig:
	// sub_550690 @0x550922 / @0x550967 (the reset @0x550974); sub_550F40 @0x551178 (the right
	// drag); sub_550C60 @0x550ca6 (the wheel) (dfx2.exe)].
	t.map_view_zoom_max = 6.0f;
	// DFX2-41: the DEATH map pane's frame, four lines round the pane's rect in 0xFFE1DF4B after
	// the map [orig: sub_550F40 @0x550fe6..0x551052 (the map, then the frame) (dfx2.exe)].
	t.death_map_frame_color = 0xFFE1DF4Bu;
	// DFX2-41: the reveal 360 ticks past the death, MAIN shown before the shroud; the activation
	// hides MAIN with it [orig: sub_552770 @0x552792 (360), CWnd_SetShown(MAIN, 1) @0x5527b1;
	// sub_550690 - CWnd_SetShown(MAIN, 0) @0x5507b8 (dfx2.exe)].
	t.death_shroud_reveal_ticks = 360;
	t.death_shroud_main = true;
	// DFX2-41: the team lists [orig: sub_5503A0 @0x5503a0, called by sub_552770 @0x5527e6
	// (dfx2.exe)].
	t.death_screen_team_lists = true;
	// DFX2-60: the campaign and instant-action tabs [orig: sub_560510 @0x560510 (sub_560190 called
	//  @0x560525, the IA_LIST test @0x560596, IA_BRIEFING @0x5606a6); sub_560ED0 @0x560ed0 (the tab
	//  radios, CA_LIST and CA_MISSION_LIST registered), run by sub_560F90 @0x560f90 (dfx2.exe)], the
	// twins of SinglePlayer_PopulateMissionList @0x561840 and UI_InitSinglePlayerScreen @0x562080.
	t.sp_campaign_tabs = true;
	// DFX2-61: the Mods list's base row reads "NONE" [orig: Options_PopulateModList @0x5582a0 (the
	//  literal @0x7CA4EC, read @0x5582d2) (dfx2.exe)].
	t.mod_list_base = "NONE";
	// DFX2-62: the render and audio populate hides OPTIONS' ADVANCED_OUTLINE and ADVANCED_TAB
	// [orig: sub_55AB20 @0x55ab20 (@0x55b829..0x55b862: CWnd_SetShown(0) on each) (dfx2.exe)], the
	// twin of UI_PopulateRenderAndAudioSettings @0x55c830, which ends at the Bink slots
	// (@0x55d530).
	t.options_hides_advanced = true;
	// DFX2-41: the no-zone instruction's penalty line [orig: sub_551E10 @0x55266f..0x55274f
	// (dfx2.exe)].
	t.death_respawn_penalty_line = true;
	// The HUD's own draws (DFX2-81..DFX2-90).
	// DFX2-90: the map key steps off <-> fullscreen [orig: sub_51DB80 @0x51DB80, called from the
	//  dispatcher's case 28 @0x4DD702 (dfx2.exe)].
	t.hud_map_cycle_skips_window = true;
	// DFX2-83: the Tab board's rows carry no class suffix [orig: sub_423890 @0x423890 - the team
	//  arm's format @0x423BB1 (dfx2.exe)].
	t.hud_kill_list_no_class = true;
	// DFX2-86: no magnification readout [orig: sub_59CEE0 @0x59CEE0 - the zero readout @0x59D428
	//  ends it (dfx2.exe)].
	t.hud_scope_no_magnification = true;
	// DFX2-84: the crosshair holds still, and no seat swaps it [orig: sub_590A80 @0x590A80 -
	//  @0x590F7F, the arms @0x590FB0..0x591032 (dfx2.exe)].
	t.hud_crosshair_fixed = true;
	// DFX2-85: the stance icon tinted by the health band [orig: sub_5A1A50 @0x5A1A50 -
	//  @0x5A1AB0..0x5A1AD4 (dfx2.exe)].
	t.hud_stance_health_tint = true;
	// DFX2-88: the message lines parse their tags [orig: sub_5B8CC0 @0x5B8DED, sub_5B8FF0
	//  @0x5B9036, sub_5B8E80 @0x5B8FAD (dfx2.exe)].
	t.hud_message_tags = true;
	t.hud_map_legend = &kHudMapLegend; // DFX2-89
	// DFX2-87: the tags' modes reach by range [orig: sub_5A27F0 @0x5A27F0 - @0x5A288C..0x5A28C1,
	//  @0x5A2E1F..0x5A309A (dfx2.exe)].
	t.hud_tag_ranges = true;
	// DFX2-81: the map's second name walk draws nothing [orig: sub_5A3850 @0x5A3850 -
	//  @0x5A3D53..0x5A3DD4 (dfx2.exe)].
	t.hud_map_no_bank_names = true;
	// DFX2-41: STATIC_GAMETYPE [orig: sub_550690 @0x550b41..0x550b77 (dfx2.exe)].
	t.death_screen_game_type_line = true;
	// DFX2-43: the cube's eye at least 5 units over the terrain [orig: EnvCube_Update @0x60F5F0 -
	//  `add eax, 50000h` @0x60F67B, the raise @0x60F689..0x60F68B (dfx2.exe)].
	t.env_cube_terrain_clearance = 0x50000;
	// DFX2-44: the NVG gain scale at the bottom centre [orig: the twin of NVG_DrawMaskAndGain
	//  @0x5CEEE0 - 0x1E0 @0x5CEF75, 0x2AA @0x5CEF7D, 0x220 @0x5CEF85, 0x2BE @0x5CEF8D (dfx2.exe)].
	t.nvg_gain_rect = TitleRect{ 480, 682, 544, 702 };
	// DFX2-45: the Inset's reticle quad, its texture never loaded [orig: the twin of
	//  Render_WeaponInsetScene @0x5C86B0 - sub_591440 @0x5C8E40, where Jointops.exe draws the four
	//  arms; sub_591440 @0x591440 - `test ecx, ecx; jz` @0x591460 over the never-written
	//  0x2724E2C (dfx2.exe)].
	t.inset_reticle_quad = true;
	// DFX2-47: the 512-square FrameFX work targets [orig: the twin of FrameFX_CreateRenderTargets
	//  @0x5820C0 - `mov eax, 200h` @0x582177; FrameFX_DrawPass @0x580F30 reads the same two words
	//  (dfx2.exe)].
	t.frame_fx_work_side = 512;
	// DFX2-48: the low-health ring [orig: the twin of Render_ProcessMainSceneFrame @0x5C9000 -
	//  the fraction @0x5C991D, FrameFX_ApplyScreenEffect(5, (0x10000 - fraction) / 32768)
	//  @0x5C995D (dfx2.exe)].
	t.low_health_screen_effect = true;
	// DFX2-49: the capture-zone rings under the map's marker layers too [orig: the twin of
	//  HUD_DrawMapOverlay @0x5A4FC0 - @0x5A5DA3..0x5A5E68 (dfx2.exe)].
	t.map_capture_rings_under_layers = true;
	// DFX2-50: the DEATH map's zone markers [orig: the twin of MapOverlay_DrawView @0x5A48A0 -
	//  @0x5A49C2..0x5A4A0D, @0x5A4A62..0x5A4ABA (dfx2.exe)].
	t.death_map_zone_markers = true;
	t.loading_screen = &kLoadingScreen;
	// DFX2-110: its own binding catalog (above).
	t.controls = &kControls;
	// DFX2-96: the I window's how-to, the briefing panel the Objectives window's.
	// [orig: sub_5BCD20 @0x5BCD20 - sub_5B8160 @0x5BCF86, dword_24BAE80 -> sub_5B9BB0 @0x5BCFB5;
	//  Input_HandleActionBinding @0x49AD50 case 58 @0x49B6DB..0x49B70F (dfx2.exe)]
	t.hud_sp_how_to = true;
	// DFX2-97: the STAT screen's DONE [orig: sub_5616C0 @0x5616c0 - DONE -> sub_561130
	//  @0x56173b..0x561756; sub_561780 @0x561780 (the hide @0x561812..0x561834); sub_561000
	//  @0x561000, from the menu-screen pass @0x54b901 (dfx2.exe)], the twins of
	// HUD_CacheStatPanelValues @0x5627a0, StatScreen_ShowCallback @0x562840 and nullsub_130 @0x562130.
	t.stat_screen_done = true;
	// The menu font scale 0.5: the shell's resource init stores flt_7C2A2C (0.5f) in flt_84DD50
	// ahead of the menu, and the font cache writes that into every menu font's +0x12EC [orig:
	// Menu_InitShellResources @0x54f750 (@0x54f750..0x54f759); CFontCache_LoadOrGetFont @0x6d8490
	// (@0x6d8607..0x6d8611); CFontCache_EnsureFontLoaded @0x6d9300 (@0x6d93c0..0x6d93cb)
	// (dfx2.exe)]. Jointops.exe's twin of the init stores nothing there (@0x552500).
	t.menu_font_scale = 0.5f;
	// DFX2-72: the host screen's combat-vehicle filter and its ADD's Switch default [orig:
	//  sub_555180 @0x555180 (@0x555300..0x555334), the twin of
	//  HostDialog_FilterMissionListByGameType @0x556fe0; sub_556B30 @0x556b30 (@0x556cd5..
	//  0x556d18), the twin of UI_InitHostSettingsDialog @0x558960; the ADD / REMOVE chunk of
	//  sub_556180 @0x555df0 (the 0 @0x556076, the re-show @0x555f87..0x555faf), the twin of
	//  HostDialog_AddRemoveSelectedMissions @0x557c10 (dfx2.exe)].
	t.host_vehicle_missions = true;
	t.host_switch_starts_off = true;
	// DFX2-73: the three category lists carry no NONE row [orig: UI_PopulateThreeCategoryLists
	//  @0x565d10 - the three ListWidget_SortRows @0x565e33..0x565e4d go straight to the ammo
	//  populate @0x565e55; no TextResource_GetStringWithFallback("Menu", "NONE") (dfx2.exe)], the
	// twin of Jointops.exe's @0x566db0, which inserts it at row 0 of each @0x566f05..0x566f34.
	t.armory_lists_none_row = false;
	// DFX2-115: the DEATH map names no one [orig: the twin of MapOverlay_DrawView @0x5A48A0 -
	//  `push 0` @0x5A4A30, sub_5A3850 @0x5A4A3E (dfx2.exe)].
	t.death_map_no_names = true;
	// DFX2-53: each player drawn as the first combo of its viewer-relative side [orig:
	//  Terrain_RenderSectorEntitiesBySide @0x5C6C20 - @0x5C6EA6..0x5C6F42 (dfx2.exe)].
	t.player_avatar_by_viewer_side = true;
	// DFX2-114: the GAMEINFO session board [orig: sub_59ABF0 @0x59ABF0, called from sub_5A71E0
	//  @0x5A7609 (dfx2.exe)].
	t.hud_game_info_board = true;
	// DFX2-82: the vehicle panel's rider lines [orig: sub_5A3DF0 @0x5A3DF0 - @0x5A4700..0x5A4871
	//  (dfx2.exe)].
	t.hud_vehicle_panel_riders = true;
	return t;
}

} // namespace

const Title &dfx2_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
