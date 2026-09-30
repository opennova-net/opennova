#include <formats/mus/mus.h>
// The mission kernel (ADR 0042 d3) booted over a SYNTHETIC mission and an
// in-memory file source — ungated. Locks the promoted rig body's engine home:
// open_document + boot run the S9 order end to end (entities promoted, the
// layered WAC compiled and installed, the local player spawned at the origin
// when no start marker exists, the post-PreMission baseline captured), a
// the local role's tick advances the logic clock, the teleport/health seams round-trip
// through both stores, and the CanFire verdict answers over the spawned
// player. The retail-path legs stay in tests/common/retail_mission_files.
#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <formats/wac/bytecode.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/mission_kernel.h>

#include "common/boot_file_source.h"
#include "common/file_io.h"
#include "common/synthetic_mission.h"
#include "common/test_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
namespace ms = opennova::mission;
namespace w = opennova::world;

static int failures = 0;
#define CHECK(c)                                                                            \
	do {                                                                                    \
		if (!(c)) {                                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                        \
			++failures;                                                                     \
		}                                                                                   \
	} while (0)

namespace {

using test_mission::item;
using test_mission::organic;
using test_boot::source_over;
using test_boot::tick_no_net;

bool near_equal(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

// The boot gates through the trace: a joiner never spawns its own player
// here (L spawns on the name-match inside the joiner frame); a rootless boot
// keeps only the root-free steps.
static void run_boot_trace_gates() {
    for (bool joiner : {false, true}) {
        bms::File mission{};
        auto sp_only = item(164, 0, 0, 0);
        sp_only.id = 71;
        sp_only.bmsi_attributes = 0x40;
        auto mp_only = sp_only;
        mp_only.id = 72;
        mp_only.bmsi_attributes = 0x80;
        mission.items = {sp_only, mp_only};
        bms::Event event{};
        event.flags = bms::EventFlags::PreMission;
        event.action_count = 1;
        mission.events.push_back(event);
        bms::Action action{};
        action.action_type = bms::ActionType::OutputText;
        action.param1 = 12;
        mission.actions.push_back(action);
        ms::MissionKernel kernel;
        kernel.open_document(mission, "boot-gates", {});
        ms::KernelBootOptions options;
        options.playable = false;
        options.mp_session = true;
        options.joiner = joiner;
        std::string error;
        CHECK(kernel.boot(options, error));
        CHECK(kernel.world.rules.mp_session);
        CHECK(kernel.world.registry.by_net_id(71) == nullptr);
        CHECK(kernel.world.registry.by_net_id(72) != nullptr);
        CHECK(kernel.world.out.effects.count("text") == (joiner ? 0u : 1u));
    }

	std::map<std::string, std::string> files;
	{
		bms::File m{};
		m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1, /*yaw=*/90));
		ms::MissionKernel kernel;
		kernel.open_document(std::move(m), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.joiner = true;
		std::string error;
		CHECK(kernel.boot(options, error));
		const std::vector<std::string> expected = {
				"ai_profiles", "mission_text", "load_mission", "infantry_anim",
				"script_catalogs", "wac", "weapon_table", "ammo_table", "powerup_table", "organic_init"};
		CHECK(kernel.boot_trace == expected);
		CHECK(!kernel.local.has_local_player());
	}
	{
		bms::File m{};
		m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1, /*yaw=*/90));
		ms::MissionKernel kernel;
		kernel.open_document(std::move(m), "synth", ms::BootFileSource{});
		ms::KernelBootOptions options;
		std::string error;
		CHECK(kernel.boot(options, error));
		const std::vector<std::string> expected = {
				"mission_text", "load_mission", "wac", "spawn_local_player", "organic_init", "premission"};
		CHECK(kernel.boot_trace == expected);
		CHECK(kernel.local.has_local_player());
	}
}

