// The weapon.def ACTION anim keys against the shipped weapon .adm files, as the
// bind resolves them: a key resolves when its table loads (it holds a reset
// clip) and the key names one of the 252 anim slots, whether or not the table
// authors that slot. An authored slot bakes an 'auto' delay from its ring (a
// one-clip ring from that clip, whatever read came first); a slot the table
// does not author serves the table's first reset clip, so its 'auto' delay
// bakes that clip's length; a key naming no slot, or a table that does not
// load (SPAS12's spas_1st set), bakes as no-anim with 'auto' collapsed to 0
// (D-WPN-26). The unauthored and unresolved sets are reported, not judged:
// they are the data's, not the engine's.
// [orig: Anim_InitActions @0x541FA0 -- existence is the AnimMap_FindSlotByName
//  lookup @0x5421AE, the reads @0x5421C5 / @0x5421D8, the collapses @0x542152,
//  @0x542180, @0x542202; AnimMap_RegisterBoneNode's reset backfill
//  @0x40C39A..0x40C3E2]
// Gated on OPENNOVA_JO_ASSETS (an extracted JO tree carrying weapon.def and the
// weapon .adm files).
#include "common/retail_paths.h"

#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <base/vfs/vfs.h>
#include <formats/def/def.h>
#include <runtime/world/weapon_table_build.h>
#include <runtime/anim/adm_clip_index.h>
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
	opennova::assets::AssetStore index_assets{&index};
	if (!index.scan(assets) && !index.scan(assets, std::string(), VfsMountMode::LooseOnly))
		return retail::skip("a mountable OPENNOVA_JO_ASSETS tree");
	std::vector<uint8_t> bytes;
	if (!index.read_file("weapon.def", bytes)) return retail::skip("weapon.def under OPENNOVA_JO_ASSETS");
	DefWeaponsFile file{};
	if (def_parse_weapons_memory(bytes.data(), bytes.size(), &file) != 0)
		return retail::skip("a parseable weapon.def");
	const world::WeaponTable table = world::build_weapon_table(file, &index_assets);
	expect(!table.empty(), "the weapon table bakes");

	int weapons = 0, keyed = 0, authored = 0, backfilled = 0, unresolved = 0, collapsed = 0;
	int auto_checked = 0;
	for (size_t i = 0; i < file.count; ++i) {
		const DefWeaponDef &d = file.entries[i];
		if (d.animadm[0] == '\0') continue;
		++weapons;
		const int table_index = table.index_of(d.weapon_name);
		const world::WeaponTableEntry *entry =
				table_index >= 0 ? table.by_index(static_cast<uint8_t>(table_index)) : nullptr;
		// The file the load resolved: default.adm for one the roots lack.
		const std::string animadm = entry != nullptr ? entry->animadm : std::string(d.animadm);
		anim::AdmClipIndex clips;
		clips.load(&index_assets, animadm);
		const std::vector<anim::AdmClipFacts> *reset = clips.clips_for("anim_reset");
		const bool loads = reset != nullptr && !reset->empty();
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
			if (!loads || anim::adm_slot_index(act.anim) < 0) {
				++unresolved;
				std::printf("weapon_action_clips: %s action %s anim %s resolves no slot of %s\n",
						d.weapon_name, act.name, act.anim, d.animadm);
				if (baked == nullptr) continue;
				if (baked->has_anim) {
					std::fprintf(stderr, "FAIL: %s action %s baked an anim for %s\n", d.weapon_name,
							act.name, act.anim);
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
				continue;
			}
			// A slot the table does not author holds the first reset clip.
			const std::vector<anim::AdmClipFacts> *lengths = clips.clips_for(act.anim);
			const bool is_authored = lengths != nullptr && !lengths->empty();
			if (is_authored) {
				++authored;
			} else {
				++backfilled;
				lengths = reset;
				std::printf("weapon_action_clips: %s action %s anim %s is not in %s: its reset clip\n",
						d.weapon_name, act.name, act.anim, d.animadm);
			}
			if (baked == nullptr) continue;
			if (!baked->has_anim) {
				std::fprintf(stderr, "FAIL: %s action %s (%s) resolves in %s but baked no anim\n",
						d.weapon_name, act.name, act.anim, d.animadm);
				++failures;
			}
			// The rings are one table per file, read by every def naming it in
			// weapon.def order, so only a one-clip ring (or the reset backfill,
			// which never moves) pins its 'auto' delay here; the shared read
			// order is anim_adm_ring_table's.
			if (act.delaystart != -1 || (is_authored && lengths->size() != 1)) continue;
			const float seconds = is_authored ? lengths->front().seconds : reset->front().seconds;
			const int32_t want =
					world::weapon_anim_ticks_from_ms(static_cast<int32_t>(seconds * 1000.0f));
			++auto_checked;
			if (baked->delay_start != want) {
				std::fprintf(stderr, "FAIL: %s action %s (%s) baked delaystart %d, the clip says %d\n",
						d.weapon_name, act.name, act.anim, baked->delay_start, want);
				++failures;
			}
		}
	}
	std::printf("weapon_action_clips: %zu weapons (%d with an animadm), %d keyed actions: %d on an "
				"authored slot, %d on the reset backfill, %d unresolved (%d 'auto' delays collapsed "
				"to 0); %d 'auto' delays pinned against their clip\n",
			file.count, weapons, keyed, authored, backfilled, unresolved, collapsed, auto_checked);
	expect(weapons > 0, "weapon.def names weapon .adm files");
	expect(keyed > 0, "weapon.def actions name anim keys");
	expect(authored > keyed / 2, "most weapon.def ACTION anim keys name a slot their animadm authors");
	expect(auto_checked > 0, "at least one 'auto' delay was pinned against its clip length");
	def_free_weapons(&file);
	if (failures == 0) std::printf("weapon_action_clips: OK\n");
	return failures == 0 ? 0 : 1;
}
