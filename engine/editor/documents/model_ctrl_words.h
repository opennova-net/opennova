#pragma once

// What the game does with each CTRL register a model names (DI-10, the deep-integration plan's "damage
// states named and played"): the group it falls in, its words, what writes it while the game draws the
// model (the writer, cited) and the values the writer gives it. The model preview's Registers popup is
// grouped and worded from it, and its envelope's `registers` rows carry it. The catalog's own names and
// ordinals are formats/threedi/threedi_ctrl_catalog.h [orig: global control-register descriptor table @
// 0x83DCE8]; a register's value is a signed dword on the global bus, 0x10000 the exact 16.16 endpoint
// (docs/threedi/3di-gp-format-re.md, "Loader remap and runtime bus").
//
// The writers are the census of docs/threedi/3di-gp-format-re.md ("Retail writer coverage"): a register
// the game writes names its writer where a port of it stands; one with a dedicated retail writer whose
// publisher is not ported says so; one no writer reaches reads 0 in the game (the weapon action's generic
// `ctrlreg` could animate it; no shipped weapon.def does).

#include <cstdint>
#include <vector>

namespace opennova::editor {

// A register's group: its token on the wire and its title in the Registers popup, in the popup's order.
struct CtrlRegisterGroup {
	const char *token;
	const char *title;
};
const std::vector<CtrlRegisterGroup> &ctrl_register_groups();

// How the game writes a register.
enum class CtrlWriter : uint8_t {
	Ported,  // a writer of the game's the engine ports: `driven` says what it writes, `cite` where
	Open,    // the game has a dedicated writer whose publisher is not ported yet
	None,    // nothing in the game writes it: it reads 0
};
const char *ctrl_writer_token(CtrlWriter writer);

// One register's words: its group's token, its label, what writes it in the game and with what, the
// citation, and the range of the values the game gives it (the slider's reach: a share is 0 to 0x10000,
// a word's high half 0 to 0xFFFF; a register no witnessed writer bounds takes a whole share either way).
struct CtrlRegisterWords {
	const char *group = "";
	const char *label = "";
	CtrlWriter writer = CtrlWriter::None;
	const char *driven = "";
	const char *cite = "";
	int64_t min = -65536;
	int64_t max = 65536;
	// A share of 0x10000 (shown as a percent beside the word).
	bool share = false;
};
// The words of the register at `ordinal` (0..95); an ordinal outside the catalog gets an empty row.
const CtrlRegisterWords &ctrl_register_words(int ordinal);

// The six destroy-fade registers' ordinals, OBJECT_DESTROY first [orig: Entity_PublishSwapFadePhases @
// 0x5C3F40]: the damage state's (preview/model_damage) phases land on these.
bool ctrl_register_is_destroy_phase(int ordinal);

} // namespace opennova::editor