// Only the authority compiles the mission's WAC layers: a joiner's load skips
// all three compiles and installs the bare terminator.
// [orig: WacScript_InitAndLoad @0x4F9437 (the authority test), @0x4F944E,
//  @0x4F95A9]
static void test_joiner_installs_only_the_wac_terminator() {
	for (bool joiner : {false, true}) {
		std::map<std::string, std::string> files;
		files["synth.wac"] = "if never() then inc(v1) endif\n";
		bms::File m{};
		m.organics.push_back(organic(1 << 16, 1 << 16, 0, /*team=*/1, /*yaw=*/90));
		ms::MissionKernel kernel;
		kernel.open_document(std::move(m), "synth", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
		options.mp_session = true;
		options.joiner = joiner;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(kernel.wac.vm().loaded());
		CHECK(kernel.wac_loaded == !joiner);
		if (joiner) {
			CHECK(kernel.wac.program().code.size() == 1);
			CHECK(kernel.wac.program().code[0] == wac::kProgramTerminator);
			CHECK(kernel.wac.program().event_count == 0);
		} else {
			CHECK(kernel.wac.program().event_count == 1);
		}
	}
}

// Every peer starts the mission on the same frame clock: retail zeroes `tick`
// on the host and on each client alike, past the authority-only pre pass, and
// advances it ahead of every frame's entity update, so the first frame runs at
// tick 1 whether or not this peer ran the pre pass.
// [orig: Game_StartMission `mov tick, ebx` (ebx = 0) @0x525B9F, the
//  is_authority gate @0x525B78; Game_ProcessMainFrame `add tick, ebx`
//  @0x5265B4 ahead of the Entity_UpdateAllEntities call @0x52674B]
static void test_first_frame_tick_matches_on_host_and_joiner() {
	for (bool joiner : {false, true}) {
		bms::File mission{};
		mission.items = {item(164, 0, 0, 0)};
		ms::MissionKernel kernel;
		kernel.open_document(mission, "first-frame", {});
		ms::KernelBootOptions options;
		options.playable = false;
		options.mp_session = true;
		options.joiner = joiner;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(kernel.world.logic_tick == 1u);
	}
}

// PreMission has one shared variable bank, but its numbered writes are not
// initial WAC input. The same startup reset applies to BMS-only missions.
static void test_numbered_vars_reset_after_premission_before_initial_wac() {
	for (int source_kind = 0; source_kind < 3; ++source_kind) {
		const bool has_wac = source_kind == 2;
		std::map<std::string, std::string> files;
		if (has_wac) files["synth.wac"] = "var carried\nv2 = v1\ninc(v1)\ninc(carried)\n";
		bms::File mission{};
		bms::Event first{};
		first.flags = bms::EventFlags::PreMission;
		first.action_count = 2;
		bms::Event second = first;
		second.trigger_count = 1;
		second.action_index = 2;
		second.action_count = 3;
		mission.events = {first, second};
		bms::Trigger sees_first{};
		sees_first.main_type = bms::TriggerMainType::MissionVariable;
		sees_first.sub_type = static_cast<int32_t>(
				bms::MissionVariableTriggerType::MissionVariableIsEqual);
		sees_first.param1 = 1;
		sees_first.param2 = 40;
		mission.triggers = {sees_first};
		const auto set_variable = [](int index, int value) {
			bms::Action action{};
			action.action_type = bms::ActionType::MisvarChange;
			action.action_sub_type = static_cast<int32_t>(bms::MissionVariableActionSubType::Set);
			action.param1 = index;
			action.param2 = value;
			return action;
		};
		bms::Action output{};
		output.action_type = bms::ActionType::OutputText;
		output.param1 = 12;
		mission.actions = {set_variable(0, 17), set_variable(1, 40),
				set_variable(1, 41), set_variable(255, 51), output};
		ms::MissionKernel kernel;
		kernel.world.script.vars.set_mission(256, 7);
		kernel.open_document(std::move(mission), "synth",
				source_kind == 0 ? ms::BootFileSource{} : source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
		options.defer_mission_start = true;
		std::string error;
		CHECK(kernel.boot(options, error));
		CHECK(kernel.world.out.effects.count("text") == 1);
		CHECK(kernel.world.script.vars.get_mission(0) == 17);
		CHECK(kernel.world.script.vars.get_mission(1) == 41);
		CHECK(kernel.world.script.vars.get_mission(255) == 51);
		CHECK(kernel.world.script.vars.get_mission(256) == 7);
		CHECK(kernel.wac.runs() == 0);
		CHECK(!kernel.have_baseline);
		CHECK(kernel.complete_mission_start());
		CHECK(kernel.world.script.vars.get_mission(0) == 0);
		CHECK(kernel.world.script.vars.get_mission(1) == (has_wac ? 1 : 0));
		CHECK(kernel.world.script.vars.get_mission(2) == 0);
		CHECK(kernel.world.script.vars.get_mission(255) == 0);
		CHECK(kernel.world.script.vars.get_mission(256) == (has_wac ? 8 : 7));
		CHECK(kernel.have_baseline);
		kernel.world.script.vars.set_mission(1, 99);
		CHECK(!kernel.complete_mission_start());
		CHECK(kernel.world.script.vars.get_mission(1) == 99);
		CHECK(kernel.restore_baseline());
		CHECK(kernel.world.script.vars.get_mission(1) == (has_wac ? 1 : 0));
		CHECK(kernel.world.script.vars.get_mission(256) == (has_wac ? 8 : 7));
	}
}

// Missing files still initialize the terminator-only program. Its startup
// clock holds an empty host, and the sealed clock is visible as soon as restore
// returns. An explicit WAC-disable option retains the unloaded-script contract.
static void test_empty_wac_clock_and_baseline_gate() {
    for (int source_kind = 0; source_kind < 4; ++source_kind) {
        const bool enabled = source_kind != 3;
        std::map<std::string, std::string> files;
        if (source_kind == 2) files["synth.wac"] = "";
        if (!enabled) files["synth.wac"] = "inc(v1)\n";
        bms::File mission{};
        bms::Event event{};
        event.action_count = 1;
        mission.events = {event};
        bms::Action output{};
        output.action_type = bms::ActionType::OutputText;
        output.param1 = 12;
        mission.actions = {output};
        ms::MissionKernel kernel;
        kernel.open_document(std::move(mission), "synth",
                source_kind == 0 ? ms::BootFileSource{} : source_over(&files));
        ms::KernelBootOptions options;
        options.playable = false;
        options.wac = enabled;
        std::string error;
        CHECK(kernel.boot(options, error));
        CHECK(kernel.wac.vm().loaded() == enabled);
        CHECK(kernel.wac.vm().time() == (enabled ? 1u : 0u));
        CHECK(kernel.world.cached.wac_ticks == (enabled ? 1 : 0));
        CHECK(kernel.world.script_may_advance() == !enabled);
        for (int i = 0; i < 64; ++i) kernel.world.run_logic_tick();
        CHECK(kernel.world.out.effects.count("text") == (enabled ? 0u : 1u));
        CHECK(kernel.wac.runs() == (enabled ? 1u : 0u));
        if (!enabled) continue;
        kernel.world.cached.humans = 1;
        for (int i = 0; i < wac::WacSystem::kTicksPerExecution; ++i)
            kernel.world.run_logic_tick();
        CHECK(kernel.wac.vm().time() == 2);
        CHECK(kernel.world.cached.wac_ticks == 2);
        CHECK(kernel.restore_baseline());
        CHECK(kernel.wac.vm().time() == 1);
        CHECK(kernel.world.cached.wac_ticks == 1);
        CHECK(!kernel.world.script_may_advance());
        CHECK(!kernel.wac.execute_initial(kernel.world));
        kernel.world.cached.humans = 1;
        for (int i = 0; i < wac::WacSystem::kTicksPerExecution - 1; ++i)
            kernel.world.run_logic_tick();
        CHECK(kernel.wac.vm().time() == 1);
        kernel.world.run_logic_tick();
        CHECK(kernel.wac.vm().time() == 2);
    }
}

// A Stop/restart that finds a UseGun switch still pending cancels only the
// personal slot's action state: its magazine, its reserve and its scope zoom
// (MountSlot+0xC, slot state seeded once per slot) survive the cancel.
// [orig: WeaponSlot_InitFromDef @0x53EF35]
static void test_restart_cancel_keeps_the_personal_slot_zoom() {
	ms::MissionKernel kernel;
	kernel.open_document(bms::File{}, "synth", ms::BootFileSource{});
	ms::KernelBootOptions options;
	options.playable = false;
	std::string error;
	CHECK(kernel.boot(options, error));
	kernel.local.weapon.usegun_switch = w::LocalUseGunSwitch::kAttach;
	kernel.local.weapon.slot.current = w::weapon_action::kSwitchFrom;
	kernel.local.weapon.slot.clip = 9;
	kernel.local.weapon.slot.reserve = 30;
	kernel.local.weapon.slot.scope_zoom = 6;
	CHECK(kernel.restore_baseline());
	CHECK(kernel.local.weapon.slot.current != w::weapon_action::kSwitchFrom);
	CHECK(kernel.local.weapon.slot.clip == 9 && kernel.local.weapon.slot.reserve == 30);
	CHECK(kernel.local.weapon.slot.scope_zoom == 6);
}

static void test_initial_wac_waits_for_the_weather_owner_once() {
    std::map<std::string, std::string> files;
    files["synth.wac"] = "if never then inc(v1) fov(40) endif\n";
    ms::MissionKernel kernel;
    kernel.open_document(bms::File{}, "synth", source_over(&files));
    ms::KernelBootOptions options;
    options.playable = false;
    options.defer_mission_start = true;
    std::string error;
    CHECK(kernel.boot(options, error));
    CHECK(kernel.wac.runs() == 0);
    CHECK(kernel.world.script.vars.get_mission(1) == 0);
    kernel.world.weather.seed(w::WeatherSeed{});
    CHECK(!kernel.have_baseline);
    CHECK(kernel.complete_mission_start());
    CHECK(!kernel.complete_mission_start());
    CHECK(kernel.world.script.vars.get_mission(1) == 1);
    CHECK(kernel.world.weather.core.scalar_channels.camera_fov_fp == (40 << 16));
    CHECK(kernel.have_baseline);
    kernel.world.weather.command_fov(120);
    kernel.tick_weather();
    CHECK(kernel.restore_baseline());
    CHECK(kernel.world.script.vars.get_mission(1) == 1);
    CHECK(kernel.world.weather.core.scalar_channels.camera_fov_fp == (40 << 16));
    CHECK(kernel.world.weather.core.scalar_channels.camera_fov_target_fp == (40 << 16));
    CHECK(!kernel.wac.execute_initial(kernel.world));
}


static void test_vehicle_spawn_pose_is_captured_after_initial_wac() {
 std::map<std::string, std::string> files;
 files["synth.wac"] = "if never then teleport(7,42) endif\n";
 ms::MissionKernel kernel;
 bms::File mission{};
 mission.items.push_back(item(164, 1 << 16, 2 << 16, 0));
 kernel.open_document(std::move(mission), "synth", source_over(&files));
 ms::KernelBootOptions options;
 options.playable = false;
 options.defer_mission_start = true;
 std::string error;
 CHECK(kernel.boot(options,error));
 const auto vehicle = w::EntityHandle::make(1,0);
 auto *hull = kernel.world.registry.get(vehicle);
 CHECK(hull != nullptr);
 if (hull == nullptr) return;
 hull->group_id = 7;
 // No items table in this kernel: stamp the def row's ordinal the teleport's
 // +0x1C member gate reads [orig: Entity_TeleportTeamToSpawn @0x43D3EE].
 hull->item_type_index = 7;
 w::VehicleTraits traits;
 traits.player_control = true;
 kernel.world.vehicles.traits.set(hull->item_id,traits);
 kernel.world.registry.configure_pool(3,4);
 w::Entity marker;
 marker.item_id = 6088;
 marker.wp_number = 42;
 marker.position = {100,200,30};
 kernel.world.registry.spawn(3,marker);
 CHECK(!hull->veh.spawn_pose_valid);
 CHECK(kernel.complete_mission_start());
 hull = kernel.world.registry.get(vehicle);
 CHECK(hull->position.x == 100 && hull->position.y == 200);
 CHECK(hull->veh.spawn_pose_valid && hull->veh.spawn_pose[0] == 100 * 65536);
 hull->position.x = 300;
 CHECK(kernel.restore_baseline());
 hull = kernel.world.registry.get(vehicle);
 CHECK(hull->position.x == 100 && hull->veh.spawn_pose[0] == 100 * 65536);
}

// SndProf.def is parsed by the kernel's script_catalogs step exactly once, and
// a table an embedder filled before the boot wins (the parse appends).
static void test_sound_profiles_parse_once_and_keep_a_pre_boot_override() {
    std::map<std::string, std::string> files;
    files["SndProf.def"] = "begin alpha\nend\nbegin beta\nend\n";
    {
        ms::MissionKernel kernel;
        kernel.open_document(bms::File{}, "synth", source_over(&files));
        ms::KernelBootOptions options;
        options.playable = false;
        std::string error;
        CHECK(kernel.boot(options, error));
        const auto &entries = kernel.world.tables.sound_profiles.entries();
        CHECK(entries.size() == 2);
        CHECK(entries.size() == 2 && entries[0].name == "alpha" && entries[1].name == "beta");
    }
    {
        ms::MissionKernel kernel;
        kernel.open_document(bms::File{}, "synth", source_over(&files));
        const std::string pre = "begin embedder\nend\n";
        kernel.world.tables.sound_profiles.parse(pre.data(), pre.size());
        ms::KernelBootOptions options;
        options.playable = false;
        std::string error;
        CHECK(kernel.boot(options, error));
        const auto &entries = kernel.world.tables.sound_profiles.entries();
        CHECK(entries.size() == 1);
        CHECK(!entries.empty() && entries[0].name == "embedder");
    }
}


static void test_initial_wac_binds_the_preopened_music_context() {
    for (bool context_active : {false, true}) {
        const std::map<std::string, std::string> files = {
            {"synth.wac", "set(v1,m7) set(m7,55) set(v2,m7)\n"}
        };
        auto audio = std::unique_ptr<mus::MusVM, decltype(&mus::mus_vm_destroy)>(
                mus::mus_vm_create(), mus::mus_vm_destroy);
        mus::MusScript music_script{};
        if (context_active) CHECK(mus::mus_vm_load_script(audio.get(), &music_script) == 0);
        mus::mus_vm_set_var(audio.get(), 7, 99);
        ms::MissionKernel kernel;
        kernel.open_document(bms::File{}, "synth", source_over(&files));
        ms::KernelBootOptions options;
        options.playable = false;
        options.music_globals = mus::mus_vm_globals(audio.get());
        std::string error;
        CHECK(kernel.boot(options, error));
        CHECK(kernel.wac.runs() == 1);
        CHECK(kernel.world.script.vars.get_mission(1) == (context_active ? 99 : 0));
        CHECK(kernel.world.script.vars.get_mission(2) == 55);
        CHECK(mus::mus_vm_get_var(audio.get(), 7) == (context_active ? 55 : 99));
    }
}

// A kernel-hosted world answers the named-point queries from the carrier's
// model through its collision pose, so the AI entry walk reaches the model's
// UseGun point by name. No seat table carries the point here (E/G/S/H points
// never are seats), so the seat-scan fallback alone left the goal on the
// target's origin. The same kernel answers the last-match, userpoint-pivot and
// section-pivot queries from that model.
// [orig: Entity_GetBoneTransformAndOrientation @0x4B0C50 by name, reached from
//  the entry walk Entity_UpdateInfantryAI @0x4BB373..0x4BB849;
//  Entity_FindAttachBone @0x4B9580; Entity_ComputeWeaponFireTransform_0
//  @0x456980; Entity_ProcessSectionDamageTransition @0x43F496..0x43F501]
static void test_board_walk_reaches_a_kernel_named_point() {
	namespace fs = std::filesystem;
	const std::string root = std::string(test_paths_temp_dir()) + "/opennova_kernel_named_points";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root, ec);
	const std::vector<uint8_t> mount = test_io::read_file(
			std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/mount.3di");
	if (test_io::is_lfs_pointer(mount)) {
		// A checkout without the LFS fixtures (the net-linux job pulls only
		// fixtures/novaworld) carries the pointer, not the model.
		std::printf("SKIP-LEG: needs the LFS fixture fixtures/threedi/synth/mount.3di\n");
		return;
	}
	CHECK(!mount.empty() && test_io::write_file(root + "/NamedMount.3di", mount));
	static const char kItems[] =
			"begin \"Named mount\"\n id 100164\n type object\n graphic NamedMount\n hp 100\nend\n";
	def::DefItemsFile items{};
	CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
			sizeof(kItems) - 1, &items) == 0);
	{
		ResourceIndex index;
		CHECK(index.scan(root, std::string(), VfsMountMode::LooseOnly));
		assets::AssetStore store{&index};
		std::map<std::string, std::string> files;
		bms::File m{};
		m.items.push_back(item(/*type_id=*/164, 10 << 16, 20 << 16, 0));
		m.items[0].id = 21;
		m.organics.push_back(organic(20 << 16, 20 << 16, 0, /*team=*/1, /*yaw=*/90));
		m.organics[0].id = 31;
		ms::MissionKernel kernel;
		kernel.set_items_table(&items);
		kernel.set_assets(&store);
		kernel.open_document(std::move(m), "named", source_over(&files));
		ms::KernelBootOptions options;
		options.playable = false;
		options.wac = false;
		options.terrain = false;
		options.seat_specs = false; // no seat carries the point: only its name reaches it
		std::string error;
		CHECK(kernel.boot(options, error));
		w::World &world = kernel.world;
		const w::Entity *carrier = world.registry.by_net_id(21);
		const w::Entity *npc = world.registry.by_net_id(31);
		CHECK(carrier != nullptr && npc != nullptr && world.pose_provider == &kernel);
		if (carrier == nullptr || npc == nullptr || world.pose_provider == nullptr) {
			def::def_free_items(&items);
			return;
		}
		CHECK(carrier->seats.empty());
		// The four queries reach the model through the kernel.
		w::IPoseProvider &pose = *world.pose_provider;
		int32_t named[6] = {}, direct[6] = {};
		CHECK(pose.resolve_named_transform(world, carrier->handle, "USEGUN", named));
		CHECK(kernel.collision_pose.resolve_named_transform(world, carrier->handle, "Usegun", direct));
		CHECK(std::equal(named, named + 6, direct));
		CHECK(direct[0] != (10 << 16) || direct[1] != (20 << 16)); // off the origin
		CHECK(pose.last_named_userpoint(world, carrier->handle, "usegun") == 6);
		int32_t pivot[3] = {}, pivot_direct[3] = {};
		CHECK(pose.resolve_userpoint_pivot(world, carrier->handle, 6, pivot));
		CHECK(kernel.collision_pose.resolve_userpoint_pivot(world, carrier->handle, 6, pivot_direct));
		CHECK(std::equal(pivot, pivot + 3, pivot_direct));
		int32_t section[3] = {}, section_direct[3] = {};
		CHECK(pose.resolve_section_pivot(world, carrier->handle, 2, section));
		CHECK(kernel.collision_pose.resolve_section_pivot(world, carrier->handle, 2, section_direct));
		CHECK(std::equal(section, section + 3, section_direct));
		// The any-seat board order walks to the named point on the 1-unit ring.
		w::AiEntity *body = world.ai.for_handle(npc->handle);
		if (body == nullptr) body = world.ai.at(world.ai.attach(npc->handle));
		body->inf.active = true;
		body->slot.f[37] = 125;
		body->slot.f[38] = 21;
		int32_t entry_heading = 0;
		world.ai.infantry_board_think(*body, world, 125, entry_heading);
		CHECK(body->inf.move_target[0] == direct[0] && body->inf.move_target[1] == direct[1] &&
				body->inf.move_target[2] == direct[2]);
		CHECK(body->inf.arrival_radius == 0x10000);
	}
	def::def_free_items(&items);
	fs::remove_all(root, ec);
}

