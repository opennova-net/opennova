#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace opennova::np {

// A checksum source proven against one exact retail resource corpus. These are
// deliberately named profiles, not caller-supplied CRC knobs: selecting the
// wrong corpus can make a retail host disconnect the client immediately.
struct IntegrityCrcEntry {
	uint8_t index = 0;
	uint32_t crc = 0;
};

struct IntegrityChallengeProfile {
	std::string_view id;
	uint32_t weapon_slot_table_crc = 0;
	uint16_t ammo_definition_count = 0;
	const IntegrityCrcEntry *animation_definition_crcs = nullptr;
	std::size_t animation_definition_crc_count = 0;
	const IntegrityCrcEntry *ammo_definition_crcs = nullptr;
	std::size_t ammo_definition_crc_count = 0;

	bool find_animation_definition_crc(uint8_t index, uint32_t &crc_out) const;
	bool find_ammo_definition_crc(uint8_t index, uint32_t &crc_out) const;
};

// Retail's whole-WeaponSlotDef reply has one received-value sentinel: id 0xFF
// with checksum 42 succeeds before the ordinary profile-CRC/salt comparison.
// The profile source itself has no sentinel meaning.
constexpr bool weapon_integrity_reply_matches(
		uint8_t id, uint32_t source_crc, uint32_t salt,
		uint32_t received_crc) noexcept {
	return (id == 0xFFu && received_crc == 42u) ||
			(source_crc ^ salt) == received_crc;
}

inline constexpr std::string_view kRetailRevx02IntegrityProfileId =
		"retail-revx02-024f56f2-2d087374";

// Unknown ids return null. Callers must treat null and uncovered in-range
// entries as silence, never as CRC zero.
const IntegrityChallengeProfile *find_integrity_challenge_profile(
		std::string_view id);

} // namespace opennova::np
