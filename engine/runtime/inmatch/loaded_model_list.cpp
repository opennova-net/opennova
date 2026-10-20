#include <runtime/inmatch/loaded_model_list.h>

#include <runtime/terrain_query/terrain_field_store.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <base/io/strutil.h>
#include <formats/avatars/avatars.h>
#include <formats/charattr/charattr.h>
#include <formats/def/def_hudfx.h>
#include <formats/env/env.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace opennova::inmatch {

namespace {

// items.def's id less 100000, the type id the game finds a row by [orig: the id parse
// `atol - 186A0h`; ItemList_FindIndexByTypeId @0x49e100 compares it].
constexpr int kItemIdOffset = 100000;

// ItemList_FindIndexByTypeId: the first row of the type id, 0 when none (row 0 too)
// [orig: @0x49e100 - the walk @0x49e117..0x49e12f, 0 @0x49e12f / @0x49e131].
int find_index_by_type_id(const def::DefItemsFile *items, int type_id) {
	if (items == nullptr) return 0;
	for (size_t i = 0; i < items->count; ++i)
		if (items->entries[i].id - kItemIdOffset == type_id) return static_cast<int>(i);
	return 0;
}

// One item row's model loads, EntityDef_LoadModelsAndCallbacks: nothing once a call loaded
// its graphic model; else the graphic, the husk, the husk with a trailing "xa" (lower
// case) changed to "xb" as the final husk, `huskfinal` while no final husk loaded, the enemy
// graphic, the husk shadow and the virtual display, each only when named.
// [orig: EntityDef_LoadModelsAndCallbacks @0x439f50 - the graphic-model test @0x439fb3,
//  graphic @0x439fdc, husk @0x43a0b6, strlwr + the "xa" test @0x43a145..0x43a191, the "xb"
//  load @0x43a1c8, huskfinal @0x43a245 / @0x43a26e, graphicenemy @0x43a2fc, huskshadow
//  @0x43a38a, virtualdisplay @0x43a412]
class ItemModelLoads {
public:
	ItemModelLoads(LoadedModelList &list, const def::DefItemsFile *items)
			: list_(list), items_(items),
			  graphic_(items != nullptr ? items->count : 0, 0),
			  husk_final_(items != nullptr ? items->count : 0, 0) {}

	void load(int index) {
		if (items_ == nullptr || index < 0 || static_cast<size_t>(index) >= items_->count) return;
		const size_t i = static_cast<size_t>(index);
		if (graphic_[i]) return;
		const def::DefItemDef &d = items_->entries[i];
		if (d.graphic[0] != '\0') graphic_[i] = list_.load(d.graphic);
		if (d.husk[0] != '\0') {
			list_.load(d.husk);
			std::string variant = d.husk;
			for (char &c : variant) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (variant.size() >= 2 && variant[variant.size() - 2] == 'x' && variant.back() == 'a') {
				variant.back() = 'b';
				husk_final_[i] = list_.load(variant);
			}
		}
		if (!husk_final_[i] && d.huskfinal[0] != '\0') husk_final_[i] = list_.load(d.huskfinal);
		if (d.graphic_enemy[0] != '\0') list_.load(d.graphic_enemy);
		if (d.huskshadow[0] != '\0') list_.load(d.huskshadow);
		if (d.virtual_display[0] != '\0') list_.load(d.virtual_display);
	}
	void load_type(int type_id) { load(find_index_by_type_id(items_, type_id)); }

private:
	LoadedModelList &list_;
	const def::DefItemsFile *items_;
	std::vector<char> graphic_;    // the row's graphic model is loaded (+0xF0 set)
	std::vector<char> husk_final_; // the row's final husk is loaded (+0xF8 set)
};

bool read_text(const mission::BootFileSource *files, const std::string &name, std::string &text) {
	std::vector<uint8_t> bytes;
	if (files == nullptr || !files->valid() || !files->read_file(name, bytes)) return false;
	text.assign(bytes.begin(), bytes.end());
	return true;
}

// The camouflage property the climate selects (desert 11, jungle 10, arctic 12)
// [orig: Game_ReloadEntityModelsAndCallbacks @0x522840..0x52285a, @0x522c63..0x522c91].
uint8_t cammo_property(bms::ClimateType climate) {
	return static_cast<uint8_t>(charattr::cammo_property_for_camouflage(static_cast<int>(climate)));
}

} // namespace