int main() {
    test_first_frame_tick_matches_on_host_and_joiner();
    test_joiner_installs_only_the_wac_terminator();
    test_initial_wac_binds_the_preopened_music_context();
    test_empty_wac_clock_and_baseline_gate();
	test_sound_profiles_parse_once_and_keep_a_pre_boot_override();
	test_vehicle_spawn_pose_is_captured_after_initial_wac();
	test_numbered_vars_reset_after_premission_before_initial_wac();
	test_initial_wac_waits_for_the_weather_owner_once();
	test_restart_cancel_keeps_the_personal_slot_zoom();
	test_board_walk_reaches_a_kernel_named_point();
	// The synthetic mission: two placed entities plus one (empty) BMS event,
	// and a mission-named WAC layer in the in-memory source.
	std::map<std::string, std::string> files;
	files["synth.wac"] = "if never() then set(v1,1) endif\n";

	ms::MissionKernel kernel;
	kernel.open_document(test_mission::two_entity_mission(), "synth", source_over(&files));

	ms::KernelBootOptions options; // playable, wac, collision, seat_specs on; game_type 0 (SP)
	std::string error;
	CHECK(kernel.boot(options, error));
	CHECK(error.empty());

	// Boot side effects, in the S9 order's observable residue: the promote,
	// the BMS event registration, the layered WAC install, the origin spawn
	// (no start marker in the synthetic mission), the baseline capture.
	CHECK(kernel.promo.spawned == 2);
	CHECK(kernel.promo.dropped == 0);
	CHECK(kernel.world.registry.by_net_id(21) != nullptr);
	CHECK(kernel.world.registry.by_net_id(31) != nullptr);
	CHECK(kernel.events.events().size() == 1);
	CHECK(kernel.wac_loaded);
	CHECK(kernel.wac.vm().loaded());
	CHECK(kernel.local.has_local_player());
	CHECK(kernel.local.player() != nullptr);
	CHECK(kernel.local.player_ai() != nullptr);
	CHECK(near_equal(kernel.local.player_position().x, 0.0f, 0.001f));
	CHECK(near_equal(kernel.local.player_position().y, 0.0f, 0.001f));
	CHECK(kernel.have_baseline);
	CHECK(!kernel.has_terrain()); // no terrain documents were supplied
	CHECK(kernel.text_source == ms::MissionTextSource::kNone);
	// The boot ORDER (ADR 0043 slice E9): the file source is present, no item
	// db (no items.def in the source), no terrain, a playable non-joiner --
	// the trace is the literal step sequence with those gates applied.
	{
		const std::vector<std::string> expected = {
				"ai_profiles", "mission_text", "load_mission", "infantry_anim",
				"script_catalogs", "wac", "spawn_local_player", "weapon_table", "ammo_table",
				"powerup_table", "organic_init", "premission"};
		CHECK(kernel.boot_trace == expected);
		if (kernel.boot_trace != expected)
			for (const std::string &s : kernel.boot_trace) std::printf("  trace: %s\n", s.c_str());
	}
	run_boot_trace_gates();

	// One no-net tick advances the authoritative logic clock.
	const uint32_t tick0 = kernel.world.logic_tick;
	tick_no_net(kernel);
	CHECK(kernel.world.logic_tick == tick0 + 1);
	tick_no_net(kernel);
	CHECK(kernel.world.logic_tick == tick0 + 2);

	// The camera shake: the counter decays ONCE per tick in the pre-tick pass
	// (never per frame), while every composed frame advances the IIR filters
	// again from the same PRNG word — two rendered frames between ticks
	// differ. Observing the view between composes reads the last composed
	// view and advances nothing, so two observations agree and leave the
	// filters where the frames left them
	// [orig: @ 0x4de590; the Camera_ComputeThirdPersonView calls @ 0x526781 / @ 0x5ca34d;
	//  the only other callers @ 0x5c9841 (the Inset scene) / @ 0x52b082].
	kernel.local.view.shake.counter = 10;
	tick_no_net(kernel);
	CHECK(kernel.local.view.shake.counter == 8);
	{
		const w::LocalPlayerViewFrame frame_a = kernel.local.present_view_frame();
		const w::LocalPlayerViewFrame frame_b = kernel.local.present_view_frame();
		CHECK(kernel.local.view.shake.counter == 8);
		CHECK(frame_a.camera.yaw_deg != frame_b.camera.yaw_deg ||
		      frame_a.camera.pitch_deg != frame_b.camera.pitch_deg ||
		      frame_a.camera.roll_deg != frame_b.camera.roll_deg);
		const w::CameraShakeState filters = kernel.local.view.shake;
		const w::LocalPlayerViewFrame seen_a = kernel.local.view_frame();
		const w::LocalPlayerViewFrame seen_b = kernel.local.view_frame();
		CHECK(seen_a.camera_pose_valid && seen_b.camera_pose_valid);
		CHECK(seen_a.camera.yaw_deg == seen_b.camera.yaw_deg &&
		      seen_a.camera.pitch_deg == seen_b.camera.pitch_deg &&
		      seen_a.camera.roll_deg == seen_b.camera.roll_deg);
		CHECK(seen_a.camera.yaw_deg == frame_b.camera.yaw_deg &&
		      seen_a.camera.pitch_deg == frame_b.camera.pitch_deg);
		CHECK(kernel.local.view.shake.yaw == filters.yaw &&
		      kernel.local.view.shake.pitch == filters.pitch &&
		      kernel.local.view.shake.roll == filters.roll);
	}
	tick_no_net(kernel);
	CHECK(kernel.local.view.shake.counter == 6);
	kernel.local.view.shake.counter = 0;

	// Teleport writes BOTH stores: the registry position and the AI 16.16
	// mirror, with the input-owned view seeded to the new facing.
	const w::EntityHandle player_h = kernel.local.player()->handle;
	kernel.local.teleport_local_player(w::Vec3{100.0f, 200.0f, 5.0f}, /*yaw_deg=*/90.0, /*pitch_deg=*/0.0);
	CHECK(near_equal(kernel.local.player_position().x, 100.0f, 0.001f));
	CHECK(near_equal(kernel.local.player_position().y, 200.0f, 0.001f));
	CHECK(near_equal(kernel.local.player_position().z, 5.0f, 0.001f));
	if (const w::AiEntity *body = kernel.local.player_ai()) {
		CHECK(body->pos[0] == 100 << 16);
		CHECK(body->pos[1] == 200 << 16);
		CHECK(body->pos[2] == 5 << 16);
		CHECK(kernel.local.input.look_heading == body->heading);
	}

	// set_entity_position / set_entity_health round-trip the same dual store.
	kernel.world.commands.set_entity_position(player_h, w::Vec3{50.0f, 60.0f, 2.0f});
	CHECK(near_equal(kernel.local.player_position().x, 50.0f, 0.001f));
	if (const w::AiEntity *body = kernel.local.player_ai()) CHECK(body->pos[1] == 60 << 16);
	kernel.world.commands.set_entity_health(player_h, 37);
	CHECK(kernel.local.player_health() == 37);
	CHECK(kernel.local.player()->alive);
	if (const w::AiEntity *body = kernel.local.player_ai()) CHECK(body->health == 37);
	kernel.world.commands.set_entity_health(player_h, 0);
	CHECK(kernel.local.player_health() == 0);
	CHECK(!kernel.local.player()->alive);
	kernel.world.commands.set_entity_health(player_h, 100);

	// The CanFire verdict on the spawned player: no weapon table was loaded
	// (no weapon.def in the source), so the local weapon is inactive and the
	// verdict is a hard no — the same gate the pre-tick stamps onto
	// inf.aimed_shot_available.
	CHECK(!kernel.local.weapon.active);
	CHECK(!kernel.local.local_player_can_fire());
	tick_no_net(kernel);
	if (const w::AiEntity *body = kernel.local.player_ai()) CHECK(!body->inf.aimed_shot_available);

	// The baseline restores the post-PreMission world.
	CHECK(kernel.restore_baseline());

	if (failures == 0) std::printf("mission_kernel: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
