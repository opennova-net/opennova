#include <editor/preview/model_damage.h>

#include <algorithm>
#include <cstdio>

#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <base/vfs/file_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_edge.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/entity.h>
#include <runtime/world/present_passes.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

std::string file_of(const std::string &path) {
	return path.substr(path.find_last_of("/\\") + 1);
}

// The count of a model's user points named `name` (any case), over all of them or the first 16 the
// death banks scan.
int points_named(const threedi::Threedi3di3 &model, const char *name, bool first16) {
	if (first16) {
		const uint16_t mask = threedi::threedi_3di3_user_point_mask(&model, name);
		int count = 0;
		for (uint16_t m = mask; m; m &= uint16_t(m - 1)) ++count;
		return count;
	}
	int count = 0;
	for (size_t i = 0; model.user_points && i < model.user_point_count; ++i)
		if (strutil::iequals(model.user_points[i].name, name)) ++count;
	return count;
}

// Ticks in seconds, as the game's clock passes them (62.5 a second).
std::string seconds(int32_t ticks) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f s", ticks / io::kTickHz);
	return text;
}

std::string plural(int count, const char *one, const char *many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

DamageLeg leg(int32_t tick, const char *kind, const std::string &name, std::string words, const char *cite,
              const char *shown) {
	DamageLeg out;
	out.tick = tick;
	out.kind = kind;
	out.name = name;
	out.words = std::move(words);
	out.cite = cite;
	out.shown = shown;
	return out;
}

// What an effect leg says of the preview: the seam DI-14's effect preview fills.
constexpr const char *kEffectNamed = "named";

// The husk swap leg: the husk drawn in the intact model's place from here [orig: Flags |= 4, the bit the
// renderer reads: Render_SectorEntity @ 0x5C4190 draws entity+0x34 for a husked entity], or the graphic
// kept where neither husk nor final husk is authored (the witnessed fallback).
DamageLeg swap_leg(int32_t tick, const DamagePlan &plan, const char *cite) {
	if (plan.husk.empty())
		return leg(tick, "swap", std::string(), "The item is dead (its husk flag set), but it authors no husk model: "
		           "the preview keeps drawing its graphic.", cite, "shown");
	return leg(tick, "swap", plan.husk,
	           "The husk swap: " + plan.husk + " is drawn in the item's place from here on.", cite, "shown");
}

// The pieces and the death flash [orig: Entity_SpawnDeathPieces @ 0x493400]: not for an item fully under
// water (the preview's is dry), and only with a husk model loaded.
void pieces_legs(int32_t tick, const DamageItem &item, const DamageModels &models, DamagePlan &plan) {
	if (plan.pieces_from.empty()) return;
	if (!models.piece_read) {
		plan.legs.push_back(leg(tick, "pieces", plan.pieces_from,
		                        "The death pieces fly from " + plan.pieces_from + ", which the project lacks or "
		                        "does not read (the game then rolls the def's husk_sub_parts sections): the preview "
		                        "leaves the husk whole.",
		                        "[orig: Entity_SpawnDeathPieces @ 0x493400, the bound @ 0x49361A]", "named"));
		return;
	}
	if (!item.decoration)
		plan.legs.push_back(leg(tick, "flash", std::string(),
		                        "The death flash: a glow light at the item, twice the piece model's radius (not "
		                        "for a decoration).",
		                        "[orig: Entity_SpawnDeathPieces @ 0x49351A]", "named"));
	// The loop bound is the piece model's LOD 0 section count, else the def's husk_sub_parts.
	const int sections = models.piece_sections > 0 ? models.piece_sections : item.husk_sub_parts;
	std::string list;
	for (int s = 1; s < sections; ++s) {
		const world::DeathPieceType &type = world::death_piece_type(world::death_piece_type_index(item.piece_types, s));
		DamagePiece piece;
		piece.section = s;
		piece.type = type.name;
		piece.chance = type.probability;
		if (type.probability >= 1.0f) plan.hidden_sections |= 1u << (s & 31);
		plan.pieces.push_back(piece);
		list += (list.empty() ? "" : ", ") + std::to_string(s) + " " + type.name +
		        (type.probability >= 1.0f
		                 ? std::string()
		                 : " (a " + std::to_string(int(type.probability * 100.0f + 0.5f)) + "% chance: kept here)");
	}
	if (plan.pieces.empty()) {
		plan.legs.push_back(leg(tick, "pieces", plan.pieces_from,
		                        plan.pieces_from + " has no section past its hull: no piece flies.",
		                        "[orig: Entity_SpawnDeathPieces @ 0x493400]", "shown"));
		return;
	}
	plan.legs.push_back(leg(tick, "pieces", plan.pieces_from,
	                        "The death pieces: sections of " + plan.pieces_from + " fly off, each its debris row's " +
	                        "trail behind it, and leave the husk: " + list + ".",
	                        "[orig: Entity_SpawnDeathPieces @ 0x493400; g_DeathPieceTypes @ 0x8404F0]", "shown"));
}

// A set played at the item [the class's death sound].
void sound_leg(int32_t tick, const std::string &set, const char *cite, DamagePlan &plan) {
	if (set.empty()) return;
	plan.legs.push_back(leg(tick, "sound", set, "The death sound " + set + ", played at the item.", cite, "played"));
}

// Entity_InitDeathSounds's tail: the death sound, the three bone banks (the water death's pair under
// water), the KZ blasts [orig: Entity_InitDeathSounds @ 0x4939B0].
void death_sounds_legs(int32_t tick, const DamageItem &item, const DamageModels &models, DamagePlan &plan) {
	const char *cite = "[orig: Entity_InitDeathSounds @ 0x4939B0]";
	sound_leg(tick, item.sounddeath, cite, plan);
	const bool banks = models.husk_read || models.piece_read; // the banks need a loaded husk model
	struct Bank {
		const std::string &effect;
		int points;
		const char *bone;
	};
	const Bank rows[] = {{item.particledeath, models.dead_points, "Dead"},
	                     {item.particlefire, models.fire_points, "Fire"},
	                     {item.particleother, models.other_points, "Other"}};
	for (const Bank &bank : rows) {
		if (bank.effect.empty() || !banks) continue;
		const bool dead = std::string(bank.bone) == "Dead";
		if (bank.points == 0 && !dead) continue; // a Fire or Other bank with no point spawns nothing
		std::string where = bank.points ? "at the piece model's " + plural(bank.points, bank.bone, bank.bone) + " point" +
		                                          (bank.points == 1 ? "" : "s")
		                                : std::string("once at the item (the husk names no Dead point)");
		std::string words = std::string("The ") + bank.bone + " bank: " + bank.effect + " " + where;
		if (dead && !item.particleh2odeath.empty())
			words += " (" + item.particleh2odeath + " in its place for an item under water)";
		plan.legs.push_back(leg(tick, "effect", bank.effect, words + ".", cite, kEffectNamed));
		plan.legs.back().bank = int(&bank - rows) + 1;
	}
	const bool husk_kz = !item.husk.empty() && models.kz_points > 0;
	plan.legs.push_back(leg(tick, "blast", "kz_OrganicBlast",
	                        husk_kz ? "The death blast: kz_OrganicBlast, radius 5, at each of the husk's " +
	                                          plural(models.kz_points, "KZ point", "KZ points") + "."
	                                : std::string("The death blast: one kz_OrganicBlast at the item, its radius the "
	                                              "item's kz else its bound radius (the husk names no KZ point)."),
	                        "[orig: Entity_QueueKzBlastAtUserPoints @ 0x4EABF0]", "named"));
}

} // namespace

