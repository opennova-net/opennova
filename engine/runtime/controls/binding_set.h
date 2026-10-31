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
// defaults and the mouse/joystick words come from the static catalog; the remap
// capture writes a joystick button too (assign_joystick).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <runtime/controls/controls.h>
#include <runtime/controls/key_event_queue.h>

namespace opennova::controls {

// DirectInput POV angle in hundredths of a degree -> up/down/left/right
// bits. 0xffff (including -1) is the centered/disconnected sentinel.
// [orig: Joystick_MapAxisToBitfield @0x764c10, fed dev->pov by @0x7653c0]
uint8_t joystick_pov_mask(int32_t angle);

// The joystick device's state for one input frame: buttons 1..128 at bits 0..127, the four
// hats' directions at bits 128..143 (hat h's direction d at 128 + 4h + d, in
// joystick_pov_mask's order), so a binding's 1-based byte b reads bit b - 1.
// [orig: dword_3343048, filled by Joystick_UpdateInputBitfield @0x7653C0 - the buttons
//  @0x7653E6..0x76551F, the hats Joystick_MapAxisToBitfield(pov, 0x80 / 0x84 / 0x88 / 0x8C)
//  @0x765535..0x76557A]
using JoystickBits = std::array<uint32_t, 5>;
JoystickBits joystick_bits(const std::function<bool(int)> &button_down,
                           const std::array<int32_t, 4> &pov_angles);
inline bool joystick_bit(const JoystickBits &bits, int bit) {
  return bit >= 0 && bit < 160 && ((bits[static_cast<std::size_t>(bit >> 5)] >> (bit & 31)) & 1u) != 0;
}


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
// modifier walks the two slots joining with the "OR" separator and prefixing
// "Ctrl-" / "Shift-" per slot; (2) a slot with a key and NO modifier RESETS
// the buffer and prints the LAST RESOLVED slot's key (the primary is resolved
// up front, then every keyed slot arm 1 visits, so the secondary when both
// are keyed) behind whichever modifier either slot carries (Ctrl / Alt /
// Shift); (3) a mouse button RESETS the buffer to "<mod>-" for the mouse
// modifier (sprintf, not strcat @0x496f79), then appends the separator when a
// keyboard slot exists, then "Mouse 1/2/3" or "Mouse Whl Up/Dn"; finally
// " *" when the record's flag word carries 0x200. Every prefix, separator
// and name is a "Keys" table lookup (key_strings.h: the shipped keyhelp.bin
// yields "Ctrl-9 or Space"; with no table installed the fallbacks render
// marker-stripped, "Ctrl - 9 or Space").
struct BindingRecord;
std::string format_display_string(const BindingRecord &rec, bool flagged = false);

class BindingSet;
// The in-game display string of the binding NAMED by a config token, the
// tip text's "$name$" expansion: the first record whose token matches
// case-insensitively, formatted as above with its own flag word's 0x200
// star; "???" when no record carries the token.
// [orig: KeyBinding_GetDisplayStringByActionName @0x5b6a00 — the stricmp walk
//  over the 768 records @0x5b6a10..0x5b6a2a, the "???" miss @0x5b6a2e, the
//  KeyBinding_FormatDisplayString call @0x5b6a46 (its star reads the
//  record's own +4 flags, `test [esi+4], 200h` @0x49715f)]
std::string display_string_for_token(const BindingSet &set, const std::string &token);

// The console's SAVEKEYS dump, keylist.key's text: a version line with the filtered
// table's count, a column header, then one line per filtered binding (the records,
// by action code, whose static flags carry 0x4000000): the filtered index, the live
// record's code word, both keys, both key modifiers, the mouse mask and modifier,
// the joystick button and modifier bytes, the code, the SEQ star (flags 0x800), the
// filter star (0x4000000), the action's "Text" help name right-justified in 27, the
// in-game display string and " Action on Key Up only" for flags 0x800000. Written
// in text mode: every line ends CR LF. The lines are fprintf'd as their own format
// strings, so a '%' in a name would be read as a conversion; no shipped name has one.
// [orig: KeyBinding_DumpAllToFile @0x54C0A0 - DeleteFileA / fopen("keylist.key",
//  "a+t") @0x54c0ba..0x54c0ca, the version line @0x54c0f9 (0x31313230 and
//  dword_2550190), the header @0x54c127, the row format @0x54c252 over
//  g_FilteredKeyBindings (KeyBinding_BuildFilteredTable @0x54C2B0, run after the
//  re-lay KeyBinding_SortBySequentialId @0x498260 in Game_InitSubsystems
//  @0x4a6de7 / @0x4a70a1), KeyBinding_FormatDisplayString @0x496BD0,
//  KeyHelp_GetStringWithFallback("Text", token, name) @0x54c1c5]
std::string keylist_text(const BindingSet &set);

struct BindingRecord {
  uint16_t primary = 0;        // slot-1 VK scan (0 = unbound)
  uint16_t secondary = 0;      // slot-2 VK scan
  uint16_t primary_mod = 0;    // slot-1 modifier VK (17 Ctrl / 16 Shift / 0)
  uint16_t secondary_mod = 0;  // slot-2 modifier VK
  uint16_t mouse_mask = 0;     // kMouse* mask (0 = unbound)
  uint16_t mouse_mod = 0;      // mouse modifier VK
  uint8_t joy_mod = 0;         // joystick modifier button
  uint8_t joy_button = 0;      // 1-based button, or POV-hat direction 0x81..0x90
};

// Whether a VK rides Windows' extended-key lParam bit (bit 24), which the
// original's queue folds into the event flag word as 0x100 — an extended key
// held with Ctrl therefore records NO modifier (the capture's compare is
// against Ctrl-held ALONE) [orig: Input_QueueKeyEvent @ 0x760c10;
// KeyBinding_HandleKeyAssignment @ 0x55bb4f].
bool is_extended_vk(int vk);

class BindingSet {
 public:
  // One record per row of `catalog` (the title's, controls::title_catalog), each
  // its row's defaults. `catalog` outlives the set (the titles' catalogs do).
  explicit BindingSet(const Catalog &catalog);

