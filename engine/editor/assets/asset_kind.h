#pragma once

#include <string>

namespace opennova::editor {

// What a project file IS to the engine (ADR 0046 d6/d7). The runtime catalog's kind
// vocabulary (`resource_kind_for_name_and_magic`, engine/base/resource_index) is the
// shared source for every kind the runtime browses; this enum adds the kinds the
// editor must also know (the name-keyed .def family, scripts, textures, banks,
// videos, plain text) so a requirement row and a scanned file compare by one type.
enum class AssetKind {
	Unknown = 0,
	Archive,        // .pff (never a project asset: a build output)
	Model,          // .3di
	Animation,      // .bad
	AnimationMap,   // .adm
	AiProfile,      // .aip
	Texture,        // .tga .pcx .dds
	Font,           // .fnt
	Strings,        // RTXT .bin
	MusicScript,    // SCR0 .bin
	RawBin,         // .bin with neither magic (raw tables, exp_info style)
	Credits,        // .kda
	Mission,        // .bms .mis
	Terrain,        // .trn
	TerrainPolyData, // .cpt
	TileInfo,       // .til
	Environment,    // .env
	Menu,           // .mnu
	MenuStyle,      // .mns
	SoundBank,      // .sbf
	WaveBank,       // .lwf
	DialogBank,     // .dbf
	Particles,      // .ptl .ptu .ptg
	Script,         // .wac
	ItemDefs,       // items.def
	WeaponDefs,     // weapon.def
	AmmoDefs,       // ammo.def
	HudPosDefs,     // hudpos.def
	HudFxDefs,      // hudfx.def
	AvatarDefs,     // Avatars.def
	SoundProfileDefs, // SndProf.def
	CharAttrDefs,   // charattr.def
	PowerupDefs,    // powerup.def
	OtherDefs,      // any other .def
	StringTableCoo, // .coo (the NovaWorld UI string table)
	Video,          // .bik
	PlayerSave,     // .sav
	Shader,         // .fx
	Config,         // .cfg .ini .ssc
	Text,           // .txt
};

// The stable lower-case token for a kind ("mission", "item_defs"); "unknown" for Unknown.
const char *asset_kind_token(AssetKind kind);

// The user-facing label ("Mission", "Item definitions").
const char *asset_kind_label(AssetKind kind);

} // namespace opennova::editor
