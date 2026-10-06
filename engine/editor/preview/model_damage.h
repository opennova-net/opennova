#pragma once

// A model's damage states, named and played (DI-10, the deep-integration plan's "damage states named and
// played"): the items that draw the model intact (its graphic) or destroyed (its husk, its final husk),
// what an item's death does in the game, in order from its death tick, and the state the game draws at a
// tick of it: the husk swapped in, the sections that left as pieces, the six destroy-fade registers. The
// model preview's Damage popup and its envelope's `damage` read it; its State and Play destroy set it.
//
// Every rule is the engine's own: the husk the game draws and the model its pieces come from
// (world::husk_render_graphic, world::death_piece_graphic), the item's death class (world::
// item_death_class_from_tag), the debris rows a section rolls (world::death_piece_type_index,
// world::death_piece_type), the fade's phases (world::destroy_fade_phases); what each class's death runs
// is world/item_events.cpp's and world/destruction.cpp's (docs/world/world-wac-ai-re.md §24). The item is
// read from its catalog by the game's parser (def::def_parse_items_memory).
//
// The effects a death spawns are named, not drawn: no editor device draws an effect on this branch (the
// effect preview, DI-14's preview_effects, is not on the trunk). The legs of kind "effect" are the seam it
// fills: each names its effect, where it spawns and its tick.

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <runtime/world/destruction.h>

namespace opennova {
class FileSource;
namespace threedi {
struct Threedi3di3;
}
} // namespace opennova

namespace opennova::editor {

class AssetGraph;
class AssetScan;
struct GraphEdge;

// The part a model plays in an item's damage, by the items.def field naming it.
enum class DamageRole : uint8_t {
	Graphic,      // graphic: the item intact
	EnemyGraphic, // graphic_enemy: the item intact, seen by its enemies
	Husk,         // husk: drawn once the item is destroyed
	FinalHusk,    // huskfinal: the death pieces' model (the husk drawn where no husk is authored)
};
// "graphic", "enemy_graphic", "husk", "final_husk".
const char *damage_role_token(DamageRole role);

// An item naming the model at `field`: the record, its catalog (project-relative), the record's locator
// there, and the edge (what a Go to opens: usage_target; valid while the graph it came from stands).
struct DamageUse {
	std::string item;
	std::string file;
	std::string field;
	std::string locator;
	DamageRole role = DamageRole::Graphic;
	const GraphEdge *edge = nullptr;
};
// Every item naming the model at `model_path` as its graphic, graphic_enemy, husk or huskfinal, in the
// graph's order.
std::vector<DamageUse> model_damage_uses(const AssetGraph &graph, const std::string &model_path);

// What a death reads of an item, as the game's item parser reads it.
struct DamageItem {
	bool found = false;
	std::string name;
	std::string file; // its catalog's file name
	std::string graphic, graphic_enemy, husk, huskfinal;
	int32_t destroy_timing_ticks[3] = {};
	std::string ai_function;
	world::ItemDeathClass death_class = world::ItemDeathClass::kNull;
	bool ai_class = false; // attrib AIData: a brain runs it, and its death
	bool decoration = false; // type decoration (2)
	int unit_type = 0;
	std::string sounddeath, particledeath, particleh2odeath, particlefire, particleother;
	uint8_t piece_types[17] = {}; // husk_sub_part_types by section, slot 16 the clamp's (unauthored: 0)
	int husk_sub_parts = 0;
	float kz = 0.0f;
};
// The record `record` of the catalog `catalog` (a file name the game reads it by) through `files`; false
// when the catalog does not read or holds no such record.
bool read_damage_item(const FileSource &files, const std::string &catalog, const std::string &record,
                      DamageItem &out);

// The state the model preview draws (the options' `damage`): the item intact, or destroyed as the game
// destroys it, the death at the preview clock's tick 0; the item by its record ("" the first naming the
// model).
enum class DamageState : uint8_t { Intact, Destroyed };
struct DamageOptions {
	DamageState state = DamageState::Intact;
	std::string item;
	bool operator==(const DamageOptions &other) const { return state == other.state && item == other.item; }
	bool operator!=(const DamageOptions &other) const { return !(*this == other); }
};
// On the wire: {state ("intact", "destroyed"), item}.
io::JsonValue damage_options_to_json(const DamageOptions &options);
bool read_damage_options(const io::JsonValue &json, DamageOptions &held, std::string &error);

// What the plan reads of the husk models (the project's, as the game loads them by name): the piece
// model's LOD 0 section count (-1 unread), its Dead, Fire and Other points (the death banks' [orig:
// Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522EE0, ItemDef_GetBoneMaskByName @ 0x49EA40: the first
// 16 user points named so]) and the first husk's KZ points [orig: Entity_QueueKzBlastAtUserPoints @
// 0x4EABF0].
struct DamageModels {
	bool husk_read = false;  // the husk the game draws read
	bool piece_read = false; // the piece model read
	int piece_sections = -1;
	int dead_points = 0, fire_points = 0, other_points = 0;
	int kz_points = 0;
};
void note_damage_husk(const threedi::Threedi3di3 &husk, DamageModels &models);
void note_damage_pieces(const threedi::Threedi3di3 &pieces, DamageModels &models);

// One thing a death does: its tick after the death tick, its kind ("swap", "pieces", "flash", "sound",
// "effect", "blast", "fade"), what it names (the husk, a set, an effect, an ammo), its words and its
// citation, and how the preview shows it ("shown", "played", "named": an effect no device draws here, a
// blast nothing in the preview takes).
struct DamageLeg {
	int32_t tick = 0;
	std::string kind;
	std::string name;
	std::string words;
	std::string cite;
	std::string shown;
};
// One section of the piece model leaving as a death piece: its debris row, its chance (1 always).
struct DamagePiece {
	int section = 0;
	std::string type;
	float chance = 1.0f;
};
// An item's death as the game runs it [orig: world-wac-ai-re §24]: its class in words, whether it lands
// the husk (Flags |= 4) and on which tick, the husk drawn and the model the pieces come from (as
// authored), the pieces, the sections that leave for certain (hidden on the husk from the swap), and the
// legs in order.
struct DamagePlan {
	std::string class_words;
	bool swaps = false;
	int32_t swap_tick = 0;
	std::string husk;
	std::string pieces_from;
	std::vector<DamagePiece> pieces;
	uint32_t hidden_sections = 0;
	std::vector<DamageLeg> legs;
};
DamagePlan damage_plan(const DamageItem &item, const DamageModels &models);

// The state the game draws `ticks` after the death tick: husked from the swap; the fade's six phases once
// husked, from the death tick, or from the first husked tick past the item's delay where it authors one
// (update_item_destroy_fade restamps the origin there [orig: Entity_PublishSwapFadePhases @ 0x5C3F40]);
// the sections that left as pieces.
struct DamageFrame {
	bool husked = false;
	int32_t fade_elapsed = -1; // -1: the phases read 0 (intact, or the delay runs)
	world::DestroyFade fade;
	uint32_t hidden_sections = 0;
};
DamageFrame damage_frame(const DamagePlan &plan, const DamageItem &item, int32_t ticks);

// The fade's length in ticks from the death tick to its last phase's end: the delay, then duration + 4 x
// stagger (0 takes 50 and 25).
int32_t damage_fade_end_tick(const DamagePlan &plan, const DamageItem &item);

} // namespace opennova::editor