  // The catalog the records follow, row for record.
  const Catalog &catalog() const { return *catalog_; }

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

  // The joystick capture: the device's lowest set bit (a button, else a hat direction) as
  // the record's 1-based joystick byte; false with no bit set (the capture keeps waiting).
  // No conflict test, no modifier.
  // [orig: Options_UpdatePendingState @0x55C450 - Joystick_UpdateInputBitfield @0x55C6CF, the
  //  scan of 160 bits @0x55C6D4..0x55C6F8, the store of bit + 1 @0x55C720, then
  //  UI_RefreshControlMappingList @0x55C726, which ends the capture (@0x55B6E1)]
  bool assign_joystick(int index, const JoystickBits &bits);

  // Clear the record's slots for one device [orig: CLEAR_KEY @ 0x55c056
  //  (keyboard), @ 0x55c03e (mouse), @ 0x55c028 (joystick)].
  void clear(int index, Device device);

  // The Control-column text for one action on one device: the formatted
  // keyboard binding, the mouse button name, or "JOYBUTTONn"
  // [orig: UI_UpdateControlMappingDisplay @ 0x55b700].
  std::string control_text(int index, Device device) const;

  // The Class/Action/Control rows for a device from the LIVE records
  // (same visibility gate as the static build_rows)
  // [orig: UI_PopulateControlMappingList @ 0x55c0c0].
  std::vector<ControlRow> build_rows(Device device) const;

  // The catalog index behind a visible row (row order == build_rows order),
  // -1 out of range.
  int action_index_for_row(int row) const;

