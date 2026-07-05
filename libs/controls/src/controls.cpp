#include "controls/controls.h"

namespace opennova::controls {

// The static action catalog, byte-witnessed from [orig: aAbsoluteTurnLe @ 0x8159cb]
// (108-byte stride). Each entry: catalog id, config token, marker-stripped display
// name, category (Class), default primary/secondary keyboard VK codes. The default
// keys are read from the catalog's binding slot (record-relative -15/-13), validated
// against the canonical JO defaults (Forward=W/Up, Reload=R, Jump=Space, ...).
static const ActionDef k_catalog[] = {
    {0, "turn_left_abs", "Absolute Turn Left", ActionClass::Movement, 0x00, 0x00, 0x0C100425},
    {1, "lookpitch", "Look Pitch", ActionClass::Movement, 0x00, 0x00, 0x0C200425},
    {2, "move_forward", "Forward", ActionClass::Movement, 0x57, 0x26, 0x0C002C05},
    {3, "move_back", "Back", ActionClass::Movement, 0x53, 0x28, 0x0C000C05},
    {4, "strafe_left", "StrafeLeft", ActionClass::Movement, 0x41, 0x25, 0x0C001C05},
    {5, "strafe_right", "StrafeRight", ActionClass::Movement, 0x44, 0x27, 0x0C000C05},
    {6, "LeanRoll_left", "Lean/Roll Left", ActionClass::Movement, 0x51, 0x24, 0x0C004C05},
    {7, "LeanRoll_right", "Lean/Roll Right", ActionClass::Movement, 0x45, 0x21, 0x0C000C05},
    {8, "move_jump", "Jump", ActionClass::Movement, 0x20, 0x2D, 0x0C000C05},
    {9, "Prone", "Prone", ActionClass::Movement, 0x5A, 0x22, 0x0C000C01},
    {10, "Crouch", "Crouch", ActionClass::Movement, 0x58, 0x23, 0x0C000C01},
    {11, "Stand", "Stand", ActionClass::Movement, 0x43, 0x2E, 0x0C000C01},
    {12, "look_down", "LookDown", ActionClass::Movement, 0xBE, 0x00, 0x0C000C05},
    {13, "look_up", "LookUp", ActionClass::Movement, 0x50, 0x00, 0x0C000C05},
    {14, "turn_left", "TurnLeft", ActionClass::Movement, 0x4C, 0x00, 0x0C008C05},
    {15, "turn_right", "TurnRight", ActionClass::Movement, 0xBA, 0x00, 0x0C000C05},
    {16, "Last_move", "Last_move", ActionClass::Movement, 0x91, 0x00, 0x0C000405},
    {17, "seat1", "Seat1", ActionClass::Movement, 0x31, 0x00, 0x0C000C01},
    {18, "seat2", "Seat2", ActionClass::Movement, 0x32, 0x00, 0x0C000C01},
    {19, "seat3", "Seat3", ActionClass::Movement, 0x33, 0x00, 0x0C000C01},
    {20, "seat4", "Seat4", ActionClass::Movement, 0x34, 0x00, 0x0C000C01},
    {21, "seat5", "Seat5", ActionClass::Movement, 0x35, 0x00, 0x0C000C01},
    {22, "seat6", "Seat6", ActionClass::Movement, 0x36, 0x00, 0x0C000C01},
    {23, "seat7", "Seat7", ActionClass::Movement, 0x37, 0x00, 0x0C000C01},
    {24, "seat8", "Seat8", ActionClass::Movement, 0x38, 0x00, 0x0C000C01},
    {25, "seat9", "Seat9", ActionClass::Movement, 0x39, 0x00, 0x0C000C01},
    {26, "seat10", "Seat10", ActionClass::Weapons, 0x30, 0x00, 0x0C000C01},
    {27, "showhud", "Hide Gun", ActionClass::Weapons, 0x00, 0x00, 0x04000000},
    {28, "Knife", "Knife", ActionClass::Weapons, 0x31, 0x00, 0x0C000801},
    {29, "Secondary", "Sidearm", ActionClass::Weapons, 0x32, 0x00, 0x0C000801},
    {30, "Primary", "Primary", ActionClass::Weapons, 0x33, 0x00, 0x0C000801},
    {31, "Flashbang", "Flashbang", ActionClass::Weapons, 0x34, 0x00, 0x0C000801},
    {32, "FragGrenade", "FragGrenade", ActionClass::Weapons, 0x35, 0x00, 0x0C000801},
    {33, "SmokeGrenade", "SmokeGrenade", ActionClass::Weapons, 0x36, 0x00, 0x0C000801},
    {34, "Accessory", "Accessory", ActionClass::Weapons, 0x37, 0x00, 0x0C000801},
    {35, "Detonator", "Detonator", ActionClass::Weapons, 0x38, 0x00, 0x0C000801},
    {36, "medpack", "Medpack", ActionClass::Weapons, 0x39, 0x00, 0x0C000801},
    {37, "ToSpecial", "ToSpecial", ActionClass::Weapons, 0x46, 0x00, 0x8C000801},
    {38, "dotsize", "Sights Dot Size", ActionClass::Weapons, 0x4F, 0x00, 0x0C000C01},
    {39, "cycleweaponP", "Cycle Weapon Prev", ActionClass::Weapons, 0xDB, 0x00, 0x0C000C01},
    {40, "cycleweaponN", "Cycle Weapon Next", ActionClass::Weapons, 0xDD, 0x00, 0x0C000C01},
    {41, "ScopeZeroDec", "Decrement Scope Zero", ActionClass::Weapons, 0xDE, 0x00, 0x0C000401},
    {42, "ScopeZeroInc", "Increment Scope Zero", ActionClass::Weapons, 0xDE, 0x00, 0x0C000401},
    {43, "attack_1", "Fire Weapon", ActionClass::Weapons, 0x00, 0x00, 0x8C400C01},
    {44, "useitem", "UseItem", ActionClass::Weapons, 0x10, 0x00, 0x0C000805},
    {45, "nvggainup", "Increase NVG Gain", ActionClass::Weapons, 0xBB, 0x00, 0x0C000C01},
    {46, "nvggaindown", "Decrease NVG Gain", ActionClass::Weapons, 0xBD, 0x00, 0x0C000C01},
    {47, "magazine", "Reload", ActionClass::Map, 0x52, 0x00, 0x0C000801},
    {48, "radarout", "Radar Zoom Out", ActionClass::Map, 0xBD, 0x00, 0x04000800},
    {49, "radarin", "Radar Zoom In", ActionClass::Map, 0xBB, 0x00, 0x04000800},
    {50, "huddetail", "Hud Detail", ActionClass::Map, 0x75, 0x00, 0x04000800},
    {51, "NextWaypoint", "Next Waypoint", ActionClass::Map, 0x76, 0x00, 0x0C000C01},
    {52, "nextflag", "Next Flag", ActionClass::Map, 0x77, 0x00, 0x0C000801},
    {53, "commander_menu", "Commander Menu", ActionClass::Communications, 0x56, 0x00, 0x04000801},
    {54, "Briefing", "Briefing", ActionClass::Communications, 0x49, 0x00, 0x05000800},
    {55, "Goals", "Goals", ActionClass::Communications, 0x47, 0x00, 0x05000800},
    {56, "OldMessages", "Recent Messages", ActionClass::Communications, 0x4A, 0x00, 0x05000800},
    {57, "talk", "Chat", ActionClass::Communications, 0x0D, 0x00, 0x05000800},
    {58, "ltalk", "Local Chat", ActionClass::Communications, 0x54, 0x00, 0x05000800},
    {59, "gtalk", "Global Chat", ActionClass::Communications, 0x54, 0x00, 0x05000800},
    {60, "stalk", "Team Chat", ActionClass::Communications, 0x59, 0x00, 0x05000800},
    {61, "sqtalk", "SQChat", ActionClass::Communications, 0x59, 0x00, 0x05000800},
    {62, "ctalk", "Crew Chat", ActionClass::Communications, 0x55, 0x00, 0x05000800},
    {63, "playerlist_alt", "PlayerList", ActionClass::Communications, 0x09, 0x00, 0x04000800},
    {64, "MedicReq", "MedicRequest", ActionClass::Null, 0x39, 0x00, 0x04000040},
    {65, "null", "Null", ActionClass::Null, 0x00, 0x00, 0x00000000},
    {67, "escape", "Escape", ActionClass::System, 0x1B, 0x00, 0x05000000},
    {68, "respawn", "Respawn", ActionClass::System, 0x52, 0x00, 0x0C000C01},
    {69, "PrintScreen", "Screen Shot", ActionClass::System, 0x7A, 0x00, 0x04000800},
    {70, "pause", "Pause Game", ActionClass::System, 0x13, 0x48, 0x05000800},
    {71, "command", "Command Promptx", ActionClass::System, 0xC0, 0x00, 0x05000800},
    {72, "command2", "Command Promptx", ActionClass::System, 0xC0, 0x00, 0x05000800},
    {73, "helpmap", "Help Map Screen", ActionClass::System, 0x7B, 0x00, 0x05000800},
    {74, "scopescale", "Scope Scale", ActionClass::System, 0x00, 0x00, 0x04000000},
    {75, "Verbose", "Verbose", ActionClass::System, 0x00, 0x00, 0x04000000},
    {76, "hudcolor", "Hud Color", ActionClass::System, 0x75, 0x00, 0x04000000},
    {77, "exit", "Exit Mission", ActionClass::System, 0x00, 0x00, 0x04000000},
    {78, "quit", "Exit Game", ActionClass::System, 0x00, 0x00, 0x04000000},
    {79, "flipmouse", "Flip Mouse", ActionClass::System, 0x00, 0x00, 0x04000000},
    {80, "restart", "Restart Mission", ActionClass::System, 0x00, 0x00, 0x04000000},
    {81, "lights", "Lights", ActionClass::System, 0x00, 0x00, 0x04000000},
    {82, "mousescale", "Mouse Scale", ActionClass::System, 0x00, 0x00, 0x04000000},
    {83, "ToggleServer", "Server Screen", ActionClass::Server, 0xDC, 0x00, 0x04000000},
    {84, "lockgame", "Lock Game", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {85, "PuntCRC", "Punt CRC", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {86, "PuntLog", "Punt Log", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {87, "Punt", "Punt", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {88, "savescores", "Save Scores", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {89, "netdelay", "Net Delay", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {90, "talkred", "Indonesian Talk", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {91, "talkblue", "Joint Ops Talk", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {92, "resetgames", "Reset Game", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {93, "Ban", "Ban", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {94, "LastGame", "Last Game", ActionClass::Server, 0x00, 0x00, 0x04000000},
    {95, "tod", "TimeOfDay", ActionClass::Server, 0x00, 0x00, 0x00000000},
    {96, "todrate", "TimeOfDayRate", ActionClass::Camera, 0x00, 0x00, 0x00000000},
    {97, "FreeLook", "FreeLook", ActionClass::Map, 0x00, 0x00, 0x2C000C05},
    {98, "map_toggle", "Map", ActionClass::System, 0x4D, 0x00, 0x05000801},
    {99, "ShowScore", "Show Score", ActionClass::Communications, 0x74, 0x00, 0x04000800},
    {100, "ShowFriendly", "Friendly Tags", ActionClass::Communications, 0x4B, 0x00, 0x0C000C01},
    {101, "AudioEmote", "AudioEmote", ActionClass::Communications, 0x78, 0x00, 0x0C000C01},
    {102, "RadioMacro", "RadioMacro", ActionClass::Weapons, 0x79, 0x00, 0x0C000C01},
    {103, "binoculars", "Binoculars", ActionClass::Weapons, 0x42, 0x00, 0x0C000801},
    {104, "NVG", "Night Vision", ActionClass::Weapons, 0x4E, 0x00, 0x0C000801},
    {105, "scope", "Toggle Scope", ActionClass::System, 0xBF, 0x00, 0x4C000C01},
    {106, "help", "Help Screen", ActionClass::Camera, 0x70, 0x00, 0x05000800},
    {107, "view1st", "1st Person View", ActionClass::Camera, 0x71, 0x00, 0x0C000801},
    {108, "viewwithgun", "gun view", ActionClass::Camera, 0x72, 0x00, 0x0C000801},
    {109, "viewchase", "Chase View", ActionClass::Spectator, 0x73, 0x00, 0x04000800},
    {110, "CycleSpectatorMode", "Cycle Spectator Mode", ActionClass::Spectator, 0x20, 0x00, 0x04000800},
    {111, "IncSpectatorTarget", "Spectator Target +", ActionClass::Spectator, 0x00, 0x00, 0x04000800},
};

const ActionDef *catalog(std::size_t *out_count) {
  if (out_count != nullptr) {
    *out_count = sizeof(k_catalog) / sizeof(k_catalog[0]);
  }
  return k_catalog;
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

// Windows VK code -> display name. Ported from the engine's switch; the original
// returns a binding name plus an "XX"-prefixed display name and we keep the
// marker-stripped display form. [orig: KeyBinding_GetKeyNameAndDisplayName @ 0x494c60]
std::string key_name(int vk) {
  switch (vk) {
    case 0x01: return "Mouse 1";
    case 0x02: return "Mouse 2";
    case 0x03: return "Cancel";
    case 0x04: return "Mouse 3";
    case 0x08: return "Backspace";
    case 0x09: return "Tab";
    case 0x0C: return "Clear";
    case 0x0D: return "Enter";
    case 0x10: return "Shift";
    case 0x11: return "Ctrl";
    case 0x12: return "Alt";
    case 0x13: return "Pause";
    case 0x14: return "Caps Lock";
    case 0x1B: return "Esc";
    case 0x20: return "Space";
    case 0x21: return "Page Up";
    case 0x22: return "Page Down";
    case 0x23: return "End";
    case 0x24: return "Home";
    case 0x25: return "Left";
    case 0x26: return "Up";
    case 0x27: return "Right";
    case 0x28: return "Down";
    case 0x29: return "Select";
    case 0x2A: return "Print";
    case 0x2C: return "Snapshot";
    case 0x2D: return "Insert";
    case 0x2E: return "Delete";
    case 0x2F: return "Help";
    case 0x5B: return "Lwin";
    case 0x5C: return "Rwin";
    case 0x5D: return "Apps";
    case 0x6A: return "Numpad *";
    case 0x6B: return "Numpad +";
    case 0x6C: return "Separator";
    case 0x6D: return "Numpad -";
    case 0x6E: return "Numpad .";
    case 0x6F: return "Numpad /";
    case 0x90: return "Numlock";
    case 0x91: return "Scroll Lock";
    case 0xA0: return "Left Shift";
    case 0xA1: return "Right Shift";
    case 0xA2: return "Left Ctrl";
    case 0xA3: return "Right Ctrl";
    case 0xA4: return "Left Alt";
    case 0xA5: return "Right Alt";
    case 0xBA: return ";";
    case 0xBB: return "=";
    case 0xBC: return ",";
    case 0xBD: return "-";
    case 0xBE: return ".";
    case 0xBF: return "/";
    case 0xC0: return "`";
    case 0xDB: return "[";
    case 0xDC: return "\\";
    case 0xDD: return "]";
    case 0xDE: return "'";
    case 0x10D: return "Numpad Enter";
    default:
      break;
  }
  if (vk >= 0x60 && vk <= 0x69) {
    return std::string("Numpad ") + static_cast<char>('0' + (vk - 0x60));
  }
  if (vk >= 0x70 && vk <= 0x87) {
    return std::string("F") + std::to_string(vk - 0x6F);
  }
  if (vk >= 0x20 && vk <= 0x7E) {
    // Printable letters/digits/punctuation render as the character itself.
    return std::string(1, static_cast<char>(vk));
  }
  if (vk == 0) {
    return std::string();
  }
  return std::string("#") + std::to_string(vk);
}

std::string format_binding(int key, int key2) {
  std::string out;
  if (key != 0) {
    out = key_name(key);
  }
  if (key2 != 0) {
    // Slots joined by the engine's " or " separator (the " XXor " fallback,
    // marker-stripped). [orig: KeyBinding_FormatBindingString @ 0x559a10]
    if (!out.empty()) {
      out += " or ";
    }
    out += key_name(key2);
  }
  return out;
}

std::vector<ControlRow> build_rows(Device device) {
  std::vector<ControlRow> rows;
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  for (std::size_t i = 0; i < n; ++i) {
    const ActionDef &a = cat[i];
    if (!is_player_visible(a)) {
      continue;
    }
    ControlRow row;
    row.cls = action_class_name(a.cls);
    row.action = a.name;
    // Keyboard shows the byte-exact catalog defaults; mouse/joystick share the action
    // list but their default-binding arrays are not yet ported (D-CTRL-2).
    if (device == Device::Keyboard) {
      row.control = format_binding(a.default_key, a.default_key2);
    }
    rows.push_back(row);
  }
  return rows;
}

}  // namespace opennova::controls
