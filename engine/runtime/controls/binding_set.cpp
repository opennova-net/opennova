#include <runtime/controls/binding_set.h>
#include <runtime/controls/key_strings.h>
#include <base/io/strutil.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::controls {

std::string format_display_string(const BindingRecord &rec, bool flagged) {
  // [orig: KeyBinding_FormatDisplayString @0x496bd0]. Every prefix, separator,
  // mouse name and key name is a "Keys" table lookup with the binary's literal
  // as the fallback (key_strings.h).
  std::string out;
  const uint16_t keys[2] = {rec.primary, rec.secondary};
  const uint16_t mods[2] = {rec.primary_mod, rec.secondary_mod};
  // The keyName/displayName scratch: resolved from the PRIMARY up front
  // (@0x496c07, unconditionally) and re-resolved for every keyed slot arm 1
  // visits (@0x496ca7); arm 2 appends whatever it holds LAST (@0x496f01).
  uint16_t last_key = keys[0];
  // Arm 1: a keyed slot WITH a modifier -- walk both slots [orig: the up-front
  // resolve's stack pop @0x496c0c..0x496c0e, then the four-compare gate
  // @0x496c0f..0x496c2d (its last compare @0x496c28) and the two-slot loop
  // @0x496c40..0x496da5].
  if ((keys[0] != 0 && mods[0] != 0) || (keys[1] != 0 && mods[1] != 0)) {
    for (int slot = 0; slot < 2; ++slot) {
      if (keys[slot] == 0) continue;
      // The separator rides the SLOT INDEX (result > 0 @0x496c4d), not a
      // printed count.
      if (slot > 0) out += key_string("OR", " XXor ");  // @0x496c8f
      last_key = keys[slot];
      // "Ctrl-": the lookup call @0x496cc5 (the earlier cite @0x496cc7 is a
      // byte inside that call), inlined strcat copy @0x496cf1; "Shift-":
      // copy @0x496d41.
      if (mods[slot] == 17) out += key_string("Ctrl-", "XXCtrl - ");
      if (mods[slot] == 16) out += key_string("Shift-", "XXShift - ");
      // The key name: lookup @0x496d59, then the inlined strcat -- `mov ecx,eax`
      // @0x496d61, `mov edi,ebx` @0x496d6c..0x496d6d, `sub eax,ecx`
      // @0x496d6e..0x496d6f, copy @0x496d84.
      out += key_name(keys[slot]);
    }
  }
  // Arm 2: a keyed slot WITHOUT a modifier resets the buffer and prints the
  // LAST resolved key behind either slot's modifier [orig: @0x496dc5..0x496f24].
  if ((keys[0] != 0 && mods[0] == 0) || (keys[1] != 0 && mods[1] == 0)) {
    out.clear();                                                                     // @0x496dd5
    if (mods[0] == 17 || mods[1] == 17) out += key_string("Ctrl-", "XXCtrl - ");     // @0x496e21
    // "Alt-": lookup @0x496e51, then the inlined strcat -- `add edi,-1`
    // @0x496e6f and `mov cl,[edi+1]` @0x496e72 (the earlier cites @0x496e70 /
    // @0x496e73 are bytes inside those two), copy @0x496e81.
    if (mods[0] == 18 || mods[1] == 18) out += key_string("Alt-", "XXAlt - ");
    if (mods[0] == 16 || mods[1] == 16) out += key_string("Shift-", "XXShift - ");   // @0x496ee1
    out += key_name(last_key);                                                       // @0x496f01
  }
  // Arm 3: the mouse slot [orig: @0x496f34..0x49714f]. The mouse modifier
  // (entry word 16) RESETS the buffer with "<mod>-" -- retail sprintf's it
  // over the keyed text [orig: mask test @0x496f34, modifier load @0x496f3f,
  // lookup("Keys", modKeyName, modDisplayName) @0x496f6d, sprintf "%s-"
  // @0x496f79] -- then the separator joins a keyed slot [orig: strcat
  // @0x496fd4]; the button names are "Keys" lookups over the lowercase
  // "XXmouse n" literals (the shipped table maps them to "Mouse n").
  if (rec.mouse_mask != 0) {
    if (rec.mouse_mod != 0) out = key_name(rec.mouse_mod) + "-";
    if (keys[0] != 0 || keys[1] != 0) out += key_string("OR", " XXor ");
    switch (rec.mouse_mask) {
      case 1: out += key_string("LBUTTON", "XXmouse 1"); break;          // @0x497058
      case 2: out += key_string("RBUTTON", "XXmouse 2"); break;          // @0x49701c
      case 16: out += key_string("MBUTTON", "XXmouse 3"); break;         // @0x497098
      case 1024: out += key_string("MWHLUP", "XXMouse Whl Up"); break;   // @0x497125
      case 2048: out += key_string("MWHLDN", "XXMouse Whl Dn"); break;   // @0x4970ea
      default: break;
    }
  }
  if (flagged) out += " *"; // entry[1] & 0x200 [orig: @0x497166]
  return out;
}