  // Whether the record shares a binding with another record the controls
  // screens list, which their refresh tints yellow: both eligible (flags
  // 0x4000800), neither pair of the exclusive flag pairs (0x40 against 1,
  // 0x20000000 against 0x40000000), a key slot of the other's equal to one
  // of this record's with the same modifier, or the same mouse mask and
  // modifier, or the same joystick button and modifier, and the two rows
  // sharing a mode (alive bit 1, death-screen bit 2).
  // [orig: KeyBinding_CheckWeaponSlotConflict @0x55ae60 - the gate @0x55ae79,
  //  the pair test @0x55aebc..0x55af13, the key slots @0x55af1b..0x55af77,
  //  the mouse @0x55af79..0x55af96, the joystick @0x55afae..0x55afc6;
  //  KeyBinding_ModesOverlap @0x55ae10 (the modes overlap)]
  bool shares_binding(int index) const;

  // Every VK bound to a catalog action token (primary then secondary; empty
  // when unbound). The gameplay consumer's lookup seam.
  std::vector<int> keys_for_token(const std::string &token) const;

  // The VK firing the record RIGHT NOW under the keyboard dispatcher's two
  // passes, or 0. `key_down(vk)` is the device's held state (retail's
  // g_input_key_down_states[vk], indexed by VK). Pass 1 fires a row whose
  // modifier word (either slot) is held together with either of its keys;
  // the fallback fires a row with BOTH modifier words zero, and only for a
  // key no pass-1 row takes -- retail runs the fallback for a key event only
  // when the modifier pass matched nothing. So Ctrl+1 fires seat1 and never
  // Knife, a bare 1 fires Knife, and Ctrl+B still fires binoculars (no row
  // claims B). A mixed record (Ctrl+1 primary, bare Z secondary) fires on
  // Ctrl+Z -- pass 1 checks the ROW's modifier, then either key -- and never
  // on a bare Z, since the fallback wants both modifier words zero.
  // [orig: Input_ProcessKeyboardEvents @0x49d1f0 -- pass 1 @0x49d327..0x49d3ac
  //  (modifier held @0x49d36a/@0x49d377, key match @0x49d3a7, fire and skip
  //  the fallback @0x49d42f..0x49d437); fallback @0x49d3ba..0x49d488 (both
  //  modifier words zero @0x49d3c1.., key match, fire @0x49d488)]
  //
  // An event row (no flag 4) fires FIRST MATCH: each pass queues the first
  // row of the walk that passes the scan's gates and matches the key, then
  // takes the next key event, so one key event fires one row at most. The
  // walk runs by action code (the boot's sort lays record i as the row whose
  // code is i): of two rows on one key the lower code fires (flipmouse 9
  // over mousescale 17, hudcolor 10 over huddetail 19, binoculars 26 over
  // seat1 182). The gates, for the row itself and for every row that could
  // take its key first: a 0x1 row is skipped while the scan word is set and
  // a 0x40 row while it is clear, a release-only 0x800000 row and a held
  // (flag 4) row always, and a row whose mode word lacks the live mode's bit
  // (bit 2 on the death screen, else bit 1). The class filter beside them,
  // `g_InputBindingClassFilter` against the row's class byte, is never
  // written (no store in the image), so it passes every row.
  // [orig: KeyBinding_SortBySequentialId @0x498260 (Game_InitSubsystems
  //  @0x4a6de7); Input_ProcessKeyboardEvents -- the scan word
  //  @0x49d1f3..0x49d222, the 0x1 / 0x40 gates @0x49d332..0x49d348 and
  //  @0x49d3c4..0x49d3da, 0x800000 @0x49d34a / @0x49d3dc, the class filter
  //  @0x49d352..0x49d359 / @0x49d3e4..0x49d3eb (g_InputBindingClassFilter,
  //  ex byte_B3B768, read @0x49d321), flag 4 @0x49d362 / @0x49d40a, the mode
  //  bit @0x49d380..0x49d393 / @0x49d40f..0x49d43d, the jumps to the next
  //  event @0x49d437 / @0x49d490 (no match @0x49d450)]
  // A held (flag 4) row takes neither pass: the held pass reads either of its
  // keys down, under its mode bit alone (no modifier test, no first match),
  // for a row with a nonzero code. 0 while the keyboard is captured (below).
  int pressed_key(int index, const std::function<bool(int)> &key_down) const;

