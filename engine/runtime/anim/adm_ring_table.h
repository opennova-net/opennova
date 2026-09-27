// The ring heads of the loaded first-person weapon tables. Retail keeps ONE
// ring-head table per loaded .adm: the first weapon (weapon.def order) naming
// an ANIMADM loads it and every later weapon naming the same file gets the
// same table back, so every weapon on one table advances the same heads. A
// slot's ring serves its row's LAST token first and then walks backward
// through the file order (a row "a" "b" "c" serves c, b, a, c, ...), and every
// duration read and every play serves the head and advances it. Slot 0 is no
// ring: each reset token replaces the head, so it serves the last reset token
// that loaded and never moves; the first reset token that loaded fills every
// slot the table does not author, so such a slot serves that reset clip.
// [orig: AnimMap_LoadAdmFile @0x40CC40 (the cached entry @0x40CD45..0x40CD5C);
//  AnimMap_RegisterBoneNode @0x40C2D0 (node->next = head @0x40C37F, tail->next
//  = node @0x40C382, table = node @0x40C385; the reset self-ring
//  @0x40C38B..0x40C38F, its backfill @0x40C39A..0x40C3E2);
//  Anim_GetDurationTicks @0x53EE10 (@0x53EE20..0x53EE26);
//  AnimMap_PlayAnimBySlot @0x40BDA0 (@0x40BDB4..0x40BDC1)]
#pragma once

#include <runtime/anim/adm_clip_index.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace opennova {
namespace assets { class AssetStore; }
}

namespace opennova::anim {

// One served ring entry: the clip the slot plays, named by the key its clip
// registered under (`anim_reset` for a slot the table fills with its reset
// clip) and its variant, the token's index in file order.
struct AdmServed {
	std::string key;
	int32_t variant = -1;
	const AdmClipFacts *clip = nullptr;
	bool valid() const { return variant >= 0; }
};

class AdmRingTable {
public:
	// Load `adm_name` (".adm" appended when missing) once: a table already
	// loaded keeps its heads. False when it holds no reset clip, which retail
	// cannot load either [orig: AnimMap_LoadAdmFile @0x40cc40, the slot-0
	// head read @0x40ce11..0x40ce16].
	bool load(const assets::AssetStore *assets, const std::string &adm_name);
	bool loaded(const std::string &adm_name) const;
	// The rings of a table no store holds: `clips` per slot key, in file
	// order. A table already loaded keeps its heads. The seam a caller that
	// brings its own clip lengths takes (a weapon installed from a definition
	// object rather than by name).
	void adopt(const std::string &adm_name,
			const std::unordered_map<std::string, std::vector<AdmClipFacts>> &clips);
	// Whether `key` names one of the 252 slots of a loaded table: the bind's
	// existence probe, which never advances [orig: AnimMap_FindSlotByName
	// @0x40cfa0, checked by Anim_InitActions @0x5421ae].
	bool resolves(const std::string &adm_name, const std::string &key) const;
	// Serve the slot `key` names and advance its head. An invalid entry when
	// the table is not loaded or the key names no slot.
	AdmServed serve(const std::string &adm_name, const std::string &key);
	// What serve would return, without advancing (diagnostics).
	AdmServed peek(const std::string &adm_name, const std::string &key) const;
	// The slot keys a loaded table authors (diagnostics), unordered.
	std::vector<std::string> keys(const std::string &adm_name) const;
	// The clock of one registered variant, without serving.
	const AdmClipFacts *clip(const std::string &adm_name, const std::string &key,
			int32_t variant) const;
	void clear();

private:
	struct Table {
		// Slot key -> the variants in file order.
		std::unordered_map<std::string, std::vector<AdmClipFacts>> rings;
		// Slot key -> serves so far: serve n takes the variant n steps back
		// from the last, modulo the ring.
		std::unordered_map<std::string, int64_t> served;
	};
	const Table *table(const std::string &adm_name) const;
	std::unordered_map<std::string, Table> tables_; // lowercased name with .adm
};

} // namespace opennova::anim
