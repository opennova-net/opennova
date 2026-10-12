#include <runtime/inmatch/integrity_sources.h>

#include <base/io/ascii_config.h>
#include <formats/lwf/lwf.h>
#include <runtime/assets/asset_store.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/inmatch/integrity_ammo_image.h>
#include <runtime/inmatch/integrity_weapon_image.h>
#include <runtime/mission/runtime_boot.h>

#include <string>

namespace opennova::inmatch {

IntegritySources::AmmoRow IntegritySources::ammo_row(uint8_t wire_index, uint32_t &crc) const {
	if (!ammo_known)
		return AmmoRow::Silent;
	const int32_t index = static_cast<int8_t>(wire_index);
	if (index < 0 || static_cast<size_t>(index) >= ammo_row_crcs.size())
		return AmmoRow::OutOfRange;
	crc = ammo_row_crcs[static_cast<size_t>(index)];
	return AmmoRow::Crc;
}

bool IntegritySources::weapon_row_crc(uint8_t id, uint32_t &crc) const {
	if (!weapon_table_known || id == 0xFFu || static_cast<int32_t>(id) < weapon_named_rows)
		return false;
	crc = 0;
	return true;
}

IntegritySources compute_integrity_sources(
		const mission::BootFileSource &files, const assets::AssetStore *assets) {
	IntegritySources sources;
	if (!files.valid())
		return sources;
	std::vector<uint8_t> items_def;
	std::vector<uint8_t> ammo_def;
	if (!files.read_file("items.def", items_def) || !files.read_file("ammo.def", ammo_def))
		return sources;
	const std::vector<IntegrityItemRow> items =
			read_integrity_item_rows(items_def.data(), items_def.size());
	// The six global bank slots, each loaded if it exists, right before
	// ammo.def [orig: Game_StartMission @0x525448..0x525461; the slot names
	// audio::global_bank_chain carries].
	audio::SoundSetIndex sounds;
	int32_t bank_index = 0;
	for (const std::string &name : audio::global_bank_chain(files.expansion_name)) {
		std::vector<uint8_t> bytes;
		lwf::File bank;
		std::string error;
		if (files.read_file(name, bytes) &&
				lwf::parse_lwf_buffer(bytes.data(), bytes.size(), bank, error))
			sounds.add_bank(bank_index++, bank);
	}
	// One tokenizer state from ammo.def's count pass through weapon.def, as
	// the load's static buffer carries it [orig: Terrain_TokenizeConfigLine
	// @0x53CB60, g_TokenizeConfigLineBuffer].
	io::ConfigTokens tokens;
	const AmmoRecordTable ammo =
			build_ammo_record_table(ammo_def.data(), ammo_def.size(), items, &sounds, tokens);
	sources.ammo_known = true;
	sources.ammo_row_crcs.reserve(static_cast<size_t>(ammo.capacity));
	for (int32_t row = 0; row < ammo.capacity; ++row) {
		uint32_t crc = 0;
		(void)ammo.row_crc(row, crc);
		sources.ammo_row_crcs.push_back(crc);
	}

	std::vector<uint8_t> weapon_def;
	if (assets == nullptr || !assets->has_source() || !files.read_file("weapon.def", weapon_def))
		return sources;
	const WeaponRecordTable weapons = build_weapon_record_table(
			weapon_def.data(), weapon_def.size(), ammo, files, assets, tokens);
	if (!weapons.computable)
		return sources;
	sources.weapon_table_known = true;
	sources.weapon_table_crc = weapons.sanitized_crc();
	sources.weapon_named_rows = weapons.live_count();
	return sources;
}

} // namespace opennova::inmatch
