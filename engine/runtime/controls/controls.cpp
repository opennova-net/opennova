#include <runtime/controls/controls.h>
#include <runtime/controls/binding_set.h>
#include <runtime/controls/key_strings.h>

#include <base/title/title.h>

#include <iterator>

namespace opennova::controls {

// Jointops.exe's static action catalog, byte-witnessed from [orig: aAbsoluteTurnLe @ 0x8159cb]
// (108-byte stride, row 0 @0x8159A8). Each entry: catalog row, config token (+0x4B),
// marker-stripped display name (+0x23), class (+0x0C), default primary/secondary keyboard VK
// codes (+0x14 / +0x16), flags (+0x04), the slot-1 modifier (+0x1C), mouse mask (+0x18),
// joystick byte (+0x1A), their modifiers (+0x20 / +0x22), modes (+0x08), help id (+0x10) and the
// action code (+0x00) [orig: KeyBinding_BuildFilteredTable @0x54c2b0 reads each field]. The
// class is the record's own +0x0C byte [orig: KeyBinding_CompareEntries @0x4965f0 sorts on
// it]; rows 26/47/53/64/83/96-99/102/105/106/109 once carried the NEXT record's byte
// (corrected 2026-09-29). Row 66 (code 36) is left out (Catalog).
static const ActionDef k_catalog[] = {
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
    {31, "Flashbang", "Flashbang", ActionClass::Weapons, 0x34, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2004, 204},
    {32, "FragGrenade", "FragGrenade", ActionClass::Weapons, 0x35, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2005, 205},
    {33, "SmokeGrenade", "SmokeGrenade", ActionClass::Weapons, 0x36, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2006, 206},
    {34, "Accessory", "Accessory", ActionClass::Weapons, 0x37, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2007, 207},
    {35, "Detonator", "Detonator", ActionClass::Weapons, 0x38, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2008, 208},
    {36, "medpack", "Medpack", ActionClass::Weapons, 0x39, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2009, 209},
    {37, "ToSpecial", "ToSpecial", ActionClass::Weapons, 0x46, 0x00, 0x8C000801, 0, 0x0, 0x0, 0, 0, 1, 2025, 220},
    {38, "dotsize", "Sights Dot Size", ActionClass::Weapons, 0x4F, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 1, 2021, 216},
    {39, "cycleweaponP", "Cycle Weapon Prev", ActionClass::Weapons, 0xDB, 0x00, 0x0C000C01, 0, 0x400, 0x0, 0, 0, 1, 2017, 212},
    {40, "cycleweaponN", "Cycle Weapon Next", ActionClass::Weapons, 0xDD, 0x00, 0x0C000C01, 0, 0x800, 0x0, 0, 0, 1, 2016, 214},
    {41, "ScopeZeroDec", "Decrement Scope Zero", ActionClass::Weapons, 0xDE, 0x00, 0x0C000401, 0, 0x800, 0x0, 17, 0, 1, 0, 223},
    {42, "ScopeZeroInc", "Increment Scope Zero", ActionClass::Weapons, 0xDE, 0x00, 0x0C000401, 0x11, 0x400, 0x0, 17, 0, 1, 0, 222},
    {43, "attack_1", "Fire Weapon", ActionClass::Weapons, 0x00, 0x00, 0x8C400C01, 0, 0x1, 0x1, 0, 0, 1, 2013, 149},
    {44, "useitem", "UseItem", ActionClass::Weapons, 0x10, 0x00, 0x0C000805, 0, 0x0, 0x0, 0, 0, 1, 2031, 177},
    {45, "nvggainup", "Increase NVG Gain", ActionClass::Weapons, 0xBB, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 3, 5014, 56},
    {46, "nvggaindown", "Decrease NVG Gain", ActionClass::Weapons, 0xBD, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 3, 5015, 57},
    {47, "magazine", "Reload", ActionClass::Weapons, 0x52, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 2012, 211},
    {48, "radarout", "Radar Zoom Out", ActionClass::Map, 0xBD, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 4003, 361},
    {49, "radarin", "Radar Zoom In", ActionClass::Map, 0xBB, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 4002, 360},
    {50, "huddetail", "Hud Detail", ActionClass::Map, 0x75, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 4004, 19},
    {51, "NextWaypoint", "Next Waypoint", ActionClass::Map, 0x76, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 3, 4005, 23},
    {52, "nextflag", "Next Flag", ActionClass::Map, 0x77, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 3, 4006, 32},
    {53, "commander_menu", "Commander Menu", ActionClass::Map, 0x56, 0x00, 0x04000801, 0, 0x0, 0x0, 0, 0, 3, 2023, 221},
    {54, "Briefing", "Briefing", ActionClass::Communications, 0x49, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5000, 53},
    {55, "Goals", "Goals", ActionClass::Communications, 0x47, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5001, 31},
    {56, "OldMessages", "Recent Messages", ActionClass::Communications, 0x4A, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5003, 29},
    {57, "talk", "Chat", ActionClass::Communications, 0x0D, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5004, 112},
    {58, "ltalk", "Local Chat", ActionClass::Communications, 0x54, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5005, 111},
    {59, "gtalk", "Global Chat", ActionClass::Communications, 0x54, 0x00, 0x05000800, 0x11, 0x0, 0x0, 0, 0, 3, 5006, 101},
    {60, "stalk", "Team Chat", ActionClass::Communications, 0x59, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 5007, 100},
    {61, "sqtalk", "SQChat", ActionClass::Communications, 0x59, 0x00, 0x05000800, 0x11, 0x0, 0x0, 0, 0, 3, 5008, 110},
    {62, "ctalk", "Crew Chat", ActionClass::Communications, 0x55, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 1, 5009, 109},
    {63, "playerlist_alt", "PlayerList", ActionClass::Communications, 0x09, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 5012, 102},
    {64, "MedicReq", "MedicRequest", ActionClass::Communications, 0x39, 0x00, 0x04000040, 0, 0x0, 0x0, 0, 0, 1, 5010, 217},
    {65, "null", "Null", ActionClass::Null, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 0, 0},
    {67, "escape", "Escape", ActionClass::System, 0x1B, 0x00, 0x05000000, 0, 0x0, 0x0, 0, 0, 3, 10005, 18},
    {68, "respawn", "Respawn", ActionClass::System, 0x52, 0x00, 0x0C000C01, 0x11, 0x0, 0x0, 0, 0, 1, 5013, 55},
    {69, "PrintScreen", "Screen Shot", ActionClass::System, 0x7A, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 10010, 2},
    {70, "pause", "Pause Game", ActionClass::System, 0x13, 0x48, 0x05000800, 0, 0x0, 0x9, 0, 0, 3, 10000, 25},
    {71, "command", "Command Promptx", ActionClass::System, 0xC0, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 10001, 1},
    {72, "command2", "Command Promptx", ActionClass::System, 0xC0, 0x00, 0x05000800, 0x11, 0x0, 0x0, 0, 0, 3, 10002, 20},
    {73, "helpmap", "Help Map Screen", ActionClass::System, 0x7B, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 10004, 234},
    {74, "scopescale", "Scope Scale", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10011, 49},
    {75, "Verbose", "Verbose", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10012, 37},
    {76, "hudcolor", "Hud Color", ActionClass::System, 0x75, 0x00, 0x04000000, 0x11, 0x0, 0x0, 0, 0, 3, 10013, 10},
    {77, "exit", "Exit Mission", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10014, 3},
    {78, "quit", "Exit Game", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10007, 4},
    {79, "flipmouse", "Flip Mouse", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10015, 9},
    {80, "restart", "Restart Mission", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10006, 12},
    {81, "lights", "Lights", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10016, 16},
    {82, "mousescale", "Mouse Scale", ActionClass::System, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 10017, 17},
    {83, "ToggleServer", "Server Screen", ActionClass::System, 0xDC, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 0, 11},
    {84, "lockgame", "Lock Game", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6012, 119},
    {85, "PuntCRC", "Punt CRC", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6002, 47},
    {86, "PuntLog", "Punt Log", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6003, 48},
    {87, "Punt", "Punt", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6001, 34},
    {88, "savescores", "Save Scores", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 1, 6006, 38},
    {89, "netdelay", "Net Delay", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 1, 6007, 103},
    {90, "talkred", "Indonesian Talk", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6008, 104},
    {91, "talkblue", "Joint Ops Talk", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6009, 105},
    {92, "resetgames", "Reset Game", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6010, 106},
    {93, "Ban", "Ban", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6005, 107},
    {94, "LastGame", "Last Game", ActionClass::Server, 0x00, 0x00, 0x04000000, 0, 0x0, 0x0, 0, 0, 3, 6011, 108},
    {95, "tod", "TimeOfDay", ActionClass::Server, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 6013, 438},
    {96, "todrate", "TimeOfDayRate", ActionClass::Server, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 6014, 497},
    {97, "FreeLook", "FreeLook", ActionClass::Camera, 0x00, 0x00, 0x2C000C05, 0, 0x2, 0x0, 0, 0, 1, 0, 176},
    {98, "map_toggle", "Map", ActionClass::Map, 0x4D, 0x00, 0x05000801, 0, 0x0, 0x0, 0, 0, 3, 4001, 28},
    {99, "ShowScore", "Show Score", ActionClass::System, 0x74, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 3, 10009, 422},
    {100, "ShowFriendly", "Friendly Tags", ActionClass::Communications, 0x4B, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 3, 5011, 30},
    {101, "AudioEmote", "AudioEmote", ActionClass::Communications, 0x78, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 1, 5013, 33},
    {102, "RadioMacro", "RadioMacro", ActionClass::Communications, 0x79, 0x00, 0x0C000C01, 0, 0x0, 0x0, 0, 0, 1, 5014, 54},
    {103, "binoculars", "Binoculars", ActionClass::Weapons, 0x42, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 3, 2019, 26},
    {104, "NVG", "Night Vision", ActionClass::Weapons, 0x4E, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 3, 2020, 41},
    {105, "scope", "Toggle Scope", ActionClass::Weapons, 0xBF, 0x00, 0x4C000C01, 0, 0x2, 0x0, 0, 0, 1, 2015, 6},
    {106, "help", "Help Screen", ActionClass::System, 0x70, 0x00, 0x05000800, 0, 0x0, 0x0, 0, 0, 3, 10003, 8},
    {107, "view1st", "1st Person View", ActionClass::Camera, 0x71, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 3001, 400},
    {108, "viewwithgun", "gun view", ActionClass::Camera, 0x72, 0x00, 0x0C000801, 0, 0x0, 0x0, 0, 0, 1, 3002, 401},
    {109, "viewchase", "Chase View", ActionClass::Camera, 0x73, 0x00, 0x04000800, 0, 0x0, 0x0, 0, 0, 1, 3003, 402},
    {110, "CycleSpectatorMode", "Cycle Spectator Mode", ActionClass::Spectator, 0x20, 0x00, 0x04000800, 0, 0x10, 0x0, 0, 0, 2, 80001, 500},
    {111, "IncSpectatorTarget", "Spectator Target +", ActionClass::Spectator, 0x00, 0x00, 0x04000800, 0, 0x1, 0x0, 0, 0, 2, 80002, 501},
    // [orig: row 112 @0x8188E8 — code 502, mode 2 (death screen), class 13, no
    //  keys; its default is the RIGHT mouse button (+24 mask 0x2)]
    {112, "DecSpectatorTarget", "Spectator Target -", ActionClass::Spectator, 0x00, 0x00, 0x04000800, 0, 0x2, 0x0, 0, 0, 2, 80003, 502},
    // The cheat rows 113..115 and 117, row 116's shape: no key, flags 0x10, mode 1, class 9,
    // and a token field that holds no text but its 4-byte command CRC (row +0x4B,
    // little-endian), which Client_ProcessLocalCommand's out-of-session arm matches. Code
    // 120 refreshes the local weapon tables, 121 toggles the infinite-ammo bit, 122 sets the
    // local player's health word to 0x4000, 40 opens the single player's weapon screen
    // (inmatch::Role::dispatch_record carries their arms).
    // [orig: row 113 @0x818954 (code 120, the token dword 0x520BEC55 @0x81899F); row 114
    //  @0x8189C0 (121, 0xD2990EA1 @0x818A0B); row 115 @0x818A2C (122, 0x7C1E3767 @0x818A77);
    //  row 117 @0x818B04 (40, 0xCE42F6BD @0x818B4F); each "!<text>" @+0x23]
    {113, "U\xEC\x0BR", "FragGrenade", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 120},
    {114, "\xA1\x0E\x99\xD2", "SmokeGrenade", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 121},
    {115, "g7\x1E|", "Detonator", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 122},
    // The cheat row whose code 123 flips the local AI-ignores-the-player bit: no key, flags
    // 0x10 (the dispatcher's head drops it in a session), mode 1, class 9, its "!Medpack" text,
    // and a token field that holds no text but the 4-byte command CRC 0xFF623840 (row +0x4B,
    // little-endian), which Client_ProcessLocalCommand's out-of-session arm matches.
    // [orig: row 116 @0x818A98 (code 123, flags 0x10, mode 1, class 9; the token dword
    //  @0x818AE3); Client_ProcessLocalCommand @0x520BF0 - the CRC walk @0x520E17..0x520E30]
    {116, "@8b\xFF", "Medpack", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 123},
    {117, "\xBD\xF6" "B\xCE", "ToSpecial", ActionClass::Cheat, 0x00, 0x00, 0x00000010, 0, 0x0, 0x0, 0, 0, 1, 0, 40},
    // The Debug-class "fps" row: no key, flags 0 (no head gate, so the walk of a session's S2C
    // 0x06 echo can queue it too), modes 3; code 74 toggles the frame-rate overlay bit.
    // [orig: row 118 @0x818B70 (code 74, flags 0, mode 3, class 11, "!Framerate", the token
    //  "fps" @0x818BBB)]
    {118, "fps", "Framerate", ActionClass::Debug, 0x00, 0x00, 0x00000000, 0, 0x0, 0x0, 0, 0, 3, 0, 74},
};

const Catalog k_default_catalog{k_catalog, std::size(k_catalog)};

const Catalog &default_catalog() { return k_default_catalog; }

const Catalog &title_catalog(const Title &title) {
  return title.controls != nullptr ? *title.controls : k_default_catalog;
}

const char *action_class_name(ActionClass cls) {
  switch (cls) {
    case ActionClass::Null: return "Null";
    case ActionClass::Movement: return "Movement";
    case ActionClass::Weapons: return "Weapons";
    case ActionClass::Camera: return "Camera";
    case ActionClass::Map: return "Map";
    case ActionClass::Communications: return "Communications";
    case ActionClass::Server: return "Server";
    case ActionClass::NovaLogic: return "NovaLogic";
    case ActionClass::Cheat: return "Cheat";
    case ActionClass::System: return "System";
    case ActionClass::Debug: return "Debug";
    case ActionClass::TeammateMenu: return "teammatemenu";
    case ActionClass::Spectator: return "Spectator";
  }
  return "Unknown Type";
}

bool is_player_visible(const ActionDef &action) {
  // [orig: UI_PopulateControlMappingList @ 0x55c0c0] the keyboard populate
  // gate: (*entry & 0x20) == 0 && (*entry & 0x800) != 0.
  return (action.flags & 0x20u) == 0u && (action.flags & 0x800u) != 0u;
}

// Windows VK code -> the (binding name, display fallback) pair. A structural
// port of the engine's switch: every arm strcpy's the "Keys" lookup key and
// the "XX"-marked display fallback [orig: KeyBinding_GetKeyNameAndDisplayName
// @ 0x494c60 -- arms @0x494c8b..0x496233; the default arm @0x496235: a
// printable VK (219 "[" and 221 "]" included) writes "%c" to both, anything
// else sprintf's "%s %d" over KeyHelp_GetStringWithFallback("Keys", "KEY",
// "KEY") (0x7C7150 = "KEY") @0x496274/@0x49629f].
KeyNames key_binding_names(int vk) {
  switch (vk) {
    case 0x01: return {"LBUTTON", "XXMouse 1"};
    case 0x02: return {"RBUTTON", "XXMouse 2"};
    case 0x03: return {"CANCEL", "XXCancel"};
    case 0x04: return {"MBUTTON", "XXMouse 3"};
    case 0x08: return {"BACK", "XXBackspace"};
    case 0x09: return {"TAB", "XXTab"};
    case 0x0C: return {"CLEAR", "XXClear"};
    case 0x0D: return {"RETURN", "XXEnter"};
    case 0x10: return {"SHIFT", "XXShift"};
    case 0x11: return {"CONTROL", "XXCtrl"};
    case 0x12: return {"MENU", "XXAlt"};
    case 0x13: return {"PAUSE", "XXPause"};
    case 0x14: return {"CAPITAL", "XXCaps Lock"};
    case 0x15: return {"HANGUL", "XXHangul"};
    case 0x17: return {"JUNJA", "XXJunja"};
    case 0x18: return {"FINAL", "XXFinal"};
    case 0x19: return {"HANJA", "XXHanja"};
    case 0x1B: return {"ESCAPE", "XXEsc"};
    case 0x1C: return {"CONVERT", "XXConvert"};
    case 0x1D: return {"NONCONVERT", "XXNon Convert"};
    case 0x1E: return {"ACCEPT", "XXAccept"};
    case 0x1F: return {"MODECHANGE", "XXModechange"};
    case 0x20: return {"SPACE", "XXSpace"};
    case 0x21: return {"PRIOR", "XXPage Up"};
    case 0x22: return {"NEXT", "XXPage Down"};
    case 0x23: return {"END", "XXEnd"};
    case 0x24: return {"HOME", "XXHome"};
    case 0x25: return {"LEFT", "XXLeft"};
    case 0x26: return {"UP", "XXUp"};
    case 0x27: return {"RIGHT", "XXRight"};
    case 0x28: return {"DOWN", "XXDown"};
    case 0x29: return {"SELECT", "XXSelect"};
    case 0x2A: return {"PRINT", "XXPrint Screen"};
    case 0x2B: return {"EXECUTE", "XXExecute"};
    case 0x2C: return {"SNAPSHOT", "XXSnapshot"};
    case 0x2D: return {"INSERT", "XXInsert"};
    case 0x2E: return {"DELETE", "XXDelete"};
    case 0x2F: return {"HELP", "XXHelp"};
    case 0x5B: return {"LWIN", "XXLwin"};
    case 0x5C: return {"RWIN", "XXRwin"};
    case 0x5D: return {"APPS", "XXApps"};
    case 0x6A: return {"MULTIPLY", "XXNumpad *"};
    case 0x6B: return {"ADD", "XXNumpad +"};
    case 0x6C: return {"SEPARATOR", "XXSeparator"};
    case 0x6D: return {"SUBTRACT", "XXNumpad -"};
    case 0x6E: return {"DECIMAL", "XXNumpad ."};
    case 0x6F: return {"DIVIDE", "XXNumpad /"};
    case 0x90: return {"NUMLOCK", "XXNumlock"};
    case 0x91: return {"SCROLL", "XXScroll Lock"};
    case 0xA0: return {"LSHIFT", "XXLeft Shift"};
    case 0xA1: return {"RSHIFT", "XXRight Shift"};
    case 0xA2: return {"LCONTROL", "XXLeft Ctrl"};
    case 0xA3: return {"RCONTROL", "XXRight Ctrl"};
    case 0xA4: return {"LMENU", "XXLeft Alt"};
    case 0xA5: return {"RMENU", "XXRight Alt"};
    case 0xBA: return {";", "XX;"};
    case 0xBB: return {"=", "XX="};
    case 0xBC: return {",", "XX,"};
    case 0xBD: return {"-", "XX-"};
    case 0xBE: return {".", "XX."};
    case 0xBF: return {"/", "XX/"};
    case 0xC0: return {"`", "XX`"};
    case 0xDC: return {"\\", "XX\\"};
    case 0xDE: return {"'", "XX'"};
    case 0xF6: return {"ATTN", "XXAttn"};
    case 0xF7: return {"CRSEL", "XXCRSEL"};
    case 0xF8: return {"EXSEL", "XXEXSEL"};
    case 0xF9: return {"EREOF", "XXEREOF"};
    case 0xFA: return {"PLAY", "XXPLAY"};
    case 0xFB: return {"ZOOM", "XXZOOM"};
    case 0xFC: return {"NONAME", "XXNONAME"};
    case 0xFD: return {"PA1", "XXPA1"};
    case 0xFE: return {"OEM_CLEAR", "XXOEM_CLEAR"};
    case 0x10D: return {"PADENTER", "XXNumpad Enter"};
    default:
      break;
  }
  if (vk >= 0x60 && vk <= 0x69) {
    // "NUMPAD0".."NUMPAD9" / "XXNumpad 0".."XXNumpad 9" [orig: @0x495570..0x4957e5]
    const char digit = static_cast<char>('0' + (vk - 0x60));
    return {std::string("NUMPAD") + digit, std::string("XXNumpad ") + digit};
  }
  if (vk >= 0x70 && vk <= 0x87) {
    // "F1".."F24" / "XXF1".."XXF24" [orig: @0x4959db..0x495d8c]
    const std::string fn = std::string("F") + std::to_string(vk - 0x6F);
    return {fn, "XX" + fn};
  }
  // The default arm [orig: @0x496235..0x49629f]: isprint (the C locale's
  // 0x20..0x7E) plus the two bracket VKs write the character itself to BOTH
  // names -- 219 "[" @0x4962b1, 221 "]" (word_7C18E4) @0x4962e9, else "%c"
  // @0x49630e.
  if ((vk >= 0x20 && vk <= 0x7E) || vk == 0xDB || vk == 0xDD) {
    const char c = vk == 0xDB ? '[' : vk == 0xDD ? ']' : static_cast<char>(vk);
    return {std::string(1, c), std::string(1, c)};
  }
  // Everything else (VK 0 included): "<KEY label> <vk>" in both names, the
  // label itself a "Keys" lookup with the raw "KEY" fallback [orig: @0x496269
  // ..0x49629f; 0x7C7150 = "KEY"; the shipped table maps it to "Key"].
  const std::string label = key_string("KEY", "KEY") + " " + std::to_string(vk);
  return {label, label};
}

std::string key_name(int vk) {
  // The label the formatters append: lookup("Keys", keyName, displayName)
  // [orig: @0x496d61/@0x496f01/@0x559b61].
  const KeyNames names = key_binding_names(vk);
  return key_string(names.binding.c_str(), names.display.c_str());
}

std::string format_binding(int key, int key2, int modifier, int modifier2) {
  // [orig: KeyBinding_FormatBindingString @ 0x559a10]: the two-slot loop
  // @0x559a40..0x559b9e -- a keyed slot with a nonzero SLOT INDEX first
  // appends the "OR" separator (" XXor " fallback) @0x559a8f (so an empty
  // primary behind a keyed secondary still leads with the separator, as
  // written), then "Ctrl-" ("XXCtrl - ") for modifier word 17 @0x559af1,
  // "Shift-" ("XXShift - ") for 16 @0x559b41, then the localized key name
  // @0x559b61.
  const int keys[2] = {key, key2};
  const int mods[2] = {modifier, modifier2};
  std::string out;
  for (int slot = 0; slot < 2; ++slot) {
    if (keys[slot] == 0) {
      continue;
    }
    if (slot > 0) {
      out += key_string("OR", " XXor ");
    }
    if (mods[slot] == 17) {
      out += key_string("Ctrl-", "XXCtrl - ");
    }
    if (mods[slot] == 16) {
      out += key_string("Shift-", "XXShift - ");
    }
    out += key_name(keys[slot]);
  }
  return out;
}

std::vector<ControlRow> build_rows(const Catalog &catalog, Device device) {
  return BindingSet(catalog).build_rows(device);
}

}  // namespace opennova::controls
