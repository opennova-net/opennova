#pragma once

#include <cstddef>
#include <string>

namespace opennova::editor {

// What a project file IS to the engine (ADR 0046 d6/d7). The runtime catalog's kind
// vocabulary (`resource_kind_for_name_and_magic`, engine/base/resource_index) is the
// shared source for every kind the runtime browses; this enum adds the kinds the
// editor must also know (the name-keyed .def family, scripts, textures, banks,
// videos, plain text) so a requirement row and a scanned file compare by one type.
// What each kind is (its token, words, names, archive, document type) is its row in
// assets/asset_kinds: a new kind is one value here, before kCount, and one row there.
enum class AssetKind {
	Unknown = 0,
	Archive,        // .pff (never a project asset: a build output)
	Model,          // .3di
	Animation,      // .bad
	AnimationMap,   // .adm
	FaceAnimation,  // .grm (a character's facial texture meshes and gesture offsets)
	AiProfile,      // .aip
	Texture,        // .tga .pcx .dds .mdt .png
	MaterialChunk,  // a model's material chunk container, by its content under a name no rule types
	Font,           // .fnt
	Strings,        // RTXT .bin
	MusicScript,    // SCR0 .bin
	RawBin,         // .bin with neither magic (raw tables, exp_info style)
	CountryCode,    // CC.BIN (the country code the boot reads loose)
	Credits,        // .kda
	Mission,        // .bms
	MissionText,    // .mis (the original mission editor's interchange text, which the game never reads)
	MapProject,     // .npj .npz (the mission editor's project, the mission list's other scan)
	Terrain,        // .trn
	TerrainPolyData, // .cpt
	TileInfo,       // .til
	Environment,    // .env
	Menu,           // .mnu
	MenuStyle,      // .mns
	MusicBank,      // .sbf (the music a music script plays, streamed by path)
	SoundBank,      // .lwf (the sound sets, each naming its waves)
	Wave,           // .wav (a sound bank's single, loaded from the archives by name)
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
	NovaWorldScreen, // .mnx (a NovaWorld screen's markup; retail ships it loose, a build packs it)
	Video,          // .bik
	PlayerSave,     // .sav
	Shader,         // .fx
	Config,         // .cfg .ini .ssc .cd
	Score,          // score.ini (the scoring table per game type)
	Text,           // a .txt the game reads by its name (earlyerr.txt, an expansion's version.txt, ...)
	Notes,          // the project's notes, which the game never reads: a .md, any other .txt, a text with no extension
	// A file the project imports, whatever its name: an importer's source with its import record
	// beside it (a .png the image importer turns into a texture), which the scan gives this kind;
	// never packed itself, its outputs are.
	ImportSource,
	// A file an import reads besides its source (S20: a terrain set's images), whatever its name, while
	// a record lists it among its inputs; never packed, its import's outputs are.
	ImportInput,
	kCount,         // the number of kinds, not a kind
};

inline constexpr size_t kAssetKindCount = static_cast<size_t>(AssetKind::kCount);

// The stable lower-case token for a kind ("mission", "item_defs"); "unknown" for Unknown
// (its row's, assets/asset_kinds).
const char *asset_kind_token(AssetKind kind);

// The user-facing label ("Mission", "Item definitions").
const char *asset_kind_label(AssetKind kind);

// The kind a token names; Unknown when no kind carries it.
AssetKind asset_kind_from_token(const std::string &token);

} // namespace opennova::editor
