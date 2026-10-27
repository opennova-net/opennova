#include <runtime/inmatch/character_registry.h>

#include <net/npwire/character_id.h>

using namespace opennova::avatars;

namespace opennova::inmatch {

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

// [orig: CAvatarDefs_FindComboByPackedId @0x57a270 — the decoder matches the
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

namespace {

std::array<int, 3> part_camo(const AvatarPartSnapshot &part) {
	return {part.camo[0], part.camo[1], part.camo[2]};
}

} // namespace

PlayerVisualSpec resolve_player_visual_spec(const AvatarsFile &file,
		const CharacterRegistry &registry, int character_id) {
	PlayerVisualSpec out;
	out.character_id = character_id;
	const CharacterEntry *entry =
			registry.find_by_packed_id(static_cast<uint16_t>(character_id & 0xffff));
	if (entry == nullptr || entry->nationality_index < 0 ||
			static_cast<size_t>(entry->nationality_index) >= file.nationalities_count)
		return out;
	const AvatarNationality &nat = file.nationalities[entry->nationality_index];
	if (entry->division_index < 0 ||
			static_cast<size_t>(entry->division_index) >= nat.divisions_count)
		return out;
	const AvatarDivision &div = nat.divisions[entry->division_index];
	if (entry->combo_index < 0 || static_cast<size_t>(entry->combo_index) >= div.combos_count)
		return out;
	const AvatarCombo &combo = div.combos[entry->combo_index];
	out.fallback = false;
	out.character_id = character_id & 0xffff;
	out.head = combo.head.graphic;
	out.head_camo = part_camo(combo.head);
	out.body = combo.body.graphic;
	out.body_camo = part_camo(combo.body);
	if (combo.has_arms) {
		out.arms = combo.arms.graphic;
		out.arms_camo = part_camo(combo.arms);
	}
	out.voice = combo.head.voice;
	out.sex = combo.head.sex;
	out.nationality_index = entry->nationality_index;
	out.division_index = entry->division_index;
	out.combo_index = entry->combo_index;
	return out;
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

// [orig: CAvatarDefs_PackFirstComboOfAlignment @0x57ad40 — the loop
// @0x57ad6b..0x57ad80 returns the first side match, the entry-0 fallback
// @0x57ad84..0x57adda]
uint16_t CharacterRegistry::first_character_id(int alignment) const {
	for (const CharacterEntry &entry : entries_) {
		if (entry.alignment == alignment) return entry.packed_id;
	}
	return entries_.empty() ? 0 : entries_.front().packed_id;
}

} // namespace opennova::inmatch
