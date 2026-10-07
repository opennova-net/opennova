// The process's g_CharAttr table and its disable latches (charattr_table.h).

#include <runtime/inmatch/charattr_table.h>

#include <base/io/crc32_mpeg2.h>
#include <base/io/crt_ftol.h>

namespace opennova::inmatch {

namespace {

// The row a class selects: (class - 1) & 0x0F, so class 0 is row 15 [orig: the selector in every accessor,
// e.g. CharAttr_ClassHasAttribute @ 0x4125e4..0x4125e6].
const charattr::ClassRow &row_of(const CharAttrTable &state, uint8_t class_id) {
	return state.table.rows[static_cast<uint8_t>(class_id - 1u) & 0x0Fu];
}
charattr::ClassRow &row_of(CharAttrTable &state, uint8_t class_id) {
	return state.table.rows[static_cast<uint8_t>(class_id - 1u) & 0x0Fu];
}

bool disabled(const CharAttrTable &state, uint8_t property) {
	return property < charattr::kPropertyCount && state.disabled[property] != 0;
}

float *real_field(charattr::ClassRow &row, uint8_t property) {
	switch (property) {
		case charattr::kStealth: return &row.stealth;           // +8
		case charattr::kHpBonus: return &row.hp_bonus;          // +12
		case charattr::kManaBonus: return &row.mana_bonus;      // +16
		case charattr::kRecoilMute: return &row.recoil_mute;    // +20
		case charattr::kXhairMute: return &row.xhair_mute;      // +28
		case charattr::kScopeMute: return &row.scope_mute;      // +36
		case charattr::kXhairDxMute: return &row.xhairdx_mute;  // +32
		case charattr::kReloadMute: return &row.reload_mute;    // +24
		default: return nullptr;
	}
}

const int32_t *integer_field(const charattr::ClassRow &row, uint8_t property) {
	switch (property) {
		case charattr::kJungleCammo: return &row.jungle_cammo;  // +44
		case charattr::kDesertCammo: return &row.desert_cammo;  // +48
		case charattr::kArcticCammo: return &row.arctic_cammo;  // +52
		case charattr::kRunModifier: return &row.run_modifier;  // +56
		default: return nullptr;
	}
}

} // namespace

void charattr_load(CharAttrTable &state, const uint8_t *data, size_t size) {
	state.loaded = charattr::read_table(data, size, state.table);
}

bool charattr_class_has_attribute(const CharAttrTable &state, uint8_t class_id, uint32_t mask) {
	// [orig: CharAttr_ClassHasAttribute @ 0x4125e0 -- the ATTRIBUTES latch @ 0x4125e8, the active dword
	//  @ 0x4125fa, the mask over +0x28 @ 0x412603..0x412609]
	if (disabled(state, charattr::kAttributes)) return false;
	const charattr::ClassRow &row = row_of(state, class_id);
	return row.active && (row.attributes & mask) != 0;
}

int32_t charattr_cammo_type_id(const CharAttrTable &state, uint8_t class_id, uint8_t property) {
	// [orig: CharAttr_GetCammoTypeId @ 0x4127b0 -- inactive @ 0x4127c5, 10/11/12 @ 0x4127d2..0x4127dc,
	//  any other id 0 @ 0x4127c9]
	const charattr::ClassRow &row = row_of(state, class_id);
	if (!row.active) return 0;
	switch (property) {
		case charattr::kJungleCammo: return row.jungle_cammo;
		case charattr::kDesertCammo: return row.desert_cammo;
		case charattr::kArcticCammo: return row.arctic_cammo;
		default: return 0;
	}
}

bool charattr_get_int(const CharAttrTable &state, uint8_t class_id, uint8_t property, int32_t &out) {
	// [orig: CharAttr_GetIntProperty @ 0x412800 -- the active and latch gate @ 0x41281c, 10..13 @ 0x41282e]
	const charattr::ClassRow &row = row_of(state, class_id);
	if (!row.active || disabled(state, property)) return false;
	const int32_t *field = integer_field(row, property);
	if (field == nullptr) return false;
	out = *field;
	return true;
}

bool charattr_get_float(const CharAttrTable &state, uint8_t class_id, uint8_t property, float &out) {
	// [orig: CharAttr_GetAttributeFloat @ 0x412640 -- the gate @ 0x412660, 2..9 the floats, 10..12 the
	//  camouflage ids as unsigned integers @ 0x41272b..0x412771, any other id 0 @ 0x412779]
	const charattr::ClassRow &row = row_of(state, class_id);
	if (!row.active || disabled(state, property)) return false;
	charattr::ClassRow copy = row;
	if (const float *field = real_field(copy, property)) {
		out = *field;
		return true;
	}
	if (property >= charattr::kJungleCammo && property <= charattr::kArcticCammo) {
		out = static_cast<float>(static_cast<uint32_t>(*integer_field(row, property)));
		return true;
	}
	return false;
}

bool charattr_set_property(CharAttrTable &state, uint8_t class_id, uint8_t property, float value) {
	// [orig: CharAttr_SetProperty @ 0x412890 -- the gate @ 0x4128b3; property 0 through `fistp qword` with
	//  the truncating control word, its low dword @ 0x4128d1..0x4128ef; the floats' arms 2 @0x41290b,
	//  3 @0x41291e, 4 @0x412931, 5 @0x412944, 6 @0x412957, 7 @0x41297d, 8 @0x41296a, 9 @0x412990;
	//  1 and past 9 the default arm @ 0x41299a]
	charattr::ClassRow &row = row_of(state, class_id);
	if (!row.active || disabled(state, property)) return false;
	if (property == charattr::kAttributes) {
		row.attributes = static_cast<uint32_t>(io::retail_fistp_truncate_low_dword(static_cast<double>(value)));
		return true;
	}
	float *field = real_field(row, property);
	if (field == nullptr) return false;
	*field = value;
	return true;
}

void charattr_set_property_allowed(CharAttrTable &state, uint8_t property, bool allowed) {
	// [orig: CharAttr_SetPropertyAllowed @ 0x4125c0 -- latch = !allowed]
	if (property < charattr::kPropertyCount) state.disabled[property] = allowed ? 0 : 1;
}

void charattr_disable_property(CharAttrTable &state, uint8_t property) {
	// Classes 1 to 16, each set to 0.0, then the latch [orig: NapiNPClientMsg_CharAttrDisableProperty
	// @ 0x4254c0 -- the loop @ 0x4254e2..0x4254ef, the latch @ 0x42550d].
	for (int class_id = 1; class_id <= static_cast<int>(charattr::kClassCount); ++class_id)
		charattr_set_property(state, static_cast<uint8_t>(class_id), property, 0.0f);
	charattr_set_property_allowed(state, property, false);
}

void charattr_apply_restrictions(CharAttrTable &state, const CharAttrRestrictions &restrictions) {
	// [orig: CharAttr_ApplyMpRestrictions @ 0x4247d0 -- g_SessionNoCharAbilities @ 0x4247d0 (property 0),
	//  g_SessionNoWeaponRecoil @ 0x424805 (5), g_SessionNoCrossHairSpread @ 0x424838 (6),
	//  g_SessionNoScopeDrift @ 0x42486b (7)]
	if (restrictions.no_char_abilities) charattr_disable_property(state, charattr::kAttributes);
	if (restrictions.no_weapon_recoil) charattr_disable_property(state, charattr::kRecoilMute);
	if (restrictions.no_crosshair_spread) charattr_disable_property(state, charattr::kXhairMute);
	if (restrictions.no_scope_drift) charattr_disable_property(state, charattr::kScopeMute);
}

namespace {

// The S2C 0x42 word's bits and the latch each carries [orig: CharAttr_PackDisabledProperties @ 0x412550,
// CharAttr_UnpackDisabledProperties @ 0x4124d0].
struct PackedLatch {
	uint16_t bit;
	uint8_t property;
};
constexpr PackedLatch kPacked[] = {
		{0x0001, charattr::kRunModifier}, {0x0002, charattr::kAttributes}, {0x0004, charattr::kXhairMute},
		{0x0008, charattr::kRecoilMute},  {0x0010, charattr::kScopeMute},  {0x0020, charattr::kReloadMute},
		{0x1000, charattr::kHpBonus},     {0x2000, charattr::kManaBonus},
};

} // namespace

uint16_t charattr_pack_disabled(const CharAttrTable &state) {
	uint16_t word = 0;
	for (const PackedLatch &latch : kPacked)
		if (state.disabled[latch.property] != 0) word = static_cast<uint16_t>(word | latch.bit);
	return word;
}

void charattr_unpack_disabled(CharAttrTable &state, uint16_t word) {
	for (const PackedLatch &latch : kPacked) state.disabled[latch.property] = (word & latch.bit) != 0 ? 1 : 0;
}

uint32_t charattr_class_checksum(const CharAttrTable &state, uint8_t class_id, uint32_t seed, bool *found) {
	// [orig: CharAttr_GetClassChecksum @ 0x412aa0 -- the active dword and the class byte @ 0x412ac8, the
	//  zero arm @ 0x412abf, seed ^ CRC_ComputeCustomTable(row, 124) @ 0x412ad2]
	const charattr::ClassRow &row = row_of(state, class_id);
	const bool match = row.active && row.class_id == class_id;
	if (found != nullptr) *found = match;
	if (!match) return 0;
	const std::array<uint8_t, charattr::kRowBytes> bytes = charattr::row_bytes(row);
	return seed ^ io::crc32_mpeg2_update(io::kCrc32Mpeg2Init, bytes.data(), bytes.size());
}

std::array<uint32_t, charattr::kClassCount> charattr_class_attribute_rows(const CharAttrTable &state) {
	std::array<uint32_t, charattr::kClassCount> out{};
	if (disabled(state, charattr::kAttributes)) return out;
	for (size_t i = 0; i < charattr::kClassCount; ++i)
		out[i] = state.table.rows[i].active ? state.table.rows[i].attributes : 0u;
	return out;
}

} // namespace opennova::inmatch