namespace {

// Extended-Enter remap: the keypad Enter captures as scan 269
// [orig: Input_IsExtendedEnterKey check @ 0x55bb5b -> scan 269 @ 0x55bb67].
constexpr int kKeypadEnterScan = 269;

}  // namespace

bool is_extended_vk(int vk) {
  switch (vk) {
    case 0x21:  // PageUp
    case 0x22:  // PageDown
    case 0x23:  // End
    case 0x24:  // Home
    case 0x25:  // Left
    case 0x26:  // Up
    case 0x27:  // Right
    case 0x28:  // Down
    case 0x2C:  // PrintScreen
    case 0x2D:  // Insert
    case 0x2E:  // Delete
    case 0x6F:  // keypad divide
    case 0x90:  // NumLock
    case kKeypadEnterScan:
      return true;
    default:
      return false;
  }
}

BindingSet::BindingSet() {
  restore_defaults();
}

void BindingSet::restore_defaults() {
  ++revision_;
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  records_.assign(n, BindingRecord{});
  for (std::size_t i = 0; i < n; ++i) {
    // Keyboard defaults are the byte-exact catalog values; the mouse/joystick
    // runtime default arrays are the open D-CTRL-1 hunt (records stay
    // unbound) [orig: the DEFAULTS copy loop @ 0x55bdda..0x55be69].
    records_[i].primary = static_cast<uint16_t>(cat[i].default_key);
    records_[i].secondary = static_cast<uint16_t>(cat[i].default_key2);
    // The slot-1 modifier rides the same static row (row base +28, i.e. +24
    // from the flags word @0x8159AC): Ctrl+1..Ctrl+0 for the seat rows,
    // Ctrl+T for gtalk, ... [orig: seat1 flags @0x8160D8, modifier @0x8160F4].
    records_[i].primary_mod = static_cast<uint16_t>(cat[i].default_mod);
    records_[i].mouse_mask = cat[i].default_mouse;
    records_[i].mouse_mod = cat[i].default_mouse_mod;
    records_[i].joy_button = cat[i].default_joy;
    records_[i].joy_mod = cat[i].default_joy_mod;
  }
}

bool BindingSet::assign_key(int index, int vk, bool ctrl_held, bool shift_held,
                            bool extended, bool repeat) {
  if (index < 0 || index >= static_cast<int>(records_.size())) {
    return false;
  }
  // [orig: @ 0x55bb26 — VK 0x11 with the Ctrl-held flag never assigns (the
  //  ctrl-down state is set before its own event enqueues, so this is every
  //  Ctrl press); @ 0x55bb38 — VK 0xDE never assigns]
  if (vk == 0x11 && ctrl_held) {
    return false;
  }
  if (vk == 0xDE) {
    return false;
  }
  // Modifier 17 iff the event flag word is Ctrl-held ALONE (the exact
  // HIWORD == 0x800 compare — shift/extended/repeat flags defeat it)
  // [orig: @ 0x55bb4f..0x55bb51; Input_QueueKeyEvent @ 0x760c10].
  const uint16_t mod =
      (ctrl_held && !shift_held && !extended && !repeat) ? 17 : 0;
  ++revision_;
  uint16_t scan = static_cast<uint16_t>(vk & 0xFF);
  if (vk == kKeypadEnterScan) {
    scan = kKeypadEnterScan;
  }
  BindingRecord &r = records_[static_cast<std::size_t>(index)];
  // Re-assigning a (scan, modifier) the record already holds collapses the
  // record to that key as the sole primary [orig: @ 0x55bb82..0x55bc04].
  bool cleared = false;
  if (r.primary == scan && r.primary_mod == mod) {
    r.primary = scan;
    r.primary_mod = mod;
    r.secondary = 0;
    r.secondary_mod = 0;
    cleared = true;
  }
  if (r.secondary == scan && r.secondary_mod == mod) {
    r.primary = scan;
    r.primary_mod = mod;
    r.secondary = 0;
    r.secondary_mod = 0;
    return true;
  }
  if (cleared) {
    return true;
  }
  // Fill the empty slot; with both full the PRIMARY is replaced
  // [orig: @ 0x55bc0e..0x55bc41].
  if (r.primary != 0) {
    if (r.secondary == 0) {
      r.secondary = scan;
      r.secondary_mod = mod;
    } else {
      r.primary = scan;
      r.primary_mod = mod;
    }
  } else if (r.secondary != 0) {
    r.secondary = scan;
    r.secondary_mod = mod;
  } else {
    r.primary = scan;
    r.primary_mod = mod;
  }
  return true;
}

