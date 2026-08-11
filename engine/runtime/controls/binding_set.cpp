#include "controls/binding_set.h"

#include <cstring>

namespace opennova::controls {

namespace {

// Extended-Enter remap: the keypad Enter captures as scan 269
// [orig: Input_IsExtendedEnterKey check @ 0x55bb5b -> scan 269 @ 0x55bb67].
constexpr int kKeypadEnterScan = 269;

}  // namespace

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

bool BindingSet::assign_key(int index, int vk, bool extended, bool repeat) {
  if (index < 0 || index >= static_cast<int>(records_.size())) {
    return false;
  }
  // [orig: @ 0x55bb26 — a repeating Ctrl is dropped; @ 0x55bb38 — VK 0xDE
  //  never assigns]
  if (vk == 0x11 && repeat) {
    return false;
  }
  if (vk == 0xDE) {
    return false;
  }
  const uint16_t ext = extended ? 17 : 0;
  uint16_t scan = static_cast<uint16_t>(vk & 0xFF);
  if (vk == kKeypadEnterScan) {
    scan = kKeypadEnterScan;
  }
  BindingRecord &r = records_[static_cast<std::size_t>(index)];
  // Re-assigning a key the record already holds collapses the record to that
  // key as the sole primary [orig: @ 0x55bb82..0x55bc04].
  bool cleared = false;
  if (r.primary == scan && r.primary_ext == ext) {
    r.primary = scan;
    r.primary_ext = ext;
    r.secondary = 0;
    r.secondary_ext = 0;
    cleared = true;
  }
  if (r.secondary == scan && r.secondary_ext == ext) {
    r.primary = scan;
    r.primary_ext = ext;
    r.secondary = 0;
    r.secondary_ext = 0;
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
      r.secondary_ext = ext;
    } else {
      r.primary = scan;
      r.primary_ext = ext;
    }
  } else if (r.secondary != 0) {
    r.secondary = scan;
    r.secondary_ext = ext;
  } else {
    r.primary = scan;
    r.primary_ext = ext;
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
      r.primary_ext = 0;
      r.secondary_ext = 0;
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
      return format_binding(r->primary, r->secondary);
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