  // The catalog row one dequeued key event fires, or -1: the event's scan code
  // (key_event_scan_code) against the modifier pass's first eligible row,
  // else, for an event that pass left, the fallback's. `key_down` is the
  // device's key state as the scan runs (the modifier words read it live, not
  // the event's flag word). `fallback_signed`, when given, is set for a
  // fallback fire whose queued id carries the sign bit (Shift held, or the
  // row's flag 0x200). An OS auto-repeat is an event like any press, so a held
  // key fires its row once per repeat. The open text line's take of the
  // event (the capture branch) is the drain's, ahead of this scan.
  // [orig: Input_ProcessKeyboardEvents @0x49d1f0 - per dequeued event
  //  (Input_DequeueKeyEvent, the calls @0x49d2d2 / @0x49d4a0): the modifier
  //  pass @0x49d327..0x49d3ac, its fire @0x49d42f and jump past the fallback
  //  @0x49d437; the fallback @0x49d3ba..0x49d44a, the sign @0x49d452..0x49d470,
  //  its fire @0x49d488; the capture branch @0x49d2e3 -> Chat_HandleInputChar
  //  @0x49d498]
  int event_row(int scan_code, const std::function<bool(int)> &key_down,
                bool *fallback_signed = nullptr) const;

  // The release pass's row for a key: the first row of the walk (by action
  // code) holding the key in either slot whose flags carry the release bit
  // 0x80000000 or the release-only 0x800000 (ToSpecial and attack_1 in the
  // catalog), or -1. No scan gate, no mode bit, no modifier word.
  // [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the mask 0x80800000
  //  @0x49d236, the walk @0x49d252..0x49d27f (the slots @0x49d260 /
  //  @0x49d268, the flags test @0x49d26f)]
  int release_row(int key) const;

  // The keyboard pass's release pass, ahead of its first dequeue: every key
  // whose press latch is set and whose key state reads up, in key order,
  // queues its release row's event (the sign: Shift's key state, or flag
  // 0x200 of the record the walk's second pointer reaches, the row whose code
  // is the key's index, which no catalog row carries) and clears its latch;
  // an open text line (keyboard_captured) clears the latch with no event, and
  // a key no release row holds keeps its latch. `key_state` is the device's
  // key state alone (a menu close's reset honoured; no scan gate, no special
  // key's take).
  // [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the loop @0x49d22f..
  //  0x49d2cc (the latch @0x49d240, the key state @0x49d249), Shift's state
  //  @0x49d283 or [edi] & 0x200 @0x49d28c -> 0x80000000 @0x49d298, the
  //  capture word @0x49d29f, Input_QueueEvent(sign | code) @0x49d2b1, the
  //  clear @0x49d2b9; no row found leaves the latch @0x49d281]
  std::vector<KeyRowEvent> release_pass(
      KeyPressLatch &latch, const std::function<bool(int)> &key_state) const;

  // The key scan's two live gates, which the embedder sets at the head of
  // every input frame (each logic tick's) ahead of its polls: the death
  // screen (g_DeathScreenActive) selects the
  // mode bit, and the scan word is set on a dedicated authority (in a
  // session, the authority, not a session peer) and otherwise while the
  // local body's dead bit (Flags & 2) is up.
  // [orig: Input_ProcessKeyboardEvents @0x49d1f3..0x49d222 (the word: 2, else
  //  g_LocalPlayerEntity +0x24 & 2), @0x49d380 / @0x49d40f
  //  (g_DeathScreenActive)]
  void set_scan_gates(bool death_screen, bool scan_word) {
    scan_death_screen_ = death_screen;
    scan_word_ = scan_word;
  }
  bool scan_death_screen() const { return scan_death_screen_; }