const char *damage_role_token(DamageRole role) {
	switch (role) {
	case DamageRole::Graphic: return "graphic";
	case DamageRole::EnemyGraphic: return "enemy_graphic";
	case DamageRole::Husk: return "husk";
	case DamageRole::FinalHusk: return "final_husk";
	}
	return "graphic";
}

std::vector<DamageUse> model_damage_uses(const AssetGraph &graph, const std::string &model_path) {
	std::vector<DamageUse> out;
	for (const GraphEdge *edge : graph.referrers_of_file(model_path)) {
		if (edge->kind != ReferenceKind::Model) continue;
		DamageUse use;
		if (edge->field == "graphic") use.role = DamageRole::Graphic;
		else if (edge->field == "graphic_enemy") use.role = DamageRole::EnemyGraphic;
		else if (edge->field == "husk") use.role = DamageRole::Husk;
		else if (edge->field == "huskfinal") use.role = DamageRole::FinalHusk;
		else continue;
		use.item = edge->record.substr(0, edge->record.find('/'));
		use.file = edge->source;
		use.field = edge->field;
		use.locator = edge->locator;
		use.edge = edge;
		out.push_back(std::move(use));
	}
	return out;
}

bool read_damage_item(const FileSource &files, const std::string &catalog, const std::string &record,
                      DamageItem &out) {
	out = DamageItem();
	std::vector<uint8_t> bytes;
	if (catalog.empty() || !files.read(catalog, bytes) || bytes.empty()) return false;
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) != 0) return false;
	// A record name resolves to its first row, as a type id does [orig: ItemList_FindIndexByTypeId].
	for (size_t i = 0; i < items.count; ++i) {
		const def::DefItemDef &def = items.entries[i];
		if (!strutil::iequals(def.display_name, record)) continue;
		out = damage_item_of(def, catalog);
		break;
	}
	def::def_free_items(&items);
	return out.found;
}

