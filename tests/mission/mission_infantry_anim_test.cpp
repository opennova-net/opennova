// Mission animation installation over synthetic ADM/BAD fixtures, without retail
// data. Exercise the shared kernel through boot, movement, late spawn, restore,
// and the real joiner frame rather than a replacement root-motion provider.
#include <base/io/strutil.h>
#include <net/protocol/protocol.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/placement_traits.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/world/player_spawn.h>

#include "common/test_paths.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include "../common/title_fixture.h"

// JO's title identity (net/protocol/protocol.h), the identity these tests speak.
static const opennova::Protocol &kJo = *opennova::title_protocol("jo");

namespace {
using namespace opennova;
namespace w = opennova::world;
namespace fs = std::filesystem;

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
	std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #expr); } while (false)

struct Assets {
	fs::path directory;
	ResourceIndex index;
	assets::AssetStore store{&index};

	// `with_default`: the root also carries a default.adm (soldier.adm's rows).
	explicit Assets(bool with_default = false) {
		const fs::path parent = test_paths_temp_dir();
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		directory = parent / ("opennova-mission-infantry-" + std::to_string(stamp));
		CHECK(fs::create_directory(directory));
		const fs::path fixtures = fs::path(test_paths_repo_root(__FILE__)) / "fixtures/anim";
		for (const char *name : {"idle.bad", "walk.bad", "soldier.adm", "US01.adm"})
			CHECK(fs::copy_file(fixtures / name, directory / name));
		if (with_default) CHECK(fs::copy_file(fixtures / "soldier.adm", directory / "default.adm"));
		{
			std::ofstream file(directory / "unusable.adm", std::ios::binary);
			file << "anim_reset\t\"missing.bad\"\r\n";
			CHECK(static_cast<bool>(file));
		}
		CHECK(index.scan(directory.string()));
		CHECK(!index.has_file(mission::kDefaultInfantryAdm));
	}
	~Assets() {
		// Only this uniquely created test directory is owned by the harness.
		std::error_code error;
		fs::remove_all(directory, error);
	}
};

struct Harness {
	std::array<def::DefItemDef, 6> rows{};
	def::DefItemsFile items{rows.data(), rows.size()};
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	bool kernel_jo = test_titles::jo_world(kernel->world);
	Assets &assets;

	explicit Harness(Assets &source) : assets(source) {
		// The row the player's type word names (0x14B9 + the item id offset).
		rows[0].id = static_cast<int>(w::kPlayerInfantryTypeId) + mission::kItemIdOffset;
		for (size_t i = 1; i < rows.size(); ++i)
			rows[i].id = mission::kItemIdOffset + 41 + static_cast<int>(i);
		for (auto &row : rows) {
			row.type = mission::kItemTypePerson;
			row.hp = 100;
			std::strcpy(row.ai_function, "org1");
			std::strcpy(row.move_function, "org1"); // retail's org1 person rows move on org1
		}
		// The player's row moves on org2, as retail's 105305 does (D-NET-892).
		std::strcpy(rows[0].move_function, "org2");
		map(0, "soldier"); // exercise the implicit .adm extension
		map(1, "US01.ADM"); // case-insensitive registration
		map(3, "missing.adm");
		map(4, "unusable.adm");
		kernel->set_items_table(&items);
		kernel->set_assets(&assets.store);
	}

	void map(size_t row, const char *name) { std::strcpy(rows[row].anim_def, name); }