  // Whether the record's fire RIGHT NOW carries the sign bit (bit 31 of the
  // queued binding id, which steps mousescale down): the keyboard fallback
  // pass and the mouse dispatch set it while Shift (VK 16) is held; the
  // modifier pass and the joystick button queue the bare id. So a
  // modifier-less key or a held mouse button fires signed under Shift, and a
  // row fired through its held modifier (Ctrl+Shift+key) or a joystick button
  // fires unsigned. The row flag 0x200 the two signed sites also test rides no
  // catalog row, and no file reaches the flag word: player.sav's load and
  // default.key merge +24..+37 alone over the default table [orig:
  // KeyBinding_MergeSavedBindings @0x54c3f0 - @0x54c444..0x54c479, from
  // PlayerProfile_LoadAllFromDisk @0x54f60f; the default.key callback
  // @0x54bf40].
  // [orig: Input_ProcessKeyboardEvents @0x49d452..0x49d483 (the fallback's
  //  sign) vs @0x49d41e..0x49d42f (the modifier pass, unsigned);
  //  Input_TryDispatchBindingByWeaponType @0x499180..0x4991a4;
  //  Input_TryTriggerJoystickButton @0x497b30 (unsigned)]
  bool fire_signed(int index, uint16_t held_mouse_mask,
      const std::function<bool(int)> &key_down) const;

  // An open text line (the chat entry) owns the keyboard: while set, no
  // keyboard slot fires, so a token resolves from its mouse and joystick arms
  // alone (a mouse row's modifier word still reads the live key state).
  // Retail hands every key press to the line and skips the held keyboard
  // pass, but samples mouse look, the held mouse buttons, the mouse
  // click/wheel rows and the joystick unchanged.
  // [orig: Input_ProcessKeyboardEvents @0x49d2e3 -> Chat_HandleInputChar
  //  @0x49d498 (no binding dispatch, no Input_HandleSpecialKeys), the release
  //  queue skipped @0x49d29f; Input_ProcessPlayerFrame @0x49d4c0: the mouse
  //  axis/button and toggle passes gated only on the playback file @0x49d4d5,
  //  the held keyboard pass Input_ProcessAnalogBindings behind
  //  !g_InputCaptureMode @0x49d509; the click/wheel callback
  //  Input_ProcessBindings @0x4dda50 gated on the open menu @0x4dda76]
  void set_keyboard_captured(bool captured) { keyboard_captured_ = captured; }
  bool keyboard_captured() const { return keyboard_captured_; }

  // Whether the row's binding is held as the fire arm's live test reads it: a key slot down by
  // the key state, else one of the row's mouse buttons held. No modifier word, no scan gate or
  // walk, no other row's claim on the key, no text line's capture: a key shared with a row the
  // press scan reaches first still holds this row. The test's joystick leg is
  // pressed_joystick's (the embedder's device).
  // [orig: Input_IsBindingActive @0x498320 - the key slots @0x498348, the mouse word against
  //  g_MouseState.rawWParam @0x49836E, the joystick button @0x4983A0; its fire-arm call
  //  Input_HandleActionBinding_0 case 0x95 @0x4E07D9]
  bool binding_active(int index, const std::function<bool(int)> &key_state,
                      uint16_t mouse_held) const;

