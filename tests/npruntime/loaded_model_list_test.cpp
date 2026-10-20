// The mission start's model-def list and the C2S 0x3D page it freezes (D-NET-527).
// Synthetic mount and tables only; the expected list is derived from the cited original's
// rules and order: one node per name up to its first '.' with ".3DI", found again without
// case, a file that does not load leaving none, each new node at the head; foliage marks
// sticky; the loads in Game_StartMission's order (the Avatars.def combos at the climate's
// variant, the ammo rows' tracer items, weapon.def's gfx1 / gfx3, the reloaded items, the
// celestial models and upl.3di, hudfx.def, the terrain's foliage).
// [orig: ThreediGp_LoadModel @0x5B6160; CEffectDef_Construct @0x5b01e0; CEffectWorld_
//  RebuildAllModelBuffers @0x5B3A80; MinimapSlot_LoadModels_0 @0x57a0b0; AmmoDef_FreeModels
//  @0x4098c0; WeaponDefs_ParseLineCallback @0x543680; Game_ReloadEntityModelsAndCallbacks
//  @0x522830; EntityDef_LoadModelsAndCallbacks @0x439f50; EffectWorld_LoadCelestialModels
//  @0x5adc50; HUD_InitOverlaySystem @0x5a4620; Foliage_LoadDefAssets @0x601260;
//  NetPacket_WriteEntityIndexList @0x42d950]
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/loaded_model_list.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <base/io/strutil.h>
#include <formats/def/def.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	return s;
}

// The mount: text files by name, without case.
struct Mount {
	std::map<std::string, std::string> files;
	mission::BootFileSource source() const {
		mission::BootFileSource out;
		out.has_file = [this](const std::string &name) { return files.count(lower(name)) != 0; };
		out.read_file = [this](const std::string &name, std::vector<uint8_t> &bytes) {
			const auto it = files.find(lower(name));
			if (it == files.end()) return false;
			bytes.assign(it->second.begin(), it->second.end());
			return true;
		};
		return out;
	}
};

def::DefItemDef item(int type_id, const char *graphic) {
	def::DefItemDef d;
	def::def_init_item(d);
	d.id = type_id + 100000;
	std::strncpy(d.graphic, graphic, sizeof(d.graphic) - 1);
	return d;
}

