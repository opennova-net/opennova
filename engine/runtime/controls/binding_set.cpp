#include "controls/binding_set.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::controls {

std::string format_display_string(const BindingRecord &rec, bool flagged) {
  // [orig: KeyBinding_FormatDisplayString @0x496bd0]
  std::string out;
  const uint16_t keys[2] = {rec.primary, rec.secondary};
  const uint16_t mods[2] = {rec.primary_mod, rec.secondary_mod};
  // Arm 1: a keyed slot WITH a modifier — walk both slots [orig: @0x496c0e].
  if ((keys[0] != 0 && mods[0] != 0) || (keys[1] != 0 && mods[1] != 0)) {
    int printed = 0;
    for (int slot = 0; slot < 2; ++slot) {
      if (keys[slot] == 0) {
        ++printed;
        continue;
      }
      if (printed > 0) out += " or ";
      if (mods[slot] == 17) out += "Ctrl - ";
      if (mods[slot] == 16) out += "Shift - ";
      out += key_name(keys[slot]);
      ++printed;
    }
  }
  // Arm 2: a keyed slot WITHOUT a modifier resets the buffer and prints the
  // FIRST slot's key behind either slot's modifier [orig: @0x496cc7..0x496d6d].
  if ((keys[0] != 0 && mods[0] == 0) || (keys[1] != 0 && mods[1] == 0)) {
    out.clear();
    if (mods[0] == 17 || mods[1] == 17) out += "Ctrl - ";
    if (mods[0] == 18 || mods[1] == 18) out += "Alt - ";
    if (mods[0] == 16 || mods[1] == 16) out += "Shift - ";
    out += key_name(keys[0]);
  }
  // Arm 3: the mouse slot [orig: @0x496d6f..0x496e70]. Our record keeps no
  // mouse modifier word (retail entry word 16); the button names ride the
  // same "Keys" fallbacks.
  if (rec.mouse_mask != 0) {
    if (keys[0] != 0 || keys[1] != 0) out += " or ";
    switch (rec.mouse_mask) {
      case 1: out += "Mouse 1"; break;
      case 2: out += "Mouse 2"; break;
      case 16: out += "Mouse 3"; break;
      case 1024: out += "Mouse Whl Up"; break;
      case 2048: out += "Mouse Whl Dn"; break;
      default: break;
    }
  }
  if (flagged) out += " *"; // entry[1] & 0x200 [orig: @0x496e73]
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
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  records_.assign(n, BindingRecord{});
  for (std::size_t i = 0; i < n; ++i) {
    // Keyboard defaults are the byte-exact catalog values; the mouse/joystick
    // runtime default arrays are the open D-CTRL-1 hunt (records stay
    // unbound) [orig: the DEFAULTS copy loop @ 0x55bdda..0x55be69].
    records_[i].primary = static_cast<uint16_t>(cat[i].default_key);
    records_[i].secondary = static_cast<uint16_t>(cat[i].default_key2);
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
  records_[static_cast<std::size_t>(index)].mouse_mask = mask;
}

void BindingSet::clear(int index, Device device) {
  if (index < 0 || index >= static_cast<int>(records_.size())) {
    return;
  }
  BindingRecord &r = records_[static_cast<std::size_t>(index)];
  switch (device) {
    case Device::Keyboard:
      r.primary = 0;
      r.secondary = 0;
      r.primary_mod = 0;
      r.secondary_mod = 0;
      break;
    case Device::Mouse:
      r.mouse_mask = 0;
      break;
    case Device::Joystick:
      r.joy_button = 0;
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
  records_[static_cast<std::size_t>(index)] = rec;
  return true;
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