  // Held mouse rows are polled independently. Event rows dispatch the FIRST
  // eligible match, modified rows before unmodified, in catalog order.
  // [orig: Input_ProcessMouseAxisBindings @0x499680;
  //  Input_ProcessBindings @0x4dda50; Input_InitBindingSystem @0x499ab0]
  bool pressed_mouse(int index, uint16_t held_mask,
      const std::function<bool(int)> &key_down, bool dead = false) const;
  int mouse_event_action(uint16_t mask,
      const std::function<bool(int)> &key_down, bool dead = false) const;
  // The row a mouse button message's event queues (the click records, D-CTRL-6): the
  // window procedure maps the messages onto events (left down 2 / up 4 / double click
  // 1024, right 8 / 0x10 / 2048, middle 32 / 64 / 4096; the class's CS_DBLCLKS style sends
  // a double click's second press as its own message), and the binding walk queues the
  // first eligible row whose mouse word the event takes, on the walk's modifier order
  // (mouse_event_action's): a button-down or double click takes its button's row (mouse
  // word 1 / 2 / 16) unless the row is a held row (flag 4: the walk passes the seventh
  // argument 0); a button-up takes the left or right button's row only with the release
  // flag 0x400000, a held row's too; middle-up takes none. -1 = no row. The record carries
  // the Shift sign or the row's 0x200 bit (@0x49919E..0x4991A4; fire_signed's mouse arm).
  // The walk's head gate is dead in play: its word is the cine editor's open word, which
  // only the editor's toggle raises and the escape case lowers [orig: Input_ProcessBindings
  //  @0x4DDA50 (the gate @0x4DDA58..0x4DDA6F); CineEditor_Toggle @0x41AA20 (the store
  //  @0x41AA4F); Input_HandleActionBinding @0x49AD40 (case 18's lowering
  //  @0x49B313..0x49B323)].
  // [orig: WinMain @0x763A80 (the class style 8, CS_DBLCLKS, @0x763AFC); Game_WindowProc
  //  @0x7624C0 (Input_DispatchMouseEvent @0x762574); Input_DispatchMouseEvent @0x761470 (the
  //  messages 0x201..0x209 onto the events @0x76151A..0x76156A); Input_ProcessBindings
  //  @0x4DDA50 (event 1 leaves @0x4DDB09, the walk @0x4DDB15..0x4DDBF4, the arguments 0
  //  @0x4DDB6D..0x4DDB78); Input_TryDispatchBindingByWeaponType @0x499180 (the flag-4 test
  //  @0x4991A9..0x4991E1, the first switch @0x4991F7..0x4992B7, the second @0x4992C3..0x499327,
  //  Input_QueueEvent @0x499251)]
  int mouse_click_action(uint32_t event,
      const std::function<bool(int)> &key_down, bool dead = false) const;
  // The click record the walk queues: mouse_click_action's row (-1 none) and the record's
  // sign, the Shift key held or the row's 0x200 bit.
  // [orig: Input_TryDispatchBindingByWeaponType @0x499180 - @0x49919E..0x4991A4]
  struct MouseClickRecord {
    int index = -1;
    bool sign = false;
  };
  MouseClickRecord mouse_click_record(uint32_t event,
      const std::function<bool(int)> &key_down, bool dead = false) const;

  // Buttons 1..128 followed by four four-bit POV hats. The caller retains
  // event edges; held action rows can repeat each tick. The row's joystick
  // modifier (+34) never gates the fire: retail's dispatcher walks three
  // passes per row and the third fires the button with NO modifier test
  // [orig: Input_ProcessToggleBindings @0x499480 -- fallback @0x499594..
  //  0x4995c8 fires without a modifier test; the Scroll-Lock arm @0x49959d
  //  tests an address (lea @0x4994e5) and skips every row;
  //  Input_TryTriggerJoystickButton @0x497b30 reads only +26 @0x497b38].
  // This is the held test (Input_IsBindingActive's joystick leg); a row's
  // event fires come from the toggle pass (joystick_toggle_pass), whose
  // walk order and consumed latch arbitrate a shared button.
  bool pressed_joystick(int index, const std::function<bool(int)> &button_down,
      const std::array<int32_t, 4> &pov_angles, bool dead = false) const;

  // The joystick's whole dispatch is gated on the profile's ENABLE_JOYSTICK
  // word (+1432): with it clear the device is neither polled nor walked, so no
  // row fires from a joystick button or hat. A fresh profile carries 0, so a
  // stock game ignores the joystick until the player enables it. The session
  // copy, the controls apply and the in-game options Accept each copy the word
  // into the gate (profile::apply_controls).
  // [orig: Input_ProcessToggleBindings @0x499480 -- `cmp dword_24D2088, 0`
  //  @0x499481 returns before Joystick_UpdateInputBitfield @0x49948e;
  //  writers Game_ApplySessionSettingsToGlobals @0x5515c6, Profile_ApplySessionControls
  //  @0x563664, UI_IngameOptionsDialogEventHandler @0x55515e]
  void set_joystick_enabled(bool enabled) { joystick_enabled_ = enabled; }
  bool joystick_enabled() const { return joystick_enabled_; }
  // The first hat's four directions (bytes 0x81..0x84) take part in the toggle pass only
  // while the profile's +1512 word is set [orig: Input_InitBindingSystem @0x499AB0 - `cmp
  //  dword_24D20A0` with the 0x81..0x84 range @0x49A232; the session copy @0x551689].
  void set_joystick_hat_buttons(bool enabled) { joystick_hat_buttons_ = enabled; }

