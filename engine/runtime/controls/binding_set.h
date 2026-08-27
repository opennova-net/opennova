// The LIVE key-binding state behind the Options -> Controls remap flow.
//
// Retail keeps one 432-byte record per catalog action (the array at 0x25C772C):
// two keyboard slots (scan word + extended-flag word each), a mouse button mask
// word, and a joystick button byte. The remap capture writes into it, CLEAR_KEY
// clears the active device's slots, DEFAULTS re-copies the runtime default
// array, and the whole set round-trips through the player profile
// (player.sav record +1804 count / +1808 entries, 72-byte stride)
// [orig: KeyBinding_HandleKeyAssignment @ 0x55bb20; CLEAR_KEY handler
//  @ 0x55bfd0; DEFAULTS handler @ 0x55bd90; mouse capture callback
//  @ 0x55c780; profile copy @ 0x559d50; PlayerProfile_SaveToFiles @ 0x54be00].
//
// This port keeps the same slot semantics over the static catalog. Keyboard
// defaults are the byte-exact catalog values; the mouse/joystick runtime
// default arrays are still the D-CTRL-1 RE hunt, so their defaults are
// unbound. Joystick capture is not wired (no joystick runtime yet).

#ifndef OPENNOVA_CONTROLS_BINDING_SET_H
#define OPENNOVA_CONTROLS_BINDING_SET_H

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/controls/controls.h>
#include <cstddef>

namespace opennova::controls {

// Mouse capture masks [orig: the capture callback's event->mask map
// @ 0x55c78b..0x55c7d5].
inline constexpr uint16_t kMouseLeft = 0x1;
inline constexpr uint16_t kMouseRight = 0x2;
inline constexpr uint16_t kMouseMiddle = 0x10;
inline constexpr uint16_t kMouseWheelUp = 0x400;
inline constexpr uint16_t kMouseWheelDown = 0x800;

// One live binding record (the ported slice of the 432-byte original).
// The per-slot modifier word is a modifier VK: 17 (VK_CONTROL) renders the
// "Ctrl-" display prefix, 16 (VK_SHIFT) "Shift-", 0 none. The capture writes
// 17 exactly when the queued event's flag word is Ctrl-held alone
// [orig: KeyBinding_FormatBindingString @ 0x559a10 prefixes;
//  the capture's modifier write @ 0x55bb51; Input_QueueKeyEvent @ 0x760c10
//  flag word — 0x800 Ctrl held, 0x200 Shift held, 0x100 extended,
//  0x80 auto-repeat].
// The in-game binding display formatter the death screen's "call a medic"
// hint uses [orig: KeyBinding_FormatDisplayString @0x496bd0, called for the
// MedicReq row's record @0x553f0b]. Distinct from the Options table's
// KeyBinding_FormatBindingString (format_binding in controls.h): this one has
// three arms in the witnessed order — (1) a slot with BOTH a key and a
// modifier walks the two slots joining with " or " and prefixing "Ctrl - " /
// "Shift - " per slot; (2) a slot with a key and NO modifier RESETS the
// buffer and prints the first slot's key behind whichever modifier either
// slot carries (Ctrl / Alt / Shift); (3) a mouse button appends "<mod>-" for
// the mouse modifier, " or " when a keyboard slot exists, then "Mouse 1/2/3"
// or "Mouse Whl Up/Dn"; finally " *" when the record's flag word carries
// 0x200. The localized "Keys" table falls back to the marker-stripped names
// key_name() already produces.
struct BindingRecord;
std::string format_display_string(const BindingRecord &rec, bool flagged = false);

struct BindingRecord {
  uint16_t primary = 0;        // slot-1 VK scan (0 = unbound)
  uint16_t secondary = 0;      // slot-2 VK scan
  uint16_t primary_mod = 0;    // slot-1 modifier VK (17 Ctrl / 16 Shift / 0)
  uint16_t secondary_mod = 0;  // slot-2 modifier VK
  uint16_t mouse_mask = 0;     // kMouse* mask (0 = unbound)
  uint8_t joy_button = 0;      // button index + 1 (0 = unbound)
};

// Whether a VK rides Windows' extended-key lParam bit (bit 24), which the
// original's queue folds into the event flag word as 0x100 — an extended key
// held with Ctrl therefore records NO modifier (the capture's compare is
// against Ctrl-held ALONE) [orig: Input_QueueKeyEvent @ 0x760c10;
// KeyBinding_HandleKeyAssignment @ 0x55bb4f].
bool is_extended_vk(int vk);

class BindingSet {
 public:
  BindingSet();  // catalog defaults

  // Re-copy every record's defaults and forget edits
  // [orig: the DEFAULTS handler's per-record copy loop @ 0x55bdda..0x55be69].
  void restore_defaults();

  // Assign a captured key to a catalog action. Returns false for the two
  // rejected events: every Ctrl press (the ctrl-down state is set before its
  // own event enqueues, so VK 0x11 always arrives Ctrl-flagged — retail
  // cannot capture the Ctrl key) and VK 0xDE. The slot modifier becomes 17
  // exactly when Ctrl is held with no shift/extended/repeat flag. Assigning
  // a (scan, modifier) the record already holds collapses it to the sole
  // primary; otherwise the key fills the empty slot, or replaces the primary
  // when both slots are full [orig: KeyBinding_HandleKeyAssignment
  // @ 0x55bb20 — the drop @ 0x55bb26/0x55bb38, the modifier compare
  // @ 0x55bb4f..0x55bb51].
  bool assign_key(int index, int vk, bool ctrl_held, bool shift_held,
                  bool extended, bool repeat);

  // Assign a captured mouse button mask [orig: word write @ 0x55c815].
  void assign_mouse(int index, uint16_t mask);

  // Clear the record's slots for one device [orig: CLEAR_KEY @ 0x55c056
  //  (keyboard), @ 0x55c03e (mouse), @ 0x55c028 (joystick)].
  void clear(int index, Device device);

  // The Control-column text for one action on one device: the formatted
  // keyboard binding, the mouse button name, or "JOYBUTTONn"
  // [orig: update_control_mapping_display @ 0x55b700].
  std::string control_text(int index, Device device) const;

  // The Class/Action/Control rows for a device from the LIVE records
  // (same visibility gate as the static build_rows)
  // [orig: UI_PopulateControlMappingList @ 0x55c0c0].
  std::vector<ControlRow> build_rows(Device device) const;

  // The catalog index behind a visible row (row order == build_rows order),
  // -1 out of range.
  int action_index_for_row(int row) const;

  // Every VK bound to a catalog action token (primary then secondary; empty
  // when unbound). The gameplay consumer's lookup seam.
  std::vector<int> keys_for_token(const std::string &token) const;

  std::size_t size() const { return records_.size(); }
  const BindingRecord *record(int index) const;
  bool set_record(int index, const BindingRecord &rec);  // persistence load
  int index_of_token(const std::string &token) const;

 private:
  std::vector<BindingRecord> records_;  // catalog order
};

}  // namespace opennova::controls

#endif  // OPENNOVA_CONTROLS_BINDING_SET_H