void test_the_mission_start_list() {
	Mount mount;
	mount.files["avatars.def"] =
			"define head HEAD_A\r\n{\r\n\tgraphic hdA.3di\r\n\tgraphic_j hdA_j.3di\r\n}\r\n"
			"define head HEAD_B\r\n{\r\n\tgraphic hdB.3di\r\n}\r\n"
			"define body BODY_A\r\n{\r\n\tgraphic bdA.3di\r\n}\r\n"
			"define arms ARMS_A\r\n{\r\n\tgraphic amA.3di\r\n}\r\n"
			"nationality N00 AV_NAT\r\n{\r\n\talignment good\r\n\tdivision D00 AV_DIV\r\n\t{\r\n"
			"\t\tcombo 001 HEAD_A BODY_A ARMS_A\r\n"
			"\t\tcombo 002 HEAD_B BODY_A\r\n"
			"\t}\r\n}\r\n";
	mount.files["hudfx.def"] = "3DIHud hud.3di\r\n";
	mount.files["synth.trn"] =
			"polytrn_colormap synth_c.tga\r\npolytrn_detailmap synth_d.tga\r\npolytrn_polydata synth.pdt\r\n"
			"foliage\r\n  graphic pine.3di\r\nend\r\n"
			"foliage\r\n  graphic bush.3di\r\nend\r\n";
	mount.files["synth.env"] = "sun_3di sun1.3di\r\n";
	const mission::BootFileSource files = mount.source();

	// The model files that load (the rest do not exist or do not parse).
	const std::set<std::string> loadable = {
			"hda_j.3di", "bda.3di", "hdb.3di", "trc.3di", "m4.3di", "m4_3p.3di", "glock.3di",
			"pine.3di", "sol_j.3di", "solxa.3di", "solxb.3di", "solfin.3di", "item2143.3di",
			"wreck.3di", "wreckfin.3di", "chute.3di", "crate.3di", "crate_e.3di", "bay.3di",
			"ai.3di", "jeep.3di", "jeep_vd.3di", "mg.3di", "sun1.3di", "fmoon4.3di", "upl.3di",
			"hud.3di", "bush.3di", "crate_s.3di"};
	std::vector<std::string> asked;
	const inmatch::LoadedModelList::LoadsFn loads = [&](const std::string &file) {
		asked.push_back(file);
		return loadable.count(lower(file)) != 0;
	};

	// items.def: row 0 the Null marker; the camouflage item with a husk ending "xa"; 2143
	// with a husk and a final husk; the parachute; pool items (the pool-1 crate with an
	// enemy graphic and a husk shadow); a vehicle bay whose group's item carries one named
	// attachment slot of two.
	std::vector<def::DefItemDef> rows;
	rows.push_back(item(0, ""));
	def::DefItemDef camo = item(5101, "sol_j.3di");
	std::strcpy(camo.husk, "SOLXA");
	std::strcpy(camo.huskfinal, "solfin.3di"); // not loaded: the "xb" final husk loaded
	rows.push_back(camo);
	def::DefItemDef wreck = item(2143, "item2143.3di");
	std::strcpy(wreck.husk, "wreck.3di");
	std::strcpy(wreck.huskfinal, "wreckfin.3di");
	rows.push_back(wreck);
	rows.push_back(item(185, "chute.3di"));
	rows.push_back(item(7001, "trc.3di"));
	def::DefItemDef crate = item(6001, "crate.3di");
	std::strcpy(crate.graphic_enemy, "crate_e.3di");
	std::strcpy(crate.huskshadow, "crate_s.3di"); // after the enemy graphic
	rows.push_back(crate);
	def::DefItemDef bay = item(6002, "bay.3di");
	bay.attrib2 = def::DEF_ITEM_ATTRIB2_VEHICLEBAY;
	bay.vehicle_spawn_mask = 1u;
	rows.push_back(bay);
	def::DefItemEmplacementAttachment attachments[2] = {};
	std::strcpy(attachments[0].userpoint, "gun1");
	attachments[0].item_id = 106004;
	attachments[1].item_id = 106005; // no userpoint: no load
	def::DefItemDef jeep = item(6003, "jeep.3di");
	std::strcpy(jeep.virtual_display, "jeep_vd.3di");
	jeep.emplacement_attachments = attachments;
	jeep.emplacement_attachments_count = 2;
	rows.push_back(jeep);
	rows.push_back(item(6004, "mg.3di"));
	rows.push_back(item(6005, "ai.3di"));
	rows.push_back(item(5305, "player.3di")); // the players' own item: re-resolved
	def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	items.vehicle_spawn_ids[0] = 6003;
	items.vehicle_spawn_id_count = 1;

	// weapon.def: a case-folded duplicate, a name with no extension, a foliage name.
	std::vector<def::DefWeaponDef> weapon_rows(3);
	for (def::DefWeaponDef &w : weapon_rows) def::def_init_weapon(w);
	std::strcpy(weapon_rows[0].gfx1, "m4.3di");
	std::strcpy(weapon_rows[0].gfx3, "m4_3p.3di");
	std::strcpy(weapon_rows[1].gfx1, "M4.3DI");
	std::strcpy(weapon_rows[1].gfx3, "glock");
	std::strcpy(weapon_rows[2].gfx1, "pine.3di");
	def::DefWeaponsFile weapons{};
	weapons.entries = weapon_rows.data();
	weapons.count = weapon_rows.size();

	world::AmmoTable ammo;
	ammo.entries.resize(2);
	ammo.entries[0].tracer_item_friendly = 7001;
	ammo.entries[1].tracer_item_enemy = 9999; // no row: index 0, skipped

	inmatch::CharAttrTable charattr;
	for (uint8_t cls = 5; cls <= 9; ++cls) {
		charattr.table.rows[cls - 1].active = true;
		charattr.table.rows[cls - 1].jungle_cammo = 5101;
	}

	auto world = std::make_unique<world::World>();
	for (int pool = 0; pool < 3; ++pool) world->registry.configure_pool(pool, 8);
	world::Entity seed;
	seed.item_id = 6001;
	CHECK(world->registry.spawn(1, seed).valid());
	seed.item_id = 6002;
	CHECK(world->registry.spawn(2, seed).valid());
	seed.item_id = 6005;
	CHECK(world->registry.spawn(0, seed).valid());
	seed.item_id = 5305;
	seed.flags = world::kEntityFlagPlayer;
	seed.player_class = 5;
	CHECK(world->registry.spawn(0, seed).valid());

	inmatch::MissionStartModelSources sources;
	sources.files = &files;
	sources.model_loads = loads;
	sources.climate = bms::ClimateType::Jungle;
	sources.terrain = "synth";
	sources.environment = "synth";
	sources.weapons = &weapons;
	sources.items = &items;
	sources.ammo = &ammo;
	sources.charattr = &charattr;
	sources.world = world.get();
	const inmatch::LoadedModelList list = inmatch::build_mission_start_model_list(sources);

	// Creation order, then the list head first (the reverse).
	const char *created[] = {
			"hdA_j.3DI", "bdA.3DI", "hdB.3DI",                          // the combos (amA fails)
			"trc.3DI",                                                  // the tracer item
			"m4.3DI", "m4_3p.3DI", "glock.3DI", "pine.3DI",             // weapon.def
			"sol_j.3DI", "SOLXA.3DI", "solxb.3DI",                      // the camouflage item
			"item2143.3DI", "wreck.3DI", "wreckfin.3DI", "chute.3DI",   // 2143, 185
			"crate.3DI", "crate_e.3DI", "crate_s.3DI",                  // pool 1
			"bay.3DI", "ai.3DI",                                        // pools 2, 0
			"jeep.3DI", "jeep_vd.3DI", "mg.3DI",                        // the bay's group
			"sun1.3DI", "fmoon4.3DI", "upl.3DI",                        // celestial
			"hud.3DI",                                                  // hudfx.def
			"bush.3DI"};                                                // foliage
	const size_t created_count = sizeof(created) / sizeof(created[0]);
	const std::vector<inmatch::LoadedModelList::Node> nodes = list.nodes();
	CHECK(nodes.size() == created_count);
	for (size_t i = 0; i < nodes.size() && i < created_count; ++i) {
		const inmatch::LoadedModelList::Node &n = nodes[nodes.size() - 1 - i];
		CHECK(n.name == created[i]);
		if (n.name != created[i]) std::printf("  node %zu: %s, expected %s\n", i, n.name.c_str(), created[i]);
		const bool foliage = n.name == "pine.3DI" || n.name == "bush.3DI";
		CHECK(n.foliage == foliage);
	}
	const std::vector<uint32_t> page = list.freeze();
	CHECK(page.size() == created_count - 2);
	for (const uint32_t v : page) CHECK(v == 0u);
	// The variant falls back to the base where the part has none; the arms' file did not
	// load, asked once per combo (the second combo's line has no fourth token, and the
	// tokenizer's stale slot still names ARMS_A, as retail's does: io::ConfigTokens::slot);
	// the final husk is skipped once the "xb" variant loaded; the second combo's body and
	// the weapons' case-folded M4 found the existing node without a load.
	CHECK(std::count(asked.begin(), asked.end(), std::string("amA.3DI")) == 2);
	CHECK(std::count(asked.begin(), asked.end(), std::string("solfin.3DI")) == 0);
	CHECK(std::count(asked.begin(), asked.end(), std::string("M4.3DI")) == 0);
	CHECK(std::count(asked.begin(), asked.end(), std::string("player.3DI")) == 0);

	// The page the joiner's 0x3D answers from: [u32 start] and up to 50 dwords.
	inmatch::JoinerConnection joiner("ModelPage");
	joiner.set_loaded_model_challenge_snapshot(page);
	CHECK(joiner.make_loaded_model_page_reply(0).payload.size() == 4 + page.size() * 4);
	CHECK(joiner.make_loaded_model_page_reply(50).payload.size() == 4);
}

