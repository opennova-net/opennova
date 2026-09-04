// The weapon.def ACTION anim keys against the shipped weapon .adm files: every
// key that resolves bakes into the action table with its clip, and an 'auto'
// delay bakes to that clip's length in ticks; a key the animadm does not carry
// (retail ships several: SPAS12's spas_1st set, the emplaced mortar's reload,
// ...) bakes as no-anim with an 'auto' delay collapsed to 0 (D-WPN-26). The
// missing set is reported, not judged — it is the data's, not the engine's.
// Gated on OPENNOVA_JO_ASSETS (an extracted JO tree carrying weapon.def and the
// weapon .adm files).
#include "common/retail_paths.h"

#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/def/def.h>
#include <runtime/world/weapon_table_build.h>
#include <runtime/simassets/adm_clip_index.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_table.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace opennova::def;

namespace {

using namespace opennova;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

std::string lower(const char *s) {
	std::string out(s != nullptr ? s : "");
	for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
			"OPENNOVA_JO_ASSETS (an extracted JO tree carrying weapon.def)");
	ResourceIndex index;
	if (!index.scan(assets) && !index.scan(assets, std::string(), VfsMountMode::LooseOnly))
		return retail::skip("a mountable OPENNOVA_JO_ASSETS tree");
	std::vector<uint8_t> bytes;
	if (!index.read_file("weapon.def", bytes)) return retail::skip("weapon.def under OPENNOVA_JO_ASSETS");
	DefWeaponsFile file{};
	if (def_parse_weapons_memory(bytes.data(), bytes.size(), &file) != 0)
		return retail::skip("a parseable weapon.def");
	const world::WeaponTable table = world::build_weapon_table(file, &index);
	expect(!table.empty(), "the weapon table bakes");

	int weapons = 0, keyed = 0, resolved = 0, missing = 0, auto_rows = 0, auto_checked = 0, collapsed = 0;
	for (size_t i = 0; i < file.count; ++i) {
		const DefWeaponDef &d = file.entries[i];
		if (d.animadm[0] == '\0') continue;
		++weapons;
		simassets::AdmClipIndex clips;
		clips.load(&index, d.animadm);
		std::map<std::string, int> uses;
		for (size_t a = 0; a < d.actions_count; ++a)
			if (d.actions[a].anim[0] != '\0') ++uses[lower(d.actions[a].anim)];
		const int table_index = table.index_of(d.weapon_name);
		const world::WeaponTableEntry *entry =
				table_index >= 0 ? table.by_index(static_cast<uint8_t>(table_index)) : nullptr;
		for (size_t a = 0; a < d.actions_count; ++a) {
			const DefWeaponAction &act = d.actions[a];
			if (act.anim[0] == '\0') continue;
			++keyed;
			int id = -1;
			for (int i = 0; i < world::weapon_action::kCount; ++i)
				if (lower(world::kWeaponActionSuffixes[i]) == lower(act.name)) {
					id = i;
					break;
				}
			const world::WeaponFsmAction *baked =
					(entry != nullptr && id >= 0) ? &entry->action_fsm.actions[id] : nullptr;
			const std::vector<float> *lengths = clips.lengths_for(act.anim);
			if (lengths == nullptr || lengths->empty()) {
				++missing;
				std::printf("weapon_action_clips: %s action %s anim %s missing in %s\n",
						d.weapon_name, act.name, act.anim, d.animadm);
				if (baked != nullptr) {
					if (baked->has_anim) {
						std::fprintf(stderr, "FAIL: %s action %s baked an anim for the missing key %s\n",
								d.weapon_name, act.name, act.anim);
						++failures;
					}
					if (act.delaystart == -1) {
						++collapsed;
						if (baked->delay_start != 0) {
							std::fprintf(stderr, "FAIL: %s action %s 'auto' delaystart baked %d, not 0\n",
									d.weapon_name, act.name, baked->delay_start);
							++failures;
						}
					}
				}
				continue;
			}
			++resolved;
			if (baked != nullptr && !baked->has_anim) {
				std::fprintf(stderr, "FAIL: %s action %s (%s) resolved in %s but baked no anim\n",
						d.weapon_name, act.name, act.anim, d.animadm);
				++failures;
			}
			if (act.delaystart != -1 || baked == nullptr) continue;
			++auto_rows;
			// A key served once bakes its ring head (variant 0); shared keys rotate
			// the ring across the definition and are not pinned here.
			if (uses[lower(act.anim)] != 1) continue;
			const int32_t want = world::weapon_anim_ticks_from_ms(
					static_cast<int32_t>((*lengths)[0] * 1000.0f));
			++auto_checked;
			if (baked->delay_start != want) {
				std::fprintf(stderr, "FAIL: %s action %s (%s) baked delaystart %d, clip says %d\n",
						d.weapon_name, act.name, act.anim, baked->delay_start, want);
				++failures;
			}
		}
	}
	std::printf("weapon_action_clips: %zu weapons (%d with an animadm), %d keyed actions, %d resolved, "
				"%d missing (%d 'auto' delays collapsed to 0), %d auto rows (%d pinned against the clip length)\n",
			file.count, weapons, keyed, resolved, missing, collapsed, auto_rows, auto_checked);
	expect(weapons > 0, "weapon.def names weapon .adm files");
	expect(keyed > 0, "weapon.def actions name anim keys");
	expect(resolved > keyed / 2, "most weapon.def ACTION anim keys resolve in their animadm");
	expect(auto_checked > 0, "at least one 'auto' delay was pinned against its clip length");
	def_free_weapons(&file);
	if (failures == 0) std::printf("weapon_action_clips: OK\n");
	return failures == 0 ? 0 : 1;
}
