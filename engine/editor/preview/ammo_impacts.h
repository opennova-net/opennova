#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/file_source.h>
#include <editor/preview/viewport_follow.h>
#include <runtime/world/ammo_table.h>

namespace opennova::editor {

// An ammo's impact rows as a board (ADR 0046 DI-23; CONTEXT.md "Impact board"): for each surface class the game
// reads where a round stops (the terrain's char map, a placed tile's square, an object's bullet face), the
// effects-table row a round of the ammo plays there, as the game picks it, and the mark it leaves. Nothing here
// decides a row: the class is the tag's place less four (a terrain stop plays its class + 4 [orig:
// Terrain_GetSurfaceTypeAtPosition @ 0x606510 result + 4], an object's face its material byte + 4 [orig:
// Projectile_HandleEntityImpact @ 0x4E9390, `ray[22] + 4` @ 0x4e982b]), the row the impact presenter's pick
// (world::impact_effect_row: the ammo's own row of the tag, else ammo def 0's static bank at the tag's place
// [orig: AmmoDef_ProcessImpactEffect @ 0x40a1b8..0x40a1fd]), the scar the ring writer's choice (world/impact_scar.h:
// the ammo's scar_type, scorch1..4 or the glass hole by the face's material [orig: Impact_SpawnGlassEffectsOrScar
// @ 0x5CF1B0; Scar_TextureForId @ 0x5CC360; Scar_RadiusForId @ 0x5CC3B0]).

// One row of the board: a surface class and the row a round plays where it strikes it.
struct AmmoImpactRow {
	int surface = -1; // the class (0 to 19); -1 for a row no surface reaches (move, player, zip, bodyarmor)
	int tag = 0;      // its effects-table tag (the class + 4)
	world::ImpactRowPick pick;
	// The ring scar a round leaves on an object's face of the class (none on the terrain or the water, which the
	// writer never marks): the scar id (0 none), the textures it draws one of, its radius (metres).
	int scar = 0;
	std::vector<std::string> scar_textures;
	float scar_radius = 0.0f;
	std::string scar_words;
	std::string where; // where the game plays the row, in words
};

// The board of one ammo of a table: its record, ammo def 0 (the bank's owner) and how many rows it authors, the
// ammo's scar_type and terrain scorch, the twenty classes' rows and the rows no surface reaches.
struct AmmoImpactBoard {
	bool found = false;
	std::string ammo;
	int index = -1;
	std::string null_ammo;
	int null_rows = 0; // ammo def 0's authored rows (its bank holds them, in tag order, from place 1)
	int scar_type = 0;
	int scorch_id = 0;
	std::vector<AmmoImpactRow> surfaces;
	std::vector<AmmoImpactRow> others;
};

// The board of `ammo` (by name, as the game's lookup finds it [orig: AmmoDef_LookupByName @ 0x409870]) in `table`.
AmmoImpactBoard ammo_impact_board(const world::AmmoTable &table, const std::string &ammo);
// Where a row came from, in words: the ammo's own row, or the bank's (the fallback rule, cited), or nothing.
std::string ammo_impact_from_words(const AmmoImpactBoard &board, const AmmoImpactRow &row);
// Its wire form (a definition viewport's `body.impacts`): {ammo, index, null_ammo, null_rows, scar_type, scorch_id,
// rule, surfaces: [{surface, name, words, tag, row, from (own, bank), bank_tag, bank_row, effect, sound, played,
// scar {id, textures, radius, words}, where, from_words}], others: [...]}.
io::JsonValue ammo_impact_board_to_json(const AmmoImpactBoard &board);

// ammo.def read from the project's files as the game's mission load reads it (the open document standing in):
// the table built as the load builds it (world::build_ammo_table), read again only when its stamp moves.
class AmmoTableSource {
public:
	// True when the table was read again.
	bool refresh(const std::shared_ptr<const FileSource> &files);
	const world::AmmoTable &table() const { return table_; }
	bool read() const { return read_; }
	const std::string &why() const { return why_; }
	const FileStamps &reads() const { return reads_; }

private:
	std::shared_ptr<const FileSource> files_;
	FileStamps reads_;
	world::AmmoTable table_;
	bool read_ = false;
	std::string why_;
};

} // namespace opennova::editor