void BindingSet::assign_mouse(int index, uint16_t mask) {
  if (index < 0 || index >= static_cast<int>(records_.size())) {
    return;
  }
  ++revision_;
  records_[static_cast<std::size_t>(index)].mouse_mask = mask;
}

void BindingSet::clear(int index, Device device) {
  if (index < 0 || index >= static_cast<int>(records_.size())) {
    return;
  }
  ++revision_;
  BindingRecord &r = records_[static_cast<std::size_t>(index)];
  switch (device) {
    case Device::Keyboard:
      r.primary = 0;
      r.secondary = 0;
      r.primary_mod = 0;
      r.secondary_mod = 0;
      break;
    // CLEAR_KEY zeroes the device's binding AND its modifier word together
    // [orig: sub_55BFD0 @0x55c046/@0x55c04d (mouse mask + modifier),
    //  @0x55c030/@0x55c036 (joystick button + modifier); field map
    //  UI_BuildKeyBindingLoadoutTable @0x559ebe..0x559ef3].
    case Device::Mouse:
      r.mouse_mask = 0;
      r.mouse_mod = 0;
      break;
    case Device::Joystick:
      r.joy_button = 0;
      r.joy_mod = 0;
      break;
  }
}

std::string BindingSet::control_text(int index, Device device) const {
  const BindingRecord *r = record(index);
  if (r == nullptr) {
    return "";
  }
  switch (device) {
    case Device::Keyboard:
      return format_binding(r->primary, r->secondary, r->primary_mod,
                            r->secondary_mod);
    case Device::Mouse:
      // [orig: the mouse-mask display switch @ 0x55b803..0x55b961]
      switch (r->mouse_mask) {
        case kMouseLeft: return "Left";
        case kMouseRight: return "Right";
        case kMouseMiddle: return "Middle";
        case kMouseWheelUp: return "Mouse Whl Up";
        case kMouseWheelDown: return "Mouse Whl Dn";
        default: return "";
      }
    case Device::Joystick:
      // [orig: "JOYBUTTON%d" @ 0x55b79a]
      if (r->joy_button != 0) {
        return "JOYBUTTON" + std::to_string(static_cast<int>(r->joy_button));
      }
      return "";
  }
  return "";
}

std::vector<ControlRow> BindingSet::build_rows(Device device) const {
  std::vector<ControlRow> rows;
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  for (std::size_t i = 0; i < n && i < records_.size(); ++i) {
    if (!is_player_visible(cat[i])) {
      continue;
    }
    ControlRow row;
    row.cls = action_class_name(cat[i].cls);
    row.action = cat[i].name;
    row.control = control_text(static_cast<int>(i), device);
    rows.push_back(row);
  }
  return rows;
}

uint8_t joystick_pov_mask(int32_t angle) {
  if (static_cast<uint16_t>(angle) == 0xffffu) return 0;
  if (angle <= 2200) return 1;
  if (angle <= 6800) return 9;
  if (angle <= 11200) return 8;
  if (angle <= 15000) return 10;
  if (angle <= 20200) return 2;
  if (angle <= 24800) return 6;
  if (angle <= 29200) return 4;
  if (angle <= 33800) return 5;
  return 1;
}

bool BindingSet::pressed_joystick(int index,
    const std::function<bool(int)> &button_down,
    const std::array<int32_t, 4> &pov_angles, bool dead) const {
  // The ENABLE_JOYSTICK gate: see the header [orig: @0x499481].
  if (!joystick_enabled_) return false;
  const BindingRecord *r = record(index);
  std::size_t count = 0;
  const ActionDef *cat = catalog(&count);
  if (r == nullptr || (cat[index].modes & (dead ? 2u : 1u)) == 0) return false;
  const auto held = [&](uint8_t one_based) {
    if (one_based == 0) return false;
    if (one_based <= 128) return button_down(one_based - 1);
    const int bit = one_based - 129;
    return bit < 16 && (joystick_pov_mask(pov_angles[bit / 4]) & (1u << (bit % 4))) != 0;
  };
  // The joystick modifier (+34) never gates the fire: see the header.
  return held(r->joy_button);
}

