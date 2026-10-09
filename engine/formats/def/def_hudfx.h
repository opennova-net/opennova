#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <formats/textlayout/text_layout.h>

namespace opennova::def {

// hudfx.def: the HUD's 3D models (docs/interface/hud-re.md, "hudfx.def"). Read at every mission's HUD init
// through the shared ASCII walk [orig: HUD_InitOverlaySystem @ 0x5A4620 -> File_ParseASCIIFile("hudfx.def",
// HUD_CacheModelNameByTag @ 0x58F970, key 0x2A5A8EAD) @ 0x5A4633], each line's first token compared without
// case against nine tags, `3DIHud` and `3DIPower1` to `3DIPower8`, its second token copied into that tag's
// 16-byte name slot [orig: the strcpy loops into byte_27232F8 + 16 * slot @ 0x58F992..0x58FB42]. A line whose
// tag is one of them returns 1, which ends the walk [orig: File_ParseASCIIFile @ 0x53D942]: of the file only
// its first such line is read. Each slot whose name is set and whose model is not yet loaded is then loaded
// [orig: ThreediGp_LoadModel @ 0x5A465F..0x5A4797]. The HUD model (3DIHud) is drawn over the first-person
// view [orig: HUD_RenderAllOverlays @ 0x5A8221, Render_SubmitEntity @ 0x5A836E], and then each power slot's
// model in a grid of tenths of the screen for its ammo pool 3 to 10 while the pool holds any (at 40 and up
// steady, below it blinking with the update counter) [orig: @ 0x5A83D1..0x5A8459, Server_GetEntitySlotValue @
// 0x5454A0], read without a test [orig: `mov ecx, [eax+20h]` @ 0x5A840A]: a power slot with no model under a
// pool that holds any is a crash, and with one line read only one slot ever has one. JO ships no hudfx.def.

inline constexpr size_t kHudFxSlots = 9;      // 3DIHud, 3DIPower1..8
inline constexpr size_t kHudFxNameBytes = 16; // each slot's bytes, contiguous [orig: byte_27232F8 + 16 * slot]

// The tag of a slot ("3DIHud", "3DIPower1" ...), null past the ninth.
const char *hudfx_tag(size_t slot);
// The slot a tag names, compared without case [orig: the stricmp chain @ 0x58F986..0x58FB22]; -1 for none.
int hudfx_slot_of(std::string_view tag);

// A line the walk would take: its slot and its model's name (the line's second token).
struct HudFxLine {
	uint8_t slot = 0;
	std::string model;
	uint64_t note = 0; // its line in the file's modeled layout (textlayout); 0: none
};

// A file's tagged lines in its order; the game reads the first alone.
struct HudFxFile {
	std::vector<HudFxLine> lines;
	uint64_t note = 0; // the file's own record in its layout; 0: none
};

// The file's tagged lines as the walk cuts them (CR LF lines, its tokens; a line of no token or opening '/'
// skipped), each line whose first token is a tag; with `notes`, its layout modeled (every other line read for
// nothing where it stands).
HudFxFile hudfx_parse(const uint8_t *data, size_t size);
HudFxFile hudfx_parse(const uint8_t *data, size_t size, textlayout::Notes &notes);

// The nine names the slots hold once the game has read the file: the first line's model copied into its slot
// and on, a name of 16 or more characters running into the slots after it, as the copy runs [orig: @
// 0x58F992..0x58FB42]; the last slot's run past the nine goes into a buffer the init clears after [orig:
// memset(&dword_2723388, 0, 0x240) @ 0x5A4941]. Each slot's name is what its load reads, to its first NUL.
std::array<std::string, kHudFxSlots> hudfx_slot_names(const HudFxFile &file);

// The file's text from scratch (ADR 0003): each line `tag<TAB>model` with CR LF after it, over the file's
// modeled layout where the file has one (each line as the file had it but for a changed tag's or model's
// words; a line put down anew after the one before it, in the writer's form; the lines in the file's model's
// order). The text is read again: one that would not read back as the file is written in the writer's form,
// `rewritten` set. False with the reason for a model the walk reads otherwise (empty, or holding a blank, a
// comma, a quote, `;` or `//`).
bool hudfx_write(const HudFxFile &file, const textlayout::Notes *notes, std::string &text, std::string &error,
                 bool *rewritten = nullptr);

} // namespace opennova::def