std::string LoadedModelList::node_name(const std::string &name) {
	const size_t dot = name.find('.');
	return (dot == std::string::npos ? name : name.substr(0, dot)) + ".3DI";
}

bool LoadedModelList::load(const std::string &name) {
	const std::string file = node_name(name);
	for (const Node &node : created_)
		if (strutil::iequals(node.name, file)) return true;
	if (!loads_ || !loads_(file)) return false;
	created_.push_back(Node{file, false});
	return true;
}

void LoadedModelList::load_foliage(const std::string &name) {
	if (!load(name)) return;
	const std::string file = node_name(name);
	for (Node &node : created_)
		if (strutil::iequals(node.name, file)) node.foliage = true;
}

std::vector<uint32_t> LoadedModelList::freeze() const {
	size_t count = 0;
	for (const Node &node : created_)
		if (!node.foliage) ++count;
	return std::vector<uint32_t>(count, 0u);
}

std::vector<LoadedModelList::Node> LoadedModelList::nodes() const {
	return std::vector<Node>(created_.rbegin(), created_.rend());
}

bool mission_has_special_event(const bms::File &mission) {
	for (const bms::Event &event : mission.events) {
		for (int k = 0; k < event.action_count; ++k) {
			const size_t index = static_cast<size_t>(event.action_index) + static_cast<size_t>(k);
			if (event.action_index < 0 || index >= mission.actions.size()) break;
			const bms::Action &a = mission.actions[index];
			if (static_cast<int>(a.action_type) == 39 &&
					(a.action_sub_type == 1 || a.action_sub_type == 2))
				return true;
		}
	}
	return false;
}

