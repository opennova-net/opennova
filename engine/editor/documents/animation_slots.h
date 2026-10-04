#pragma once

#include <cstddef>
#include <string>

namespace opennova::editor {

// The engine's 252 animation slots in a modder's words (ADR 0046 S17, the animation lane): which
// family a slot belongs to, the heading an animation map's rows stand under, and one sentence of
// what the game plays it for. The slot names are the game's own (g_AnimStateNameTable, runtime/anim/
// anim_slot_names.h); the families and the sentences are what the RE records witness of who selects
// each slot (docs/world/world-wac-ai-re.md 3.4, 14.8, 17, 19, 33.33), and a slot whose selector is
// not traced says so. Tooling words: nothing here changes what the game does.

// A slot's family: the heading its rows stand under in a map's outline, in the order the
// families are listed (its key orders them).
struct AnimSlotFamily {
	const char *key;     // "03": orders the headings
	const char *heading; // "Crouched"
};

// The family of slot `slot` (0..251); -1 past the table (a key naming no slot).
int animation_slot_family(int slot);
size_t animation_slot_family_count();
const AnimSlotFamily &animation_slot_family_row(int family);
// The heading of the rows whose key names no slot: the game skips them.
const AnimSlotFamily &animation_slot_unknown_family();

// What the game plays slot `slot` for, one sentence ("Walking forward."); "" past the table.
std::string animation_slot_meaning(int slot);

// What becomes of a slot a map leaves out. A loaded map's every unauthored slot serves the reset
// row's first clip [orig: AnimMap_RegisterBoneNode @ 0x40C2D0, slot 0's backfill @ 0x40C39A..0x40C3E2;
// AnimMap_RegisterEntity @ 0x40BB60, @ 0x40BC24..0x40BC81], but whether a body then plays that clip
// in it is its selector's say: one that tests for the slot's clip (animMap[slot] != animMap[0]) never
// picks it without one.
enum class AnimSlotAbsence {
	PlaysReset, // a selector commits it unchecked: the body plays the reset clip in it
	NotPicked,  // its selectors pick it only when the map has its clip: the game picks another
	Untraced,   // whether its selector tests for the clip is not traced
};
AnimSlotAbsence animation_slot_absence(int slot);
// The sentence ("Left out, the game never picks it: the idle plays as it is."); "" past the table.
std::string animation_slot_when_missing(int slot);
// For a slot the game never picks without its clip, what it does instead ("the idle plays as it
// is"); "" where the records say nothing of it, and for the other slots.
std::string animation_slot_instead(int slot);

// The slot index a row's key names (anim::adm_slot_index: past its first five characters, any
// case), -1 for none; and the slot's words ("walk forward").
int animation_key_slot(const std::string &key);
std::string animation_slot_words(int slot);

// The body's slots (0..239: the map a soldier's or a vehicle crew's item names) and the first
// person's (240..251: the map a weapon names, its view model's).
inline constexpr int kAnimBodySlotCount = 240;
inline constexpr int kAnimFirstPersonSlot = 240;

} // namespace opennova::editor