	void boot(bool playable = true, bool organics = true,
			const char *default_map = mission::kDefaultInfantryAdm, bool premission = false) {
		bms::File document;
		if (organics) {
			for (int i = 1; i < 5; ++i) {
				bms::Entity e{};
				e.type = bms::ItemType::Organic;
				e.type_id = 41 + i;
				e.id = static_cast<uint16_t>(i);
				e.x = (i * 20) * 65536;
				e.yaw = 90;
				e.team = 1;
				document.organics.push_back(e);
			}
		}
		if (premission) {
			document.organics[0].group_id = 7;
			document.organics[0].spawns = 2;
			rows[5].type = 1;
			rows[5].attrib = w::kItemAttribSpawnPoint;
			rows[5].move_function[0] = '\0'; // a spawn-point item names no mover
			bms::Entity zone{};
			zone.type = bms::ItemType::Item; zone.type_id = 46; zone.id = 20;
			zone.group_id = 7; zone.team = 2;
			document.items.push_back(zone);
			bms::Event event{}; event.flags = bms::EventFlags::PreMission;
			event.action_count = 1;
			bms::Action action{}; action.action_type = bms::ActionType::SingleChangeGroup;
			action.param1 = 1; action.param2 = 8;
			document.events = {event}; document.actions = {action};
		}
		mission::BootFileSource files;
		files.has_file = [this](const std::string &name) { return assets.index.has_file(name); };
		files.read_file = [this](const std::string &name, std::vector<uint8_t> &out) {
			return assets.index.read_file(name, out);
		};
		kernel->open_document(std::move(document), "synthetic-infantry", files);
		// A flat ground field makes the test independent of retail terrain.
		CptFile cpt;
		cpt.depth_buffer.assign(64, 0);
		TrnConfig trn;
		trn.sector_count = trn.sector_rows = 16;
		trn.origin_x = trn.origin_y = -8;
		for (auto &row : trn.sector_grid)
			for (int &cell : row) cell = 1;
		terrain::terrain_field_store_build(kernel->terrain_store, cpt, trn);
		mission::KernelBootOptions options;
		options.playable = playable;
		options.wac = false;
		options.collision = false;
		options.seat_specs = false;
		options.infantry_adm = default_map;
		std::string error;
		CHECK(kernel->boot(options, error));
		CHECK(error.empty());
	}

	int player_adm() const {
		CHECK(kernel->local.player_ai() != nullptr);
		return kernel->local.player_ai()->inf.adm_id;
	}

