#pragma once

#include <cstdint>
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

// One registered clip variant as its channel clocks it: the header's fps and
// frame count, and its loop flag, which is what a channel steps and wraps by
// [orig: AnimChannel_InitFromData @0x410560; AnimChannel_AdvancePlayback
// @0x40B140, the loop bit @0x40B167].
struct AdmClipFacts {
	float seconds = 0.0f; // frame_count / fps, 0 for a degenerate clip
	uint32_t fps = 0;
	uint32_t frames = 0;
	bool loop = false;
};

// Per-key clip VARIANTS of one .adm rig with their clocks: the native source
// AdmRingTable loads a weapon table's rings from (ADR 0028). Each .adm
// entry's value list is its variant set, in authored order; each variant
// resolves to a .bad whose header supplies frame_count / fps [orig:
// Anim_GetDurationTicks @ 0x53ee10 reads the same per-clip duration the slot
// heads serve]. Retail's ACCEPT chain rebuilds the slot table with no render
// dependency [orig: WeaponSlotTable_LoadAllFromDefs @ 0x5414e0].
class AdmClipIndex {
public:
	// Parse <adm_name> (".adm" appended when missing) through the mounted
	// index and read every key's variants. Returns the number of keys with at
	// least one resolvable clip; 0 on a missing/unparsable .adm.
	int load(const opennova::assets::AssetStore *assets, const std::string &adm_name);
	void clear();

	// The key's variants in .adm value order with their clocks (fps, frames,
	// loop). Every quoted token on the row is a variant of the same slot,
	// registered in file order [orig: AnimMap_ParseConfigLine @ 0x40cb60
	// loops the tokens; AnimMap_RegisterBoneNode @ 0x40c2d0 links each into
	// the slot ring]. A variant whose .bad does not load registers
	// failsafe.bad in its place, else it is skipped (adm_token_clip, the
	// registration behavior); a parsed-but-degenerate clip is 0 seconds long. nullptr when the key is unauthored
	// or fully unresolvable. The query names its slot as a row does, past its
	// first five characters without case (`ANIM_WPN_FIRE` and `xxxx_wpn_fire`
	// are anim_wpn_fire).
	const std::vector<AdmClipFacts> *clips_for(const std::string &key) const;
	// Every registered slot key (lowercased) with its variants' clocks.
	const std::unordered_map<std::string, std::vector<AdmClipFacts>> &all_clips() const {
		return clips_;
	}

private:
	// Lowercased key -> the variants' clocks.
	std::unordered_map<std::string, std::vector<AdmClipFacts>> clips_;
};

} // namespace opennova::anim
