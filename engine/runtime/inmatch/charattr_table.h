#pragma once

// The process's character-attribute table, g_CharAttr: charattr.def's sixteen class rows
// (formats/charattr/charattr.h) loaded once at boot on every peer [orig: Game_Run @ 0x4A7FE3 ->
// CharAttr_LoadFromDef @ 0x412140], and its fourteen per-property disable latches
// (g_CharAttrPropertyDisabled @ 0xA79508), which outlive every mission and every session. The latches
// gate the table's accessors; the authority raises four of them from the session's mp_No* words at each
// mission start (charattr_apply_restrictions) and tells each joiner (S2C 0x41, S2C 0x42), whose own
// table follows. Retail's IDB named these AnimMap_* and Input_*State* (renamed 2026-10-07): they are
// charattr data, not .adm animation slots or input modes. Witness record: docs/net/novaworld-net-re.md
// ("charattr.def: the table and its readers").

#include <formats/charattr/charattr.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace opennova::inmatch {

struct CharAttrTable {
	charattr::Table table;
	// Nonzero: the property's accessors answer nothing and its sets do nothing [orig:
	// CharAttr_SetPropertyAllowed @ 0x4125c0 stores `!allowed`]. The loader does not clear them.
	std::array<uint8_t, charattr::kPropertyCount> disabled{};
	// The load ran and read the file (false: no file, or one the ConfigFile reader does not load; the
	// table stays cleared, "Server ERROR! Could not load charattr definitions." [orig: @ 0x4124a5]).
	bool loaded = false;
};

// The load [orig: CharAttr_LoadFromDef @ 0x412140]: the table cleared and read from the file's bytes; the
// latches kept (the memset @ 0x412168 covers the table alone).
void charattr_load(CharAttrTable &state, const uint8_t *data, size_t size);

// The six session words (dword_24D20E8..F4) the authority's restriction step reads: game.cfg's
// mp_NoCharAbilities, mp_NoWeaponRecoil, mp_NoCrossHairSpread and mp_NoScopeDrift in a session [orig:
// Game_ApplySessionSettingsToGlobals @ 0x551E2B..0x551E4D (authority), @ 0x551EA7..0x551EC9], the player
// profile's words +0x548 / +0x54C / +0x554 / +0x550 in single player (@ 0x551F15..0x551F3F).
struct CharAttrRestrictions {
	bool no_char_abilities = false;   // g_SessionNoCharAbilities -> property 0, ATTRIBUTES
	bool no_weapon_recoil = false;    // g_SessionNoWeaponRecoil -> property 5, RECOIL_MUTE
	bool no_crosshair_spread = false; // g_SessionNoCrossHairSpread -> property 6, XHAIR_MUTE
	bool no_scope_drift = false;      // g_SessionNoScopeDrift -> property 7, SCOPE_MUTE
};

// Whether the class carries any of `mask`'s ATTRIBUTES words: no disabled ATTRIBUTES, the class's row
// active, a word of the mask set [orig: CharAttr_ClassHasAttribute @ 0x4125e0, the row (class - 1) & 0xF].
// The game asks it of Medic (8) and KnifeBonus (4) alone.
bool charattr_class_has_attribute(const CharAttrTable &state, uint8_t class_id, uint32_t mask);

// A class's camouflage item type id: property 10 (JUNGLE_CAMMO), 11 (DESERT_CAMMO) or 12 (ARCTIC_CAMMO) of
// an active row; 0 for an inactive row or any other property. No latch gates it [orig:
// CharAttr_GetCammoTypeId @ 0x4127b0].
int32_t charattr_cammo_type_id(const CharAttrTable &state, uint8_t class_id, uint8_t property);

// Property 10..13 of an active row whose latch is clear [orig: CharAttr_GetIntProperty @ 0x412800]; false
// (and `out` untouched) otherwise.
bool charattr_get_int(const CharAttrTable &state, uint8_t class_id, uint8_t property, int32_t &out);

// Property 2..12 of an active row whose latch is clear, an integer one converted [orig:
// CharAttr_GetAttributeFloat @ 0x412640]; false otherwise. No live game code reads it (an unreferenced
// getter pair and a debug page do).
bool charattr_get_float(const CharAttrTable &state, uint8_t class_id, uint8_t property, float &out);

// Property 0 (ATTRIBUTES, stored truncated to an integer) or 2..9 of an active row whose latch is clear
// [orig: CharAttr_SetProperty @ 0x412890]; false otherwise (id 1 and ids past 9 among them).
bool charattr_set_property(CharAttrTable &state, uint8_t class_id, uint8_t property, float value);

// The property's latch: raised when not allowed [orig: CharAttr_SetPropertyAllowed @ 0x4125c0]. Ids past 13
// are ignored: retail indexes past the 14 latches (a signed byte from S2C 0x41 into the table's first rows
// and the globals before the array), which only a malicious host's 0x41 can reach.
void charattr_set_property_allowed(CharAttrTable &state, uint8_t property, bool allowed);

// One property zeroed in every class 1..16, then disabled: S2C 0x41's arm and the restriction step's
// [orig: NapiNPClientMsg_CharAttrDisableProperty @ 0x4254c0; CharAttr_ApplyMpRestrictions @ 0x4247d0].
void charattr_disable_property(CharAttrTable &state, uint8_t property);

// The restriction step: properties 0, 5, 6 and 7 disabled for each set word, in that order [orig:
// CharAttr_ApplyMpRestrictions @ 0x4247d0, single player's from SinglePlayer_StartMission @ 0x561e7b; the
// same body is Server_ResetRoundCounters' tail @ 0x4FCF10, the authority's at each Game_StartMission
// @ 0x525b90].
void charattr_apply_restrictions(CharAttrTable &state, const CharAttrRestrictions &restrictions);

// The eight latches S2C 0x42 carries [orig: CharAttr_PackDisabledProperties @ 0x412550]: bit 0 property 13,
// bit 1 property 0, bit 2 property 6, bit 3 property 5, bit 4 property 7, bit 5 property 9, bit 12
// property 3, bit 13 property 4.
uint16_t charattr_pack_disabled(const CharAttrTable &state);
// Those eight latches set from the word, the others kept [orig: CharAttr_UnpackDisabledProperties
// @ 0x4124d0, from NapiNPClientMsg_CharAttrDisabledProperties @ 0x4281a0: a body short of two bytes
// unpacks 0].
void charattr_unpack_disabled(CharAttrTable &state, uint16_t word);

// The anti-cheat reply's dword: the seed XORed with the CRC of the class's 124-byte row; 0 for an
// inactive row or one whose class byte is not the class [orig: CharAttr_GetClassChecksum @ 0x412aa0 ->
// CRC_ComputeCustomTable(row, 124)]. `found` says which.
uint32_t charattr_class_checksum(const CharAttrTable &state, uint8_t class_id, uint32_t seed, bool *found = nullptr);

// The sixteen per-class ATTRIBUTES words as charattr_class_has_attribute reads them (index = class - 1):
// an active row's word while ATTRIBUTES is not disabled, else 0. World::class_attribute_flags takes them.
std::array<uint32_t, charattr::kClassCount> charattr_class_attribute_rows(const CharAttrTable &state);

} // namespace opennova::inmatch
