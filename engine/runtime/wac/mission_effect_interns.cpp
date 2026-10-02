#include <runtime/wac/mission_effect_interns.h>

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>
#include <runtime/particle/effect_catalog_names.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/weapon_fsm.h>

namespace opennova::wac {

namespace {

// The material table: 78 {name, slot} pairs interned in table order.
// [orig: CEffectManager_RegisterAllMaterials @0x5F79C0 — off_8490D0, stride 8,
//  the intern @0x5f7a05]
constexpr const char *kMaterialEffectNames[78] = {
	"Effect_AirExp", "Effect_BigSplash", "Effect_BullVDirt", "Effect_BullVFlesh",
	"Effect_BullVGrass", "Effect_BullVMetal", "Effect_BullVMud", "Effect_BullVSand",
	"Effect_BullVSnow", "Effect_BullVStone", "Effect_DirtExp", "Effect_pover_bana",
	"Effect_pover_cact", "Effect_pover_cbls", "Effect_pover_mrsh", "Effect_pover_palm",
	"Effect_pover_pine", "Effect_pover_scrb", "Effect_pover_spin", "Effect_SmokePuff",
	"Effect_TreeFoliageExp", "Effect_TreeWoodExp", "Effect_WaterExp", "Effect_sboatwake",
	"Effect_sboatwakef", "Effect_snowwake", "Effect_DirtWake", "Effect_WaterSplash",
	"Effect_Boatexp_Bub2m", "Effect_Boatexp_Bub2s", "Effect_Boatexp_Bubm", "Effect_Boatexp_Bubs",
	"Effect_Boatexp_Lastbubm", "Effect_Boatexp_Lastbubs", "Effect_Boatexp_Oilm",
	"Effect_Boatexp_Shockm", "Effect_Boatexp_Shocks", "Effect_DebrisExp", "Effect_DebrisFlamel",
	"Effect_DebrisFlamem", "Effect_DebrisFlames", "Effect_DebrisSmokel", "Effect_DebrisSmokem",
	"Effect_DebrisSmokes", "Effect_DemoDust", "Effect_DemoDustM", "Effect_GiantExp",
	"Effect_ManFire", "Effect_ManFires", "Effect_MedSplash", "Effect_LargeSplash",
	"Effect_RwDust", "Effect_RwWater", "Effect_RwSnow", "Effect_RwGrass", "Effect_RwSand",
	"Effect_ShipSmoke", "Effect_SmlSplash", "Effect_VexpS", "Effect_VexpM", "Effect_VexpL",
	"Effect_VexpSL", "Effect_BurnScar", "Effect_DustBounce", "Effect_DustBounceS",
	"Effect_DustBounceF", "Effect_HeloGroundHit", "Effect_Type5Bubs", "Effect_Type6Bubs",
	"Effect_Boat01Steam", "Effect_BoatExpSec", "Effect_PDust_S", "Effect_PDust_M",
	"Effect_PDust_L", "Effect_ShockWater", "Effect_underH2O1", "Effect_shalH2O",
	"Effect_surfaceRings",
};

// The vehicle fire set [orig: VehicleEffect_InitAll @0x455C20 — @0x455c7a,
//  @0x455c89, @0x455c98, @0x455ca7].
constexpr const char *kVehicleEffectNames[4] = {
	"Effect_smkSigB", "Effect_vehicleFireLarge", "Effect_vehicleFireMed",
	"Effect_vehicleFireSmall",
};

void intern(particle::EffectCatalogNames &effects, const char *name) {
	(void)effects.intern(name);
}

// A token past the line's count stands for retail's stale or null slot; the
// interner refuses a null name [orig: @0x5f733f], so a missing value pools
// nothing here.
void intern_value(particle::EffectCatalogNames &effects, const io::ConfigTokens &tokens) {
	if (tokens.count >= 2) intern(effects, tokens.token(1));
}

// ammo.def: inside an `ammo` block, `ai_launcheffect` and `secondary_effect`
// pool their value, and an `effects_table` row (four or more tokens) pools its
// particle unless it is "none", for a known tag (1..27) the def has not yet
// defined. The tag table resets when the def closes. An `ammo` line while a
// def is open returns 1, which ends the file walk.
// [orig: File_ParseASCIIFile @0x53d942 (a nonzero callback return ends the
//  walk); AmmoDef_ParseProperty @0x40A2D0 — `ammo` @0x40a34d..0x40a397, `end`
//  @0x40a3bf..0x40a442 (the def's close resets the tags, CAIPathData_Init
//  @0x409b5d..0x409b85), `effects_table` @0x40a5a3..0x40a5b2, the row
//  @0x40a326..0x40a531, `ai_launcheffect` @0x40a8f6..0x40a91d,
//  `secondary_effect` @0x40aa0f..0x40aa36]
void scan_ammo(particle::EffectCatalogNames &effects, std::string_view text) {
	bool in_def = false;
	bool in_table = false;
	bool stopped = false;
	bool defined[world::kImpactEffectTagCount] = {};
	io::for_each_config_line(text.data(), text.size(), [&](const io::ConfigTokens &tokens) {
		if (stopped) return;
		const std::string_view key = tokens.token(0);
		if (strutil::iequals(key, "debug")) return;
		if (strutil::iequals(key, "ammo")) {
			if (in_def) stopped = true; // "definition missing end", returns 1
			in_def = true;
			return;
		}
		if (strutil::iequals(key, "end")) {
			if (!in_def) return;
			if (in_table) {
				in_table = false;
			} else {
				in_def = false;
				for (bool &tag : defined) tag = false;
			}
			return;
		}
		if (!in_def) return;
		if (in_table) {
			if (tokens.count < 4) return; // "wrong number of columns"
			const int tag = world::impact_effect_tag_index(tokens.token(0));
			if (tag < 1 || defined[tag]) return; // unknown, or "redefining the effect"
			defined[tag] = true;
			if (!strutil::iequals(tokens.token(1), "none")) intern(effects, tokens.token(1));
			return;
		}
		if (strutil::iequals(key, "effects_table")) {
			in_table = true;
		} else if (strutil::iequals(key, "ai_launcheffect") ||
				strutil::iequals(key, "secondary_effect")) {
			intern_value(effects, tokens);
		}
	});
}

// The action lines both def files forward: `action` opens a row only while
// none is open, `end` closes it, and `particle` pools its value while one is
// open. [orig: ActionDef_ParseScriptLine @0x4023C0 — `action` @0x4023f3 and
// the refusal @0x402409, `end` @0x40251b..0x402528, `particle`
// @0x402550..0x402560]
struct ActionRows {
	bool open = false;
	void line(particle::EffectCatalogNames &effects, const io::ConfigTokens &tokens) {
		const std::string_view key = tokens.token(0);
		if (strutil::iequals(key, "action")) {
			if (!open) open = true;
		} else if (strutil::iequals(key, "end")) {
			open = false;
		} else if (open && strutil::iequals(key, "particle")) {
			intern_value(effects, tokens);
		}
	}
};

bool weapon_action_named(std::string_view name) {
	for (const char *suffix : world::kWeaponActionSuffixes)
		if (strutil::iequals(name, suffix)) return true;
	return false;
}

// weapon.def: a `weapon` block forwards its lines to the action rows while an
// `action` it accepted (one of the 12 suffixes) is open; that block's `end`
// closes the row, any other `end` closes the weapon. A `weapon` line while one
// is open returns 1 and ends the walk.
// [orig: WeaponDefs_ParseLineCallback @0x543680 — `weapon` @0x54369d..0x5436fd,
//  `end` @0x54374c..0x5437dc (WeaponDefs_ResetParseState @0x53FF90), the
//  in-action forward @0x54388d..0x543899, `action` @0x5438cf..0x543945]
void scan_weapons(particle::EffectCatalogNames &effects, std::string_view text, ActionRows &rows) {
	bool in_weapon = false;
	bool in_action = false;
	bool stopped = false;
	io::for_each_config_line(text.data(), text.size(), [&](const io::ConfigTokens &tokens) {
		if (stopped) return;
		const std::string_view key = tokens.token(0);
		if (strutil::iequals(key, "weapon")) {
			if (in_weapon) stopped = true; // "weapon didn't have an end", returns 1
			in_weapon = true;
			return;
		}
		if (strutil::iequals(key, "end")) {
			if (in_action) {
				in_action = false;
				rows.line(effects, tokens);
			} else {
				in_weapon = false;
			}
			return;
		}
		if (!in_weapon) return;
		if (in_action) {
			rows.line(effects, tokens);
		} else if (strutil::iequals(key, "action") && weapon_action_named(tokens.token(1))) {
			in_action = true;
			rows.line(effects, tokens);
		}
	});
}

// powerup.def: a `powerup` block forwards its lines to the action rows while a
// `pickup` or `respawn` action is open; that block's `end` closes the row
// (when it opened one), any other `end` closes the powerup.
// [orig: PowerUpDef_ParseProperty @0x442EE0 — `powerup` @0x442f02..0x442f49,
//  the in-action forward @0x443039..0x443042, `action` @0x443056..0x4430bc,
//  `end` @0x442f73..0x442ff8 / @0x442f7b..0x442fb2]
void scan_powerups(particle::EffectCatalogNames &effects, std::string_view text, ActionRows &rows) {
	bool in_powerup = false;
	bool in_action = false;
	io::for_each_config_line(text.data(), text.size(), [&](const io::ConfigTokens &tokens) {
		const std::string_view key = tokens.token(0);
		if (strutil::iequals(key, "powerup")) {
			if (!in_powerup) in_powerup = true; // "definition missing end"
			return;
		}
		if (strutil::iequals(key, "end")) {
			if (in_action) {
				if (in_powerup && rows.open) {
					rows.line(effects, tokens);
					in_action = false;
				}
			} else {
				in_powerup = false;
			}
			return;
		}
		if (!in_powerup) return;
		if (in_action) {
			rows.line(effects, tokens);
		} else if (strutil::iequals(key, "action") &&
				(strutil::iequals(tokens.token(1), "pickup") ||
				 strutil::iequals(tokens.token(1), "respawn"))) {
			in_action = true;
			rows.line(effects, tokens);
		}
	});
}

} // namespace

void intern_mission_start_effects(particle::EffectCatalogNames &effects,
		const MissionEffectDefTexts &defs) {
	for (const char *name : kMaterialEffectNames) intern(effects, name);
	scan_ammo(effects, defs.ammo_def);
	// One action row state spans both def files [orig: g_CurrentActionDef].
	ActionRows rows;
	scan_weapons(effects, defs.weapon_def, rows);
	for (const char *name : kVehicleEffectNames) intern(effects, name);
	scan_powerups(effects, defs.powerup_def, rows);
}

} // namespace opennova::wac