DamageItem damage_item_of(const def::DefItemDef &def, const std::string &catalog) {
	DamageItem out;
	out.found = true;
	out.name = def.display_name;
	out.file = catalog;
	out.graphic = def.graphic;
	out.graphic_enemy = strutil::fixed_string(def.graphic_enemy, sizeof(def.graphic_enemy));
	out.husk = def.husk;
	out.huskfinal = def.huskfinal;
	std::copy(std::begin(def.destroy_timing_ticks), std::end(def.destroy_timing_ticks),
	          std::begin(out.destroy_timing_ticks));
	out.ai_function = strutil::fixed_string(def.ai_function, sizeof(def.ai_function));
	out.death_class = world::item_death_class_from_tag(out.ai_function.c_str());
	out.ai_class = (def.attrib & world::kItemAttribAIData) != 0;
	out.decoration = def.type == def::DEF_ITEM_TYPE_DECORATION;
	out.unit_type = def.unit_type;
	out.sounddeath = def.sounddeath;
	out.particledeath = def.particledeath;
	out.particleh2odeath = def.particleh2odeath;
	out.particlefire = def.particlefire;
	out.particleother = def.particleother;
	std::copy(std::begin(def.husk_sub_part_types), std::end(def.husk_sub_part_types), out.piece_types);
	out.husk_sub_parts = def.husk_sub_parts;
	out.kz = def.kz;
	return out;
}

io::JsonValue damage_options_to_json(const DamageOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("state", io::json_string(options.state == DamageState::Destroyed ? "destroyed" : "intact"));
	out.set("item", io::json_string(options.item));
	return out;
}

bool read_damage_options(const io::JsonValue &json, DamageOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options.damage is an object {state, item}.";
		return false;
	}
	DamageOptions options = held;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "state") {
			if (!member.value.is_string() || (member.value.string != "intact" && member.value.string != "destroyed")) {
				error = "options.damage.state is \"intact\" or \"destroyed\".";
				return false;
			}
			options.state = member.value.string == "destroyed" ? DamageState::Destroyed : DamageState::Intact;
		} else if (member.key == "item") {
			if (!member.value.is_string()) {
				error = "options.damage.item is the record of an item naming the model (\"\" the first).";
				return false;
			}
			options.item = member.value.string;
		} else {
			error = "Unknown damage option \"" + member.key + "\" (it takes state, item).";
			return false;
		}
	}
	held = options;
	return true;
}

void note_damage_husk(const threedi::Threedi3di3 &husk, DamageModels &models) {
	models.husk_read = true;
	models.kz_points = points_named(husk, "KZ", false);
}

void note_damage_pieces(const threedi::Threedi3di3 &pieces, DamageModels &models) {
	models.piece_read = true;
	models.piece_sections = pieces.lod_count > 0 && pieces.lods ? int(pieces.lods[0].render_object_count) : 0;
	models.dead_points = points_named(pieces, "Dead", true);
	models.fire_points = points_named(pieces, "Fire", true);
	models.other_points = points_named(pieces, "Other", true);
}

