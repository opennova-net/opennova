#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opennova {
namespace assets { class AssetStore; }
}

namespace opennova::anim {

// The anim slot a table row's key names: its tail past the first five
// characters, without case, among the 252 slot names; -1 when it names none,
// and such a row registers nothing: it never plays and serves no duration.
// [orig: AnimMap_FindSlotByName @0x40cfa0 (the -1 miss @0x40cfce);
//  AnimMap_ParseConfigLine @0x40cb60 registers only a found slot @0x40cba4]
int adm_slot_index(std::string_view key);

// Per-key clip VARIANT lengths for one .adm rig, in seconds — the native
// source the weapon action FSM bakes its 'auto' delays and variant rings
// from (ADR 0028). Each .adm entry's value list is its variant set, in
// authored order; each variant resolves to a .bad whose header supplies
// frame_count / fps [orig: Anim_GetDurationTicks @ 0x53ee10 reads the same
// per-clip duration the slot heads serve]. This replaces the render
// skeletal's get_clip_variant_lengths feed, so the install stops being
// hostage to the FP model load — retail's ACCEPT chain rebuilds the slot
// table with no render dependency [orig: WeaponSlotTable_LoadAllFromDefs
// @ 0x5414e0].
class AdmClipIndex {
public:
	// Parse <adm_name> (".adm" appended when missing) through the mounted
	// index and bake every key's variant lengths. Returns the number of keys
	// with at least one resolvable clip; 0 on a missing/unparsable .adm.
	int load(const opennova::assets::AssetStore *assets, const std::string &adm_name);
	void clear();

	bool loaded() const { return !lengths_.empty(); }
	const std::string &adm_name() const { return adm_name_; }

	// The key's variant lengths in .adm value order (seconds). Every quoted
	// token on the row is a variant of the same slot, registered in file order
	// [orig: AnimMap_ParseConfigLine @ 0x40cb60 loops the tokens;
	// AnimMap_RegisterBoneNode @ 0x40c2d0 links each into the slot ring].
	// A variant whose .bad is missing is SKIPPED (continue-on-failure, the
	// registration behavior); a parsed-but-degenerate clip contributes 0.0.
	// nullptr when the key is unauthored or fully unresolvable. The query
	// names its slot as a row does, past its first five characters without
	// case (`ANIM_WPN_FIRE` and `xxxx_wpn_fire` are anim_wpn_fire).
	const std::vector<float> *lengths_for(const std::string &key) const;

private:
	std::string adm_name_;
	// Lowercased key -> per-variant seconds.
	std::unordered_map<std::string, std::vector<float>> lengths_;
};

} // namespace opennova::anim
