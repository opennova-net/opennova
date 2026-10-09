// S9 (ADR 0028): the mission boot's file-resolution rules — structural
// translations of the shell resolvers they replace; each carries its witness.
// The boot ORDER is MissionKernel::boot (ADR 0043 slice E9).
#include <runtime/mission/runtime_boot.h>
#include <runtime/mission/mission_sidecars.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>

#include <formats/aip/aip.h>
#include <formats/mission/bms_edit.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/terrain_query/terrain_field_build.h> // the tile info load behind the ADR 0020 seam

namespace opennova::mission {

BootFileSource boot_files_from_index(const ResourceIndex &index) {
    BootFileSource files;
    files.expansion_name = index.mounted_expansion();
    files.has_file = [&index](const std::string &name) { return index.has_file(name); };
    files.read_file = [&index](const std::string &name, std::vector<uint8_t> &out) {
        return index.read_file(name, out);
    };
    files.read_loose_first = [&index](const std::string &name, std::vector<uint8_t> &out) {
        return index.read_file(name, out, VfsLookupPolicy::ForceLooseFirst);
    };
    // The effect files, less the names retail's archive walk skips for a zero stamp
    // (ResourceIndex::effect_files, D-VFS-13).
    files.list_files = [&index](const std::string &extension) {
        std::vector<std::string> result;
        for (const std::string &name : index.effect_files()) {
            const auto dot = name.find_last_of('.');
            if (dot != std::string::npos && strutil::to_lower(name.substr(dot)) == extension)
                result.push_back(name);
        }
        return result;
    };
    return files;
}

DefTableRead read_weapon_defs(const BootFileSource &files, const std::string &name,
		def::DefWeaponsFile &out) {
	std::vector<uint8_t> bytes;
	if (!files.valid() || !files.read_file(name, bytes)) return DefTableRead::Missing;
	// A SIGHTS row whose texture the mount lacks is no row [orig: the sights
	// arm's FileSystem_FileExists @0x544AE2].
	const def::DefFileProbe probe = {
			[](const void *ctx, const char *file) {
				return static_cast<const BootFileSource *>(ctx)->has_file(file);
			},
			&files};
	if (def::def_parse_weapons_memory(bytes.data(), bytes.size(), &out, nullptr, &probe) != 0)
		return DefTableRead::Unreadable;
	return DefTableRead::Read;
}

DefTableRead read_ammo_table(const BootFileSource &files, const std::string &name,
		world::AmmoTable &out) {
	std::vector<uint8_t> bytes;
	if (!files.valid() || !files.read_file(name, bytes)) return DefTableRead::Missing;
	def::DefAmmoFile file = {};
	if (def::def_parse_ammo_memory(bytes.data(), bytes.size(), &file) != 0)
		return DefTableRead::Unreadable;
	out = world::build_ammo_table(file);
	def::def_free_ammo(&file);
	return DefTableRead::Read;
}

MissionTextSource resolve_mission_text(const BootFileSource &files,
		const std::string &mission_file_basename, std::vector<uint8_t> &out) {
	out.clear();
	if (!files.valid()) return MissionTextSource::kNone;
	// Preserve the original fallback [orig: TextResource_LoadMissionTextBin
	// @ 0x51ed90]: medmssn.bin is used only when <mission>.bin does not
	// exist; a present but malformed table is passed through and rejected
	// downstream without fallback. The extension and the fallback are the
	// by-name table's text row (mission_sidecars.h).
	const Sidecar &text = *sidecar_for_role("text");
	if (!mission_file_basename.empty()) {
		const std::string mission_bin = mission_file_basename + text.extension;
		if (files.has_file(mission_bin)) {
			(void)files.read_file(mission_bin, out);
			return MissionTextSource::kMission;
		}
	}
	if (files.read_file(text.fallback, out) && !out.empty())
		return MissionTextSource::kFallback;
	out.clear();
	return MissionTextSource::kNone;
}

std::string read_placed_tiles(const BootFileSource &files, const std::string &mission_file,
		const bms::File &mission, std::vector<uint8_t> &out) {
	const MissionInfo info = mission_info(mission);
	return read_placed_tiles(files, mission_file, info.terrain, info.environment, out);
}

std::string read_placed_tiles(const BootFileSource &files, const std::string &mission_file,
		const std::string &terrain_name, const std::string &environment, std::vector<uint8_t> &out) {
	const std::string own = mission_file.empty()
			? std::string()
			: sidecar_name(mission_file, *sidecar_for_role("tiles"));
	const terrain::TerrainFileReader read_loose = [&files](const std::string &name,
				std::vector<uint8_t> &bytes) { return files.read_loose(name, bytes); };
	return terrain::read_placed_tile_bytes(read_loose, files.read_file, own, terrain_name,
			environment, out);
}

std::vector<PromoteOptions::AiProfileRow> resolve_ai_profiles(
		const BootFileSource &files, const bms::File &mission,
		const std::function<PromoteOptions::AiProfileDefaults(int32_t)> &
				ai_profile_defaults) {
	std::vector<PromoteOptions::AiProfileRow> rows;
	if (!files.valid()) return rows;
	auto have = [&rows](const std::string &profile) {
		for (const PromoteOptions::AiProfileRow &r : rows)
			if (r.profile == profile) return true;
		return false;
	};
	// The same entity walk order as the shell resolver it replaces (markers,
	// items, buildings, organics) — first occurrence wins the dedup, so the
	// order is semantic.
	struct Group { const std::vector<bms::Entity> *entities; bool placed_item; };
	const Group groups[] = {
			{&mission.markers, false}, {&mission.items, true},
			{&mission.buildings, false}, {&mission.organics, false}};
	for (const Group &group : groups) {
		for (const bms::Entity &entity : *group.entities) {
			// The name retail's AI init would load for this record: the
			// ai_textfile, else (a placed vehicle item) the def's default_aip
			// or "helo1" — lowercase; first occurrence wins.
			const std::string profile = ai_profile_name_for(
					entity, group.placed_item, ai_profile_defaults);
			if (profile.empty() || have(profile)) continue;
			const std::string file_name = profile + ".aip";
			if (!files.has_file(file_name)) continue;
			std::vector<uint8_t> text;
			if (!files.read_file(file_name, text) || text.empty()) continue;
			// The parse itself is format knowledge (engine/formats/aip);
			// this resolver owns only the profile walk and the install row.
			aip::Profile parsed = aip::parse_profile(text.data(), text.size());
			// A file that parsed no witnessed field contributes no row, like
			// the speeds-only resolver this extends (its dictionary stayed
			// empty for such files).
			if (parsed.type == 0 && parsed.patrol_speed == -1 &&
					parsed.combat_speed == -1)
				continue;
			PromoteOptions::AiProfileRow row;
			row.profile = profile;
			row.data = std::move(parsed);
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

} // namespace opennova::mission