LoadedModelList build_mission_start_model_list(const MissionStartModelSources &src) {
	LoadedModelList list(src.model_loads);
	ItemModelLoads items(list, src.items);

	// (1) The Avatars.def combos, three parts each at the climate's variant (graphic,
	// graphic_j, graphic_s), the part's base graphic where the variant is empty
	// [orig: Game_StartMission @0x524e24 (the joiner's arm; @0x525267 the other) ->
	//  MinimapSlot_LoadModels_0 @0x57a0b0 (this = g_AvatarDefs, lod = g_BmsClimate) - the
	//  variant picks @0x57a0d8..0x57a0ed / @0x57a14d..0x57a163 / @0x57a1c2..0x57a1de, the
	//  base fallbacks @0x57a0f0 / @0x57a166 / @0x57a1e4, the loads @0x57a0fd / @0x57a174 /
	//  @0x57a1f5]
	std::string avatars_text;
	if (read_text(src.files, "Avatars.def", avatars_text)) {
		avatars::AvatarsFile file{};
		if (avatars::avatars_parse_memory(avatars_text.data(), avatars_text.size(), &file) == 0) {
			auto part = [&](const avatars::AvatarPartSnapshot &p) {
				const char *variant = p.graphic;
				if (src.climate == bms::ClimateType::Jungle) variant = p.graphic_j;
				else if (src.climate == bms::ClimateType::Snow) variant = p.graphic_s;
				list.load(variant[0] != '\0' ? variant : p.graphic);
			};
			for (size_t n = 0; n < file.nationalities_count; ++n) {
				const avatars::AvatarNationality &nat = file.nationalities[n];
				for (size_t d = 0; d < nat.divisions_count; ++d) {
					const avatars::AvatarDivision &div = nat.divisions[d];
					for (size_t c = 0; c < div.combos_count; ++c) {
						const avatars::AvatarCombo &combo = div.combos[c];
						part(combo.head);
						part(combo.body);
						part(combo.arms);
					}
				}
			}
		}
		avatars::avatars_free(&file);
	}

	// (2) The world stream landed: no model load [orig: NapiClient_WaitForGameStart @0x524e29].
	// (3) Every ammo row's friendly and foe tracer items [orig: AmmoDef_LoadAll @0x40b0b0
	//  (@0x525495) -> AmmoDef_FreeModels @0x4098c0 - the row's +16 @0x4098e0 and +20
	//  @0x4098ef, each when nonzero, to g_AmmoDefCapacity @0x409906].
	if (src.ammo != nullptr) {
		for (const world::AmmoTableEntry &row : src.ammo->entries) {
			const int friendly = row.tracer_item_friendly != 0
					? find_index_by_type_id(src.items, row.tracer_item_friendly) : 0;
			const int foe = row.tracer_item_enemy != 0
					? find_index_by_type_id(src.items, row.tracer_item_enemy) : 0;
			if (friendly != 0) items.load(friendly);
			if (foe != 0) items.load(foe);
		}
	}
	// (4) weapon.def's gfx1 and gfx3 models as its lines are parsed, every row
	// [orig: WeaponDefs_LoadFile @0x5450a0 (@0x5254bd) -> WeaponDefs_ParseLineCallback
	//  @0x543680 - the gfx1 / gfx3 arms @0x54490c / @0x544922, the loads @0x544fb2
	//  (nocheckdepth) / @0x544fce].
	if (src.weapons != nullptr) {
		for (size_t w = 0; w < src.weapons->count; ++w) {
			const def::DefWeaponDef &row = src.weapons->entries[w];
			if (row.gfx1[0] != '\0') list.load(row.gfx1);
			if (row.gfx3[0] != '\0') list.load(row.gfx3);
		}
	}

	// (5) Game_ReloadEntityModelsAndCallbacks (@0x52550e).
	const uint8_t camo = cammo_property(src.climate);
	if (src.in_session) {
		// The MP classes 5..9's camouflage items [orig: @0x522865..0x5228e5].
		for (uint8_t cls = 5; cls <= 9; ++cls)
			items.load_type(src.charattr != nullptr ? charattr_cammo_type_id(*src.charattr, cls, camo) : 0);
	} else {
		// Team 1's class's [orig: CharAttr_GetIntProperty(g_CharClassTeam1, ..) @0x522901].
		int32_t type_id = 0;
		if (src.charattr != nullptr) (void)charattr_get_int(*src.charattr, src.team1_class, camo, type_id);
		items.load_type(type_id);
	}
	items.load_type(2143); // [orig: @0x52291e / @0x522924]
	// The palm fragment, only for a row of its type [orig: Entity_PreloadPalmFragmentModel
	// @0x53bd90 - @0x53bd9f].
	if (const int palm = find_index_by_type_id(src.items, 900); palm != 0) items.load(palm);
	// The parachute, the night-vision goggles, the binoculars [orig: Entity_PreloadSpecialItems
	// @0x43c220 - @0x43c230, @0x43c245, @0x43c262].
	items.load_type(185);
	items.load_type(1904);
	items.load_type(1424);
	// Pools 1, 2 and 0 in slot order, each entity of a row; a player (Flags 0x100) at its
	// class's camouflage item [orig: pool 1 @0x522967..0x5229b8, pool 2 @0x522ac6..0x522b16,
	// pool 0 @0x522c0c..0x522cad (the player re-resolve @0x522c59..0x522c9d)].
	if (src.world != nullptr) {
		for (const int pool : {1, 2, 0}) {
			src.world->registry.for_each_in_pool(pool, [&](const world::Entity &e) {
				int index = find_index_by_type_id(src.items, e.item_id);
				if (index == 0) return;
				if (pool == 0 && (e.flags & world::kEntityFlagPlayer) != 0)
					index = find_index_by_type_id(src.items,
							src.charattr != nullptr
									? charattr_cammo_type_id(*src.charattr, e.player_class, camo)
									: 0);
				items.load(index);
			});
		}
		// Each pool-2 vehicle bay (its row's attrib2 bit 0): every item group its spawn mask
		// selects whose item is set, that item and each of its four attachment slots that
		// names a userpoint [orig: @0x522dc9..0x522eb0 - the bay test @0x522e0a, the mask
		// test @0x522e3a, the group's item @0x522e5d, the attachments @0x522e76..0x522e84].
		if (src.items != nullptr) {
			src.world->registry.for_each_in_pool(2, [&](const world::Entity &e) {
				const int index = find_index_by_type_id(src.items, e.item_id);
				if (index == 0) return;
				const def::DefItemDef &bay = src.items->entries[index];
				if ((bay.attrib2 & def::DEF_ITEM_ATTRIB2_VEHICLEBAY) == 0) return;
				for (int group = 0; group < src.items->vehicle_spawn_id_count; ++group) {
					const int type_id = src.items->vehicle_spawn_ids[group];
					if ((bay.vehicle_spawn_mask & (uint32_t(1) << group)) == 0 || type_id == 0) continue;
					const int group_index = find_index_by_type_id(src.items, type_id);
					items.load(group_index);
					const def::DefItemDef &item = src.items->entries[group_index];
					for (size_t k = 0; k < item.emplacement_attachments_count && k < 4; ++k) {
						const def::DefItemEmplacementAttachment &a = item.emplacement_attachments[k];
						if (a.userpoint[0] != '\0') items.load_type(a.item_id - kItemIdOffset);
					}
				}
			});
		}
	}
	// The special event's items [orig: EventTrigger_HasActiveSpecialEvent @0x522ebf ->
	// Entity_PreloadSpecialItemModels @0x4516e0 - 4529 @0x4516eb, 4520 @0x4516fb, 1281
	// @0x45170b].
	if (src.special_event) {
		items.load_type(4529);
		items.load_type(4520);
		items.load_type(1281);
	}
	// (6) ItemDef_ResolveAllResources (@0x52557c) and Entity_InitAllFromModels (@0x52567f)
	// load no model.

	// (7) The celestial models the environment names and upl.3di; the teardown cleared the
	// models a mission before loaded [orig: EffectWorld_LoadCelestialModels @0x5adc50
	// (@0x525724) - sun @0x5adc74, moon @0x5adc9c, glare @0x5adcd1, star @0x5add06, upl.3di
	// @0x5add32; the clear Game_TeardownMission @0x52270e -> CEffectWorld_IsNameAvailable
	// @0x5abff0].
	env::MissionEnv environment;
	const env::EnvTextReader read_env = [&](const std::string &name, std::string &text) {
		return read_text(src.files, name, text);
	};
	if (!src.terrain.empty())
		(void)env::read_mission_env(read_env, src.terrain + ".trn",
				src.environment.empty() ? std::string() : src.environment + ".env", environment);
	for (const std::string *name : {&environment.config.sun_3di, &environment.config.moon_3di,
				 &environment.config.glare_3di, &environment.config.star_3di})
		if (!name->empty()) list.load(*name);
	list.load("upl.3di");
	// (8) hudfx.def's nine model slots, each named [orig: HUD_InitOverlaySystem @0x5a4620
	// (@0x525733) - File_ParseASCIIFile("hudfx.def") @0x5a4633, the loads @0x5a465f..0x5a4797;
	// the clear Game_TeardownMission @0x522713 -> HUD_ResetAllCounters @0x58fda0].
	std::string hudfx_text;
	if (read_text(src.files, "hudfx.def", hudfx_text)) {
		const def::HudFxFile hudfx = def::hudfx_parse(
				reinterpret_cast<const uint8_t *>(hudfx_text.data()), hudfx_text.size());
		for (const std::string &name : def::hudfx_slot_names(hudfx))
			if (!name.empty()) list.load(name);
	}
	// (9) The terrain's foliage models, marked [orig: Game_StartMission @0x5259fd ->
	// CRenderManager_ShutdownAllSubsystems @0x587000 -> sub_604DA0 (@0x604dbc) ->
	// Foliage_LoadDefAssets @0x601260].
	const mission::BootFileSource *files = src.files;
	const std::function<bool(const std::string &, std::vector<uint8_t> &)> read_file =
			[files](const std::string &name, std::vector<uint8_t> &bytes) {
				return files != nullptr && files->valid() && files->read_file(name, bytes);
			};
	for (const std::string &graphic :
			terrain::terrain_foliage_graphics(read_file, src.terrain, src.environment))
		if (!graphic.empty()) list.load_foliage(graphic);
	// The freeze follows [orig: Render_InitMissionTextures @0x525a6e].
	return list;
}

} // namespace opennova::inmatch