  // The input frame's joystick toggle pass: its fires, one record each in the walk's order
  // (the frame's queue takes them behind the keyboard and mouse passes'). The walk is the
  // rows with a code and a joystick byte, those with a joystick modifier first, each half
  // in catalog order (the first hat's directions only with set_joystick_hat_buttons). Each
  // row whose mode word admits it (bit 1 alive, bit 2 the death screen) tries three passes,
  // stopping at its first fire: while a keyboard modifier of its (+28 / +30) is down, while
  // its joystick modifier (+34) is down, and unless Scroll Lock is on, with none. A try fires
  // when the row's button bit is set: every pass for a row of flag 4, once per press for any
  // other, whose fire latches the button consumed until it reads up, so the first row of the
  // walk to fire a shared button takes it. A record carries no sign. Nothing runs with the
  // joystick disabled. (The analog rows' axis tries, ahead of each button try, are not
  // ported: D-CTRL-1's axis residual.)
  // [orig: Input_ProcessToggleBindings @0x499480 - the gate @0x499481, the device update
  //  Joystick_UpdateInputBitfield @0x49948E (the consumed latch's release `and` @0x765744..
  //  0x765772), the walk over g_InputToggleBindingIndices @0x4994B0, the mode word
  //  @0x4994C3..0x4994D5, the keyboard pass @0x4994E5..0x499533, the joystick-modifier pass
  //  @0x499543..0x499588, the fallback @0x499594..0x4995C8 (Input_IsScrollLockOn @0x49959D);
  //  Input_TryTriggerJoystickButton @0x497B30 - the byte @0x497B38, the bit @0x497B61, flag 4
  //  @0x497B6A, the latch @0x497B89..0x497B97, Input_QueueEvent @0x497B7A / @0x497BA9;
  //  Input_InitBindingSystem @0x499AB0 - the list @0x49A201..0x49A56C]
  std::vector<KeyRowEvent> joystick_toggle_pass(const JoystickBits &bits,
      const std::function<bool(int)> &key_down, bool scroll_lock, bool dead);

  std::size_t size() const { return records_.size(); }
  const BindingRecord *record(int index) const;
  bool set_record(int index, const BindingRecord &rec);  // persistence load
  int index_of_token(const std::string &token) const;

  // Moves on every record write (defaults, assign, clear, a persistence
  // load), so a reader that formats labels from the records (the HUDLS key
  // labels) rebuilds them only when a binding changed.
  uint32_t revision() const { return revision_; }

 private:
  // Whether catalog row `index` passes the key scan's gates (pressed_key).
  bool scan_eligible(int index) const;
  // The catalog row the key event `vk` fires in the modifier pass (`modified`)
  // or the fallback, -1 for none: the first eligible row by action code.
  int scan_first_row(int vk, bool modified,
                     const std::function<bool(int)> &key_down) const;

  // The catalog's rows and their walk by action code (the boot's re-lay of the
  // binding table: record i is the row whose code is i; the static rows'
  // codes are unique) [orig: KeyBinding_SortBySequentialId @0x498260].
  const ActionDef *catalog_rows(std::size_t *out_count) const {
    *out_count = catalog_->count;
    return catalog_->rows;
  }
  const Catalog *catalog_;
  std::vector<int> scan_order_;
  std::vector<BindingRecord> records_;  // catalog order
  bool keyboard_captured_ = false;
  bool scan_death_screen_ = false;  // g_DeathScreenActive
  bool scan_word_ = false;          // the scan word nonzero
  bool joystick_enabled_ = false;  // dword_24D2088, 0 until a profile enables it
  bool joystick_hat_buttons_ = false;  // dword_24D20A0
  JoystickBits joystick_consumed_{};   // dword_334305C
  uint32_t revision_ = 0;
};

}  // namespace opennova::controls
