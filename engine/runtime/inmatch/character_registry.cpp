#include <runtime/inmatch/character_registry.h>

#include <net/npwire/character_id.h>

using namespace opennova::avatars;

namespace opennova::npruntime {

CharacterRegistry CharacterRegistry::from_file(const AvatarsFile &file) {
	CharacterRegistry registry;
	for (size_t ni = 0; ni < file.nationalities_count; ++ni) {
		const AvatarNationality &nat = file.nationalities[ni];
		for (size_t di = 0; di < nat.divisions_count; ++di) {
			const AvatarDivision &div = nat.divisions[di];
			for (size_t ci = 0; ci < div.combos_count; ++ci) {
				const AvatarCombo &combo = div.combos[ci];
				registry.add_entry(static_cast<int>(ni), static_cast<int>(di),
						static_cast<int>(ci), nat.id, div.id, combo.id, nat.alignment,
						combo.head.voice, combo.head.sex == AVATAR_SEX_FEMALE);
			}
		}
	}
	return registry;
}

void CharacterRegistry::add_entry(int nationality_index, int division_index,
		int combo_index, int nationality_id, int division_id, int combo_id,
		int alignment, int head_voice, bool head_female) {
	CharacterEntry entry;
	entry.nationality_index = nationality_index;
	entry.division_index = division_index;
	entry.combo_index = combo_index;
	entry.nationality_id = nationality_id;
	entry.division_id = division_id;
	entry.combo_id = combo_id;
	entry.alignment = alignment;
	entry.head_voice = head_voice;
	entry.head_female = head_female;
	entry.packed_id = static_cast<uint16_t>(character_id::pack(
			nationality_id, division_id, combo_id, alignment));
	entries_.push_back(entry);
}

// [orig: MinimapSlot_FindByPackedId @0x57a270 — the decoder matches the
// nationality/division/combo numbers and the alignment bit against the combo
// entry (+0/+4/+8/+276)]
const CharacterEntry *CharacterRegistry::find_by_packed_id(uint16_t packed_id,
		int expected_alignment) const {
	const int nationality_id = character_id::nationality(packed_id);
	const int division_id = character_id::division(packed_id);
	const int combo_id = character_id::combo(packed_id);
	const int alignment = character_id::alignment(packed_id);
	if ((expected_alignment == AVATAR_ALIGN_GOOD ||
				expected_alignment == AVATAR_ALIGN_EVIL) &&
			alignment != expected_alignment)
		return nullptr;
	for (const CharacterEntry &entry : entries_) {
		if (entry.nationality_id == nationality_id &&
				entry.alignment == alignment &&
				entry.division_id == division_id && entry.combo_id == combo_id)
			return &entry;
	}
	return nullptr;
}

const CharacterEntry *CharacterRegistry::find_by_indices(int nationality_index,
		int division_index, int combo_index) const {
	for (const CharacterEntry &entry : entries_) {
		if (entry.nationality_index == nationality_index &&
				entry.division_index == division_index &&
				entry.combo_index == combo_index)
			return &entry;
	}
	return nullptr;
}

// [orig: lookup_entity_slot_and_pack_entry @0x57ad40 — the loop
// @0x57ad6b..0x57ad80 returns the first side match, the entry-0 fallback
// @0x57ad84..0x57adda]
uint16_t CharacterRegistry::first_character_id(int alignment) const {
	for (const CharacterEntry &entry : entries_) {
		if (entry.alignment == alignment) return entry.packed_id;
	}
	return entries_.empty() ? 0 : entries_.front().packed_id;
}

} // namespace opennova::npruntime