	void check_movement() {
		const w::Vec3 before = kernel->local.player_position();
		kernel->local.input.forward = true;
		inmatch::LocalRole role;
		test_titles::jo_controls(role);
		role.bind(*kernel);
		for (int i = 0; i < 40; ++i) role.run_tick(inmatch::TickInput{});
		kernel->local.input.forward = false;
		const w::Vec3 after = kernel->local.player_position();
		CHECK(std::hypot(after.x - before.x, after.y - before.y) > 0.01f);
		const w::AiEntity *body = kernel->local.player_ai();
		CHECK(std::fabs(after.x - body->self->position.x.q16 / 65536.0f) < 0.01f);
		CHECK(std::fabs(after.y - body->self->position.y.q16 / 65536.0f) < 0.01f);
	}
};

void boot_without_default(Assets &assets) {
	Harness h(assets);
	h.boot();
	CHECK(h.kernel->world.ai.root_motion == &h.kernel->root_motion);
	CHECK(h.kernel->root_motion.adm_name(h.player_adm()) == "soldier.adm");
	const int npc = h.kernel->adm_id_for_runtime_type(42);
	CHECK(npc >= 0 && npc != h.player_adm());
	CHECK(!h.kernel->root_motion.has_clip(npc, w::anim_state::kWalkForward));
	for (uint16_t type : {43, 44, 45, 46})
		CHECK(h.kernel->adm_id_for_runtime_type(type) == -1);
	for (int i = 0; i < h.kernel->world.ai.count(); ++i) {
		const w::AiEntity *body = h.kernel->world.ai.at(i);
		const w::Entity *entity = h.kernel->world.registry.get(body->handle);
		CHECK(entity != nullptr);
		if (entity->item_id >= 43 && entity->item_id <= 45) CHECK(body->inf.adm_id == -1);
	}
	h.check_movement();
	h.kernel->capture_baseline();
	const w::Vec3 saved = h.kernel->local.player_position();
	h.check_movement();
	CHECK(h.kernel->restore_baseline());
	CHECK(h.kernel->local.player_position().x == saved.x);
	CHECK(h.kernel->local.player_position().y == saved.y);
	CHECK(h.kernel->root_motion.adm_name(h.player_adm()) == "soldier.adm");
	h.check_movement();
}

void initialized_before_premission_and_retry(Assets &assets) {
    Harness h(assets);
    h.rows[1].clipsize = 19; // no ammo.def: magazine still comes from the item definition
    h.boot(false, true, mission::kDefaultInfantryAdm, true);
    auto &world = h.kernel->world;
    w::EntityHandle handle;
    world.registry.for_each_in_pool(0, [&](const w::Entity &entity) {
        if (entity.net_id == 1) handle = entity.handle;
    });
    CHECK(handle.valid());
    const auto verify = [&] {
        const auto *entity = world.registry.get(handle);
        const auto *body = world.ai.for_handle(handle);
        CHECK(entity && body);
        CHECK(entity->group_id == 8); // changed by the real PreMission action
        CHECK(entity->npc_respawn_zone.valid()); // linked using the authored group 7
        CHECK(world.registry.get(entity->npc_respawn_zone)->group_id == 7);
        CHECK(entity->hidden && (entity->spawn_flags & 1) == 0);
        CHECK(entity->npc_respawns == 2);
        CHECK(body->inf.magazine == 19);
        CHECK(body->inf.clip_phase > 0 && body->inf.wpn_clip_phase > 0);
        CHECK(w::wire_anim_ratio(body->inf, world.ai.root_motion) > 0);
    };
    verify();
    const auto initial = *world.ai.for_handle(handle);
    const auto spawn = world.registry.get(handle)->spawn_position;
    for (int retry = 0; retry < 2; ++retry) {
        world.commands.set_entity_position(handle, {90, 80, 7});
        world.ai.for_handle(handle)->inf.magazine = 1;
        world.ai.for_handle(handle)->inf.clip_phase += 30;
        world.registry.get(handle)->npc_respawn_zone = {};
        CHECK(h.kernel->restore_baseline());
        verify();
        CHECK(world.ai.for_handle(handle)->inf.clip_phase == initial.inf.clip_phase);
        CHECK(world.ai.for_handle(handle)->self->position.z.q16 == initial.self->position.z.q16);
        CHECK(world.registry.get(handle)->spawn_position.z == spawn.z);
    }
}

void configured_fallback(Assets &assets) {
	Harness h(assets);
	h.map(0, "US01.adm");
	h.boot(true, true, "soldier.adm");
	CHECK(h.player_adm() != 0);
	CHECK(strutil::iequals(h.kernel->root_motion.adm_name(h.player_adm()), "US01.adm"));
	// A table the load cannot find (missing.adm, and no default.adm) takes the
	// configured map (D-INF-33); one that opens with no RESET row (unusable.adm,
	// its RESET naming a missing .bad) holds no channel [orig: AnimMap_RegisterEntity
	// @0x40BBBC..0x40BBC6 -> @0x40BD8B]; an item naming none (types 43, 46) registers
	// none [orig: Entity_SpawnFromItemDef @0x452571, the empty animDef skips the load].
	CHECK(h.kernel->adm_id_for_runtime_type(44) == 0);
	for (uint16_t type : {43, 45, 46}) CHECK(h.kernel->adm_id_for_runtime_type(type) == -1);
	CHECK(h.kernel->install_infantry_anim("soldier.adm") > 0);
	CHECK(h.kernel->install_infantry_anim(mission::kDefaultInfantryAdm) == 0);
	CHECK(h.kernel->world.ai.root_motion != nullptr);
	CHECK(h.player_adm() == 0); // US01 is now the first successful registration
	CHECK(h.kernel->adm_id_for_runtime_type(43) == -1); // it is NOT the default
}

void no_clips_then_rearm(Assets &assets) {
	Harness h(assets);
	for (size_t i = 0; i < h.rows.size(); ++i) h.map(i, "unusable.adm");
	h.boot();
	CHECK(h.kernel->root_motion.empty());
	CHECK(h.kernel->world.ai.root_motion == nullptr);
	CHECK(h.player_adm() == -1);
	const w::Vec3 before = h.kernel->local.player_position();
	h.kernel->local.input.forward = true;
	inmatch::LocalRole role;
	test_titles::jo_controls(role);
	role.bind(*h.kernel);
	for (int i = 0; i < 10; ++i) role.run_tick(inmatch::TickInput{});
	CHECK(h.kernel->local.player_position().x == before.x);
	CHECK(h.kernel->local.player_position().y == before.y);
	h.map(0, "soldier");
	h.kernel->rearm_infantry_adm();
	CHECK(h.kernel->world.ai.root_motion == &h.kernel->root_motion);
	CHECK(h.player_adm() >= 0);
	h.check_movement();
	CHECK(h.kernel->adm_id_for_runtime_type(42) == -1);
	h.map(1, "US01.adm");
	h.kernel->rearm_infantry_adm();
	CHECK(h.kernel->adm_id_for_runtime_type(42) >= 0); // cached failure was invalidated
}

void late_spawn_and_type_resolution(Assets &assets) {
	Harness h(assets);
	CHECK(h.kernel->adm_id_for_runtime_type(w::kPlayerInfantryTypeId) == -2);
	h.boot(false, false);
	CHECK(h.kernel->root_motion.empty());
	CHECK(h.kernel->world.ai.root_motion == nullptr);
	CHECK(h.kernel->spawn_local_player(w::PlayerSpawn{}));
	CHECK(h.player_adm() == 0);
	CHECK(h.kernel->world.ai.root_motion == &h.kernel->root_motion);
	h.check_movement();

	Harness decoded(assets);
	decoded.boot(false, false);
	CHECK(decoded.kernel->root_motion.empty());
	CHECK(decoded.kernel->adm_id_for_runtime_type(w::kPlayerInfantryTypeId) == 0);
	CHECK(decoded.kernel->world.ai.root_motion == &decoded.kernel->root_motion);
	CHECK(decoded.kernel->cached_adm_id_for_runtime_type(w::kPlayerInfantryTypeId) == 0);
	CHECK(decoded.kernel->adm_id_for_runtime_type(43) == -1);
	CHECK(decoded.kernel->root_motion.set_count() == 1);
}

class QuietSocket : public IDatagramSocket {
public:
	int recv_from(uint8_t *, size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, size_t) override {}
};

void joiner_resolves_empty_registry(Assets &assets) {
	Harness h(assets);
	h.boot(false, false);
	QuietSocket socket;
	inmatch::JoinerRole role;
	test_titles::jo_joiner(role);
	role.set_rules(opennova::world::title_rules("jo"));
	role.set_protocol(&kJo);
	role.bind(*h.kernel);
	role.set_socket(&socket, PeerAddr{});
	role.create_runtime("AnimationTest", inmatch::JoinRole::Player, "", "");
	role.poll_preload();
	role.seed_session(123, 456, "CLIENT", "SERVER", 1, 0,
			5, w::kPlayerInfantryTypeId);
	replication::ClientEntityState row{};
	row.handle = 6;
	row.cls = EntityClass::Infantry;
	row.type_id = w::kPlayerInfantryTypeId;
	row.net_has_compact = true;
	row.anim_state_id = w::anim_state::kWalkForward;
	role.runtime->state().entities.push_back(row);
	CHECK(h.kernel->root_motion.empty());
	role.run_tick(inmatch::TickInput{});
	const auto &resolved = role.runtime->state().entities.front();
	CHECK(resolved.rm_adm_id >= 0);
	CHECK(h.kernel->root_motion.adm_name(resolved.rm_adm_id) == "soldier.adm");
	CHECK(resolved.rm_state == w::anim_state::kWalkForward); // source bound in the same frame

	// Reinstall changes registry indices. The portable joiner must re-resolve
	// without a Godot shell manually clearing its retained rows.
	CHECK(h.kernel->install_infantry_anim("US01.adm") > 0);
	role.run_tick(inmatch::TickInput{});
	CHECK(role.runtime->state().entities.front().rm_adm_id != 0);
	h.map(0, "missing.adm");
	h.kernel->rearm_infantry_adm();
	role.run_tick(inmatch::TickInput{});
	CHECK(role.runtime->state().entities.front().rm_adm_id == 0);
	CHECK(h.kernel->install_infantry_anim(mission::kDefaultInfantryAdm) == 0);
	role.run_tick(inmatch::TickInput{});
	CHECK(role.runtime->state().entities.front().rm_adm_id == -1);
}

// An item's map is the spawn's one .adm load: the name cut at its last '.'
// with ".adm" appended, default.adm where the roots lack that file, ahead of
// the kernel's configured infantry map [orig: Entity_SpawnFromItemDef
// @0x45257c -> AnimMap_LoadAdmFile @0x40ccb9..0x40cd25].
void missing_map_loads_default_adm() {
	Assets assets(true);
	Harness h(assets);
	h.map(2, "soldier.txt"); // any extension is swapped: soldier.adm
	h.boot();
	const int swapped = h.kernel->adm_id_for_runtime_type(43);
	CHECK(swapped >= 0 && h.kernel->root_motion.adm_name(swapped) == "soldier.adm");
	const int missing = h.kernel->adm_id_for_runtime_type(44); // "missing.adm"
	CHECK(missing >= 0 && h.kernel->root_motion.adm_name(missing) == "default.adm");
	CHECK(h.kernel->adm_id_for_runtime_type(46) == -1); // no map: none
}

// A body whose brain takes a freed AI slot registers its own map: attach() reuses the
// lowest freed slot, below brains the kernel registered already (a host player who
// leaves, then one who joins; a joiner's retired replica, then the next streamed
// person), and that spawn's registration still reaches it [orig:
// Entity_SpawnFromAnimSlotProperty @0x43C390 - the AnimMap_RegisterEntity @0x40BB60
// call @0x43C4DF].
void reused_ai_slot_registers_its_own_map(Assets &assets) {
	Harness h(assets);
	h.boot();
	auto &world = h.kernel->world;
	w::EntityHandle leaver, no_map;
	world.registry.for_each_in_pool(0, [&](const w::Entity &entity) {
		if (entity.item_id == 42) leaver = entity.handle;
		if (entity.item_id == 43) no_map = entity.handle;
	});
	CHECK(leaver.valid() && no_map.valid());
	int freed = -1;
	for (int i = 0; i < world.ai.count(); ++i)
		if (world.ai.at(i) != nullptr && world.ai.at(i)->handle == leaver) freed = i;
	CHECK(freed >= 0 && freed < world.ai.count());
	world.ai.release(leaver);
	// A new body of the type that names no map (-1), so the wrong result (the fresh
	// slot's registration 0) differs from the right one.
	w::Entity seed = *world.registry.get(no_map);
	seed.net_id = 0x99;
	seed.bms_id = 0;
	const w::EntityHandle joined = world.registry.spawn(0, seed);
	CHECK(joined.valid());
	const int index = world.ai.attach(joined);
	CHECK(index == freed);
	world.ai.at(index)->inf.active = true;
	CHECK(world.ai.at(index)->inf.adm_id != -1);
	h.kernel->resolve_new_infantry_adm_ids();
	CHECK(h.kernel->adm_id_for_runtime_type(43) == -1);
	CHECK(world.ai.at(index)->inf.adm_id == -1);
	// The brains registered before keep theirs: the walk passes them over.
	const int player = h.player_adm();
	h.kernel->resolve_new_infantry_adm_ids();
	CHECK(h.player_adm() == player);
	CHECK(world.ai.at(index)->inf.adm_id == -1);
}

// The boot's own spawn reads its bound row: the load step's traits sweep fills the row
// table (world.tables.player, item_rows) ahead of the spawn step, as retail resolves every
// def's rows once at Game_InitSubsystems, ahead of the mission start's round init. The
// authored Player row's hp 120 is the spawn's ceiling (the seed's fallback is 100, and the
// later sweep lifts no player body) and its org1 tag the row's class (D-NET-892 (c)).
// [orig: EntityDef_InitAllCallbacks @0x4A5A70, called @0x4A71D7; Game_StartMission
//  @0x524360 - the Server_InitAllPlayerEntitiesForRound call @0x525BAF;
//  Entity_SpawnFromAnimSlotProperty @0x43C390 - the class init called @0x43C4FE, the raise
//  @0x43C557..0x43C56B]
void boot_spawn_reads_its_bound_row(Assets &assets) {
	Harness h(assets);
	h.rows[0].hp = 120;
	h.boot();
	CHECK(h.kernel->world.tables.player.body_class == w::PlayerBodyClass::kOrganic);
	CHECK(h.kernel->local.player() != nullptr);
	CHECK(h.kernel->local.player()->health == 120);
}

} // namespace

int main() {
	try {
		Assets assets;
		boot_without_default(assets);
		initialized_before_premission_and_retry(assets);
		configured_fallback(assets);
		no_clips_then_rearm(assets);
		late_spawn_and_type_resolution(assets);
		joiner_resolves_empty_registry(assets);
		missing_map_loads_default_adm();
		boot_spawn_reads_its_bound_row(assets);
		reused_ai_slot_registers_its_own_map(assets);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	std::printf("mission_infantry_anim: all checks passed\n");
	return 0;
}