// The node name: up to the FIRST '.', ".3DI" appended; one node per name without case.
void test_node_names() {
	CHECK(inmatch::LoadedModelList::node_name("a.b.3di") == "a.3DI");
	CHECK(inmatch::LoadedModelList::node_name("noext") == "noext.3DI");
	CHECK(inmatch::LoadedModelList::node_name("") == ".3DI");
	inmatch::LoadedModelList list([](const std::string &file) { return file != ".3DI"; });
	CHECK(list.load("Tree.3di") && list.load("TREE.PCX") && list.load("tree"));
	CHECK(!list.load(""));
	CHECK(list.nodes().size() == 1 && list.nodes()[0].name == "Tree.3DI");
}

// The special event: an action 39 of subtype 1 or 2 in any event's actions, whose three items
// the mission start loads after the pools [orig: EventTrigger_HasActiveSpecialEvent @0x452d90;
// Entity_PreloadSpecialItemModels @0x4516e0 - 4529, 4520, 1281].
void test_the_special_event() {
	bms::File mission;
	bms::Action other{};
	other.action_type = static_cast<bms::ActionType>(39);
	other.action_sub_type = 3;
	bms::Action special = other;
	special.action_sub_type = 2;
	mission.actions = {other, special};
	bms::Event first{};
	first.action_index = 0;
	first.action_count = 1;
	mission.events = {first};
	CHECK(!inmatch::mission_has_special_event(mission));
	bms::Event second{};
	second.action_index = 1;
	second.action_count = 1;
	mission.events.push_back(second);
	CHECK(inmatch::mission_has_special_event(mission));

	std::vector<def::DefItemDef> rows;
	rows.push_back(item(0, ""));
	rows.push_back(item(4529, "cargo.3di"));
	rows.push_back(item(1281, "helo.3di"));
	def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	inmatch::MissionStartModelSources sources;
	// Only these three load (the environment's default celestial names do not here).
	sources.model_loads = [](const std::string &file) {
		return file == "cargo.3DI" || file == "helo.3DI" || file == "upl.3DI";
	};
	sources.items = &items;
	sources.special_event = true;
	const std::vector<inmatch::LoadedModelList::Node> nodes =
			inmatch::build_mission_start_model_list(sources).nodes();
	// Head first: upl.3di (always), then the helicopter and the cargo (4520 has no row).
	CHECK(nodes.size() == 3 && nodes[0].name == "upl.3DI" && nodes[1].name == "helo.3DI" &&
			nodes[2].name == "cargo.3DI");
}

// The retail page lengths for a 305-entry list (jox01 01TR): pages 50..250 full, page 300
// holds five entries (24 bytes), past the end the header alone.
void test_the_305_entry_pages() {
	inmatch::JoinerConnection joiner("ModelPage305");
	joiner.set_loaded_model_challenge_snapshot(std::vector<uint32_t>(305, 0u));
	for (uint32_t start = 0; start <= 250; start += 50)
		CHECK(joiner.make_loaded_model_page_reply(start).payload.size() == 4 + 50 * 4);
	CHECK(joiner.make_loaded_model_page_reply(300).payload.size() == 24);
	CHECK(joiner.make_loaded_model_page_reply(350).payload.size() == 4);
}

} // namespace

int main() {
	test_node_names();
	test_the_mission_start_list();
	test_the_special_event();
	test_the_305_entry_pages();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("loaded_model_list_test OK\n");
	return 0;
}