DamagePlan damage_plan(const DamageItem &item, const DamageModels &models) {
	DamagePlan plan;
	if (!item.found) return plan;
	plan.husk = world::husk_render_graphic(item.husk, item.huskfinal);
	plan.pieces_from = world::death_piece_graphic(item.husk, item.huskfinal);
	using world::ItemDeathClass;
	const std::string tag = item.ai_function.empty() ? std::string("null") : item.ai_function;
	// A brain row (attrib AIData) dies through its brain's death state, which runs the death transforms
	// once [orig: AI_TransitionToDeath_GroundVehicle @ 0x467B20; AI_TransitionToDestroyed_Vehicle @ 0x467DE0].
	if (item.ai_class || item.death_class == ItemDeathClass::kGnrc) {
		// gnrc: the first kill marks it dead and arms a four-tick think; its expiry runs the death transforms
		// [orig: sub_407020 @ 0x4070B2..0x4070E2, the expiry @ 0x407072..0x4070A6].
		const bool gnrc = !item.ai_class;
		plan.swaps = true;
		plan.swap_tick = gnrc ? 4 : 0;
		plan.class_words = gnrc ? "gnrc (" + item.ai_function +
		                                  "): dead at once, the husk, its pieces and its death sounds four ticks on, "
		                                  "when its think runs out (a cohort's pool can hold it longer)."
		                        : "A brain's item (attrib AIData, ai_function " + item.ai_function +
		                                  "): its brain's death state lands the husk, its pieces and its death "
		                                  "sounds at once.";
		const char *cite = gnrc ? "[orig: sub_407020 @ 0x407020 -> Entity_UpdateDeathTransforms @ 0x494660]"
		                        : "[orig: AI_TransitionToDeath_GroundVehicle @ 0x467B20 -> Entity_UpdateDeathTransforms "
		                          "@ 0x494660]";
		// The unitType row's callback, then Flags |= 6 [orig: Entity_DispatchDeathCallback @ 0x493EF0].
		plan.legs.push_back(swap_leg(plan.swap_tick, plan, cite));
		const bool boat = item.unit_type >= 5 && item.unit_type <= 8;
		if (!boat || models.husk_read || models.piece_read) pieces_legs(plan.swap_tick, item, models, plan);
		if (boat && (models.husk_read || models.piece_read))
			plan.legs.push_back(leg(plan.swap_tick, "sound", "EXPLO_SHIP_TINY", "The boat's explosion, EXPLO_SHIP_TINY.",
			                        "[orig: Entity_ProcessBuildingDeath @ 0x494420]", "played"));
		if (item.unit_type == 11)
			plan.legs.push_back(leg(plan.swap_tick, "effect", "Effect_ShockWaterBrdg",
			                        "The bridge's water shock: Effect_ShockWaterBrdg at each of the husk's DEAD points, "
			                        "on the water plane.",
			                        "[orig: Entity_SpawnDeathEffectsAtBones @ 0x4944C0]", kEffectNamed));
		death_sounds_legs(plan.swap_tick, item, models, plan);
	} else if (item.death_class == ItemDeathClass::kGnrl || item.death_class == ItemDeathClass::kEwep ||
	           item.death_class == ItemDeathClass::kGnl2) {
		// The class's own tail: the def's death sound and ONE particledeath effect at the item, not the banks.
		const bool gnl2 = item.death_class == ItemDeathClass::kGnl2;
		const char *cite = item.death_class == ItemDeathClass::kGnrl ? "[orig: sub_407F80 @ 0x4080BD..0x4080F4]"
		                   : item.death_class == ItemDeathClass::kEwep
		                           ? "[orig: Entity_UpdateChildAttachment @ 0x440C23..0x440C87]"
		                           : "[orig: Entity_HandleDeathEvent @ 0x407279..0x4072DD]";
		plan.swaps = true;
		plan.swap_tick = gnl2 ? 32 : 0;
		plan.class_words =
				gnl2 ? "gnl2: dead at once with its death sound and effect; 32 ticks on it explodes and the husk lands."
				     : tag + ": the husk lands at once, with the item's death sound and one death effect.";
		if (!gnl2) plan.legs.push_back(swap_leg(0, plan, cite));
		sound_leg(0, item.sounddeath, cite, plan);
		if (!item.particledeath.empty()) {
			plan.legs.push_back(leg(0, "effect", item.particledeath,
			                        "The death effect " + item.particledeath + ", once at the item.", cite, kEffectNamed));
			plan.legs.back().at_item = true;
		}
		if (gnl2) {
			const char *expiry = "[orig: Entity_HandleDeathEvent @ 0x4071EF..0x40725F; Entity_SpawnExplosionEffects @ "
			                     "0x4399C0]";
			plan.legs.push_back(leg(32, "effect", "Effect_AirExp", "The explosion: Effect_AirExp a unit above the item.",
			                        expiry, kEffectNamed));
			plan.legs.back().at_item = true;
			plan.legs.back().above = 1.0f;
			plan.legs.push_back(leg(32, "blast", "kz_M406HE", "The explosion's blast: kz_M406HE a unit above the item.",
			                        expiry, "named"));
			plan.legs.push_back(swap_leg(32, plan, expiry));
		}
	} else if (item.death_class == ItemDeathClass::kBuilding || item.death_class == ItemDeathClass::kCollapsingBuilding ||
	           item.death_class == ItemDeathClass::kTree) {
		plan.swaps = true;
		const char *cite = item.death_class == ItemDeathClass::kBuilding ? "[orig: the bldg callback @ 0x43EE60]"
		                   : item.death_class == ItemDeathClass::kCollapsingBuilding
		                           ? "[orig: Entity_ProcessBld2Destruction @ 0x43EEE0]"
		                           : "[orig: Entity_ProcessDestructibleDeath @ 0x43FBC0]";
		plan.class_words = item.death_class == ItemDeathClass::kBuilding
		                           ? "bldg: dead and husked at once."
		                   : item.death_class == ItemDeathClass::kCollapsingBuilding
		                           ? "bld2: dead and husked at once, its KZ blasts queued; then its collapse steps "
		                             "(its particledeath walks the footprint at the first)."
		                           : "tree: its sections burst into debris, then dead and husked at once.";
		plan.legs.push_back(swap_leg(0, plan, cite));
	} else if (item.death_class == ItemDeathClass::kNull) {
		plan.class_words = "null (" + tag + "): its callback only re-arms its think; the item never dies, so the game "
		                   "never draws its husk.";
	} else if (item.death_class == ItemDeathClass::kBarrel) {
		plan.class_words = "brrl: dead without the husk; it explodes and is removed.";
	} else {
		plan.class_words = tag + ": the class runs its own death, which lands no husk this preview plays.";
	}
	// The fade: published from the death tick once the item is husked.
	if (plan.swaps && !plan.husk.empty()) {
		const int32_t delay = item.destroy_timing_ticks[0];
		const int32_t duration = item.destroy_timing_ticks[1] ? item.destroy_timing_ticks[1] : 50;
		const int32_t step = item.destroy_timing_ticks[2] ? item.destroy_timing_ticks[2] : 25;
		const int32_t start = delay ? std::max(delay, plan.swap_tick) : plan.swap_tick;
		plan.legs.push_back(leg(start, "fade", plan.husk,
		                        "The destroy fade on " + plan.husk + ": OBJECT_DESTROY over " +
		                                seconds(duration + 4 * step) + ", OBJECT_DESTROY01..05 each over " +
		                                seconds(duration) + ", " + seconds(step) + " apart" +
		                                (delay ? ", after a delay of " + seconds(delay) : std::string()) +
		                                " (the item's destroy_timing; 0 takes 50 and 25 ticks).",
		                        "[orig: Entity_PublishSwapFadePhases @ 0x5C3F40]", "shown"));
	}
	std::stable_sort(plan.legs.begin(), plan.legs.end(),
	                 [](const DamageLeg &a, const DamageLeg &b) { return a.tick < b.tick; });
	return plan;
}

