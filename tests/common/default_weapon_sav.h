// The weapon.sav the original game ships beside its expansion: every record
// PlayerProfile_InitDefaults' seed [orig: PlayerProfile_InitDefaults @0x54bb40]
// with the avatar bytes its character table packs for a fresh profile
// [orig: CAvatarDefs_PackFirstComboOfAlignment @0x54bbea]: blue {0, 0, 0x0200}, red
// {7, 0, 0x8207}. Built from those values alone (no retail byte is carried);
// the pinned JO:CA install's expansion/jox01/weapon.sav (git blob
// 38c6b7d15bb7ef64fcecb39ae4ded7f4ac12833b, 337996 bytes) is these bytes, its
// FNV-1a 64 kDefaultWeaponSavFnv1a64, so a writer that reproduces the digest
// reproduces the shipped file byte for byte.
#pragma once

#include <base/io/hash.h>
#include <formats/playersav/weapon_sav.h>

#include <cstdint>
#include <vector>

namespace test_weapon_sav {

inline constexpr uint64_t kDefaultWeaponSavFnv1a64 = UINT64_C(0x8499BA1250627A88);

inline opennova::playersav::File default_file() {
	opennova::playersav::File f = opennova::playersav::make_defaults();
	for (opennova::playersav::Record &r : f.slots) {
		r.blue.avatar_a = 0;
		r.blue.avatar_b = 0;
		r.blue.avatar_packed = 0x0200;
		r.red.avatar_a = 7;
		r.red.avatar_b = 0;
		r.red.avatar_packed = 0x8207;
	}
	return f;
}

inline uint64_t fnv1a64(const std::vector<uint8_t> &bytes) {
	return opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, bytes.data(), bytes.size());
}

} // namespace test_weapon_sav