bool BindingSet::pressed_mouse(int index, uint16_t held_mask,
    const std::function<bool(int)> &key_down, bool dead) const {
  const BindingRecord *r = record(index);
  std::size_t count = 0;
  const ActionDef *cat = catalog(&count);
  if (r == nullptr || (cat[index].modes & (dead ? 2u : 1u)) == 0) return false;
  if ((cat[index].flags & 4u) != 0)
    return (r->mouse_mask & held_mask) != 0 &&
        (r->mouse_mod == 0 || key_down(r->mouse_mod));
  // Event rows use the same priority walk on the held button, leaving the
  // caller to edge-detect it. Wheel events call mouse_event_action directly.
  for (uint16_t mask : {kMouseLeft, kMouseRight, kMouseMiddle})
    if ((held_mask & mask) != 0 && mouse_event_action(mask, key_down, dead) == index)
      return true;
  return false;
}

int BindingSet::mouse_event_action(uint16_t mask,
    const std::function<bool(int)> &key_down, bool dead) const {
  std::size_t count = 0;
  const ActionDef *cat = catalog(&count);
  for (bool modified : {true, false}) {
    for (std::size_t i = 0; i < records_.size(); ++i) {
      const BindingRecord &r = records_[i];
      if ((cat[i].flags & 4u) != 0 || (cat[i].modes & (dead ? 2u : 1u)) == 0 ||
          r.mouse_mask != mask || mask == 0) continue;
      const bool has_mod = r.primary_mod || r.secondary_mod || r.mouse_mod;
      if (has_mod != modified) continue;
      if (!modified || (r.primary_mod && key_down(r.primary_mod)) ||
          (r.secondary_mod && key_down(r.secondary_mod)) ||
          (r.mouse_mod && key_down(r.mouse_mod))) return static_cast<int>(i);
    }
  }
  return -1;
}

int BindingSet::action_index_for_row(int row) const {
  if (row < 0) {
    return -1;
  }
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  int visible = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!is_player_visible(cat[i])) {
      continue;
    }
    if (visible == row) {
      return static_cast<int>(i);
    }
    ++visible;
  }
  return -1;
}

std::vector<int> BindingSet::keys_for_token(const std::string &token) const {
  std::vector<int> keys;
  const int index = index_of_token(token);
  const BindingRecord *r = record(index);
  if (r != nullptr) {
    if (r->primary != 0) {
      keys.push_back(r->primary);
    }
    if (r->secondary != 0) {
      keys.push_back(r->secondary);
    }
  }
  return keys;
}

int BindingSet::pressed_key(int index,
                            const std::function<bool(int)> &key_down) const {
  const BindingRecord *r = record(index);
  if (r == nullptr || keyboard_captured_) {
    return 0;
  }
  auto slot_down = [&](uint16_t vk) { return vk != 0 && key_down(vk); };
  auto row_mod_down = [&](const BindingRecord &rec) {
    return slot_down(rec.primary_mod) || slot_down(rec.secondary_mod);
  };
  const uint16_t keys[2] = {r->primary, r->secondary};
  // Pass 1: the row's modifier is held and one of its keys is down
  // [orig: @0x49d35b..0x49d3a7].
  if (row_mod_down(*r)) {
    for (const uint16_t key : keys) {
      if (slot_down(key)) {
        return key;
      }
    }
    return 0;
  }
  // Fallback: both modifier words zero [orig: @0x49d3c1..0x49d3d2], for a key
  // no pass-1 row claims (a pass-1 fire skips the fallback @0x49d437).
  if (r->primary_mod != 0 || r->secondary_mod != 0) {
    return 0;
  }
  for (const uint16_t key : keys) {
    if (!slot_down(key)) {
      continue;
    }
    bool claimed = false;
    for (const BindingRecord &other : records_) {
      if ((other.primary == key || other.secondary == key) && row_mod_down(other)) {
        claimed = true;
        break;
      }
    }
    if (!claimed) {
      return key;
    }
  }
  return 0;
}

const BindingRecord *BindingSet::record(int index) const {
  if (index < 0 || index >= static_cast<int>(records_.size())) {
    return nullptr;
  }
  return &records_[static_cast<std::size_t>(index)];
}

bool BindingSet::set_record(int index, const BindingRecord &rec) {
  if (index < 0 || index >= static_cast<int>(records_.size())) {
    return false;
  }
  ++revision_;
  records_[static_cast<std::size_t>(index)] = rec;
  return true;
}

std::string display_string_for_token(const BindingSet &set, const std::string &token) {
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  for (std::size_t i = 0; i < n; ++i) {
    if (cat[i].token == nullptr || !strutil::iequals(cat[i].token, token)) continue;
    const BindingRecord *rec = set.record(static_cast<int>(i));
    if (rec == nullptr) break;
    return format_display_string(*rec, (cat[i].flags & 0x200u) != 0u);
  }
  return "???";
}

int BindingSet::index_of_token(const std::string &token) const {
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  for (std::size_t i = 0; i < n; ++i) {
    if (cat[i].token != nullptr && token == cat[i].token) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace opennova::controls
