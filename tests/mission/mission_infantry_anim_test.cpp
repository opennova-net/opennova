// Mission animation installation over synthetic ADM/BAD fixtures, without retail
// data. Exercise the shared kernel through boot, movement, late spawn, restore,
// and the real joiner frame rather than a replacement root-motion provider.
#include <base/io/strutil.h>
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

	Assets() {
		const fs::path parent = test_paths_temp_dir();
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		directory = parent / ("opennova-mission-infantry-" + std::to_string(stamp));
		CHECK(fs::create_directory(directory));
		const fs::path fixtures = fs::path(test_paths_repo_root(__FILE__)) / "fixtures/anim";
		for (const char *name : {"idle.bad", "walk.bad", "soldier.adm", "US01.adm"})
			CHECK(fs::copy_file(fixtures / name, directory / name));
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
	Assets &assets;

	explicit Harness(Assets &source) : assets(source) {
		rows[0].id = mission::kPlayerVisualItemId;
		for (size_t i = 1; i < rows.size(); ++i)
			rows[i].id = mission::kItemIdOffset + 41 + static_cast<int>(i);
		for (auto &row : rows) {
			row.type = mission::kItemTypePerson;
			row.hp = 100;
			std::strcpy(row.ai_function, "org1");
		}
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
		role.bind(*kernel);
		for (int i = 0; i < 40; ++i) role.run_tick(inmatch::TickInput{});
		kernel->local.input.forward = false;
		const w::Vec3 after = kernel->local.player_position();
		CHECK(std::hypot(after.x - before.x, after.y - before.y) > 0.01f);
		const w::AiEntity *body = kernel->local.player_ai();
		CHECK(std::fabs(after.x - body->pos[0] / 65536.0f) < 0.01f);
		CHECK(std::fabs(after.y - body->pos[1] / 65536.0f) < 0.01f);
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
        CHECK(entity->net_anim_phase > 0);
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
        CHECK(world.ai.for_handle(handle)->pos[2] == initial.pos[2]);
        CHECK(world.registry.get(handle)->spawn_position.z == spawn.z);
    }
}

void configured_fallback(Assets &assets) {
	Harness h(assets);
	h.map(0, "US01.adm");
	h.boot(true, true, "soldier.adm");
	CHECK(h.player_adm() != 0);
	CHECK(strutil::iequals(h.kernel->root_motion.adm_name(h.player_adm()), "US01.adm"));
	for (uint16_t type : {43, 44, 45, 46}) CHECK(h.kernel->adm_id_for_runtime_type(type) == 0);
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
	role.bind(*h.kernel);
	role.set_socket(&socket, PeerAddr{});
	role.create_runtime("AnimationTest", inmatch::JoinRole::Player, "", "");
	role.poll_preload();
	role.runtime->seed_session(123, 456, "CLIENT", "SERVER", 1, 0,
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
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	std::printf("mission_infantry_anim: all checks passed\n");
	return 0;
}
