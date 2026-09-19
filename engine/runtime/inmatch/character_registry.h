#pragma once

// The character registry: the Avatars.def combo tree flattened into the
// wire-identity rows retail's registry walks own — each combo's packed
// character id (npwire/character_id.h: authored nationality bits 0..4,
// division 5..8, combo 9..14, alignment 15), its tree indices, and the head
// part's voice (the ClientAuth avatar byte) and sex. Built in file order —
// from a parsed AvatarsFile, or entry by entry by an embedder that holds the
// tree in its own model; every lookup here is a witnessed registry walk.

#include <formats/avatars/avatars.h>

#include <cstdint>
#include <vector>

namespace opennova::inmatch {

struct CharacterEntry {
	int nationality_index = -1; // tree indices into the parsed file
	int division_index = -1;
	int combo_index = -1;
	int nationality_id = 0; // the authored numbers the packed id carries
	int division_id = 0;
	int combo_id = 0;
	int alignment = 0; // AvatarAlignment
	int head_voice = 0; // the head part's `voice` (the avatar byte)
	bool head_female = false;
	uint16_t packed_id = 0;
};

class CharacterRegistry {
public:
	// Flatten the parsed file: nationality -> division -> combo, file order.
	static CharacterRegistry from_file(const opennova::avatars::AvatarsFile &file);

	// Append one combo (tree order); the packed id is derived here.
	void add_entry(int nationality_index, int division_index, int combo_index,
			int nationality_id, int division_id, int combo_id, int alignment,
			int head_voice, bool head_female);

	bool empty() const { return entries_.empty(); }
	const std::vector<CharacterEntry> &entries() const { return entries_; }

	// Decode a packed id against the registry: the authored
	// nationality/division/combo numbers AND the alignment bit must all match
	// the entry (a side-B id never matches a good entry and vice versa); when
	// `expected_alignment` is 0/1 an id whose bit contradicts it resolves to
	// nothing. [orig: MinimapSlot_FindByPackedId @0x57a270; packer
	// lookup_entity_slot_and_pack_entry @0x57ad40 (@0x57ae47)]
	const CharacterEntry *find_by_packed_id(uint16_t packed_id,
			int expected_alignment = -1) const;

	// The entry at the given tree indices, or null when out of range.
	const CharacterEntry *find_by_indices(int nationality_index,
			int division_index, int combo_index) const;

	// Retail's per-side default: the first entry (file order) whose alignment
	// matches the side; no side match packs entry 0; an empty registry packs
	// 0. The fresh-profile seed AND the reallocation an unknown id gets at
	// session start / on the client 0x0C fold. [orig:
	// lookup_entity_slot_and_pack_entry @0x57ad40 (loop @0x57ad6b..0x57ad80,
	// entry-0 fallback @0x57ad84..0x57adda); callers
	// PlayerSession_InitFromProfile @0x50cada/@0x50cb08, NapiNPClientMsg 0x0C
	// @0x42eafb]
	uint16_t first_character_id(int alignment) const;

private:
	std::vector<CharacterEntry> entries_;
};

} // namespace opennova::inmatch