DamageFrame damage_frame(const DamagePlan &plan, const DamageItem &item, int32_t ticks) {
	DamageFrame frame;
	if (!plan.swaps || ticks < plan.swap_tick) return frame;
	frame.husked = true;
	frame.hidden_sections = plan.hidden_sections;
	// Zeroed every evaluation, then the delay waited out from the death tick and the origin restamped at the
	// first husked evaluation past it [orig: Entity_PublishSwapFadePhases @ 0x5C3F40; update_item_destroy_fade].
	const int32_t delay = item.destroy_timing_ticks[0];
	if (delay && ticks < delay) return frame;
	const int32_t origin = delay ? std::max(delay, plan.swap_tick) : 0;
	frame.fade_elapsed = ticks - origin;
	frame.fade = world::destroy_fade_phases(frame.fade_elapsed, item.destroy_timing_ticks);
	return frame;
}

int32_t damage_fade_end_tick(const DamagePlan &plan, const DamageItem &item) {
	const int32_t delay = item.destroy_timing_ticks[0];
	const int32_t duration = item.destroy_timing_ticks[1] ? item.destroy_timing_ticks[1] : 50;
	const int32_t step = item.destroy_timing_ticks[2] ? item.destroy_timing_ticks[2] : 25;
	const int32_t origin = delay ? std::max(delay, plan.swap_tick) : 0;
	return std::max(origin + duration + 4 * step, plan.swap_tick);
}

} // namespace opennova::editor
