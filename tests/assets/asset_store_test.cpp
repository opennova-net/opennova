// Shared native assets: identity, source replacement, negative-cache refresh,
// and snapshot lifetime through the same interface used by the mission and Godot.
#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/renderer/object_lod.h>
#include <runtime/world/collision_model.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/model_geometry.h>
#include <runtime/world/occlusion.h>

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>
#include "../common/title_fixture.h"

namespace fs = std::filesystem;
using namespace opennova;

struct TestRoot {
	fs::path path;
	TestRoot() {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = fs::path(test_paths_temp_dir()) / ("opennova-shared-assets-" + std::to_string(stamp));
		if (!fs::create_directory(path)) throw std::runtime_error("Cannot create test root");
	}
	~TestRoot() {
		// The harness owns only the unique directory it created.
		const auto parent = fs::weakly_canonical(fs::path(test_paths_temp_dir()));
		if (fs::weakly_canonical(path).parent_path() == parent) {
			std::error_code error;
			fs::remove_all(path, error);
		}
	}
};

// The mounted load refuses what the game's model load refuses: a chunk file whose version
// word is below 0x103, and a GHDR mesh type other than 1 or 2; the standalone parse still
// reads both. [orig: ThreediGp_LoadFromFile @0x5B5780 - @0x5B5819..0x5B5823,
//  @0x5B5911..0x5B5922]
static int the_mount_refuses_what_the_game_load_refuses(const fs::path &models) {
	std::vector<uint8_t> bytes;
	{
		std::ifstream in(models / "crate.3di", std::ios::binary);
		bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	TEST_EXPECT(bytes.size() > 8);
	if (bytes.size() <= 8) return 1;
	size_t ghdr = std::string(bytes.begin(), bytes.end()).find("GHDR");
	TEST_EXPECT(ghdr != std::string::npos && ghdr + 8 + 20 <= bytes.size());
	if (ghdr == std::string::npos || ghdr + 8 + 20 > bytes.size()) return 1;
	const size_t mesh_type = ghdr + 8 + 16; // the GHDR data's mesh-type dword
	struct Case { const char *name; uint32_t version; int32_t mesh; bool loads; };
	const Case cases[] = {
		{"stock", 0x103, -1, true},
		{"old", 0x102, -1, false},
		{"newer", 0x104, -1, true},
		{"wide", 0x00010102, -1, false}, // the compare is the low word's
		{"mesh3", 0x103, 3, false},
		{"mesh0", 0x103, 0, false},
	};
	TestRoot root;
	for (const Case &c : cases) {
		std::vector<uint8_t> patched = bytes;
		for (int i = 0; i < 4; ++i) patched[4 + i] = uint8_t(c.version >> (8 * i));
		if (c.mesh >= 0)
			for (int i = 0; i < 4; ++i) patched[mesh_type + i] = uint8_t(uint32_t(c.mesh) >> (8 * i));
		std::ofstream out(root.path / (std::string(c.name) + ".3di"), std::ios::binary);
		out.write(reinterpret_cast<const char *>(patched.data()), std::streamsize(patched.size()));
		out.close();
		const assets::Model standalone = assets::parse_model(patched.data(), patched.size());
		TEST_EXPECT(standalone != nullptr);
		if (standalone && c.mesh >= 0) TEST_EXPECT(int32_t(standalone->header.mesh_type) == c.mesh);
	}
	ResourceIndex index;
	TEST_EXPECT(index.scan(root.path.string(), {}, VfsMountMode::LooseOnly));
	assets::AssetStore store{&index};
	for (const Case &c : cases) TEST_EXPECT((store.model(c.name) != nullptr) == c.loads);
	return 0;
}

// What the simulation reads of a model, compared between a Full store's and a Simulation store's
// load of one file: the header, the LODs' RMDL words, strips, render objects and part animations,
// the user points, the collision and occlusion counts, CTRL and MTRX, and the derivations the
// mission load and the world take from a model.
static int expect_same_simulation_view(
		const threedi::Threedi3di3 &full, const threedi::Threedi3di3 &sim) {
	TEST_EXPECT(sim.version == full.version);
	TEST_EXPECT(sim.header.has_header == full.header.has_header);
	TEST_EXPECT(std::strcmp(sim.header.name, full.header.name) == 0);
	TEST_EXPECT(sim.header.mesh_type == full.header.mesh_type);
	TEST_EXPECT(sim.header.lod_count_decl == full.header.lod_count_decl);
	TEST_EXPECT(sim.header.max_radius_fp16 == full.header.max_radius_fp16);
	TEST_EXPECT(sim.lod_count == full.lod_count);
	for (size_t i = 0; i < full.lod_count && i < sim.lod_count; ++i) {
		const threedi::ThreediLod &a = full.lods[i];
		const threedi::ThreediLod &b = sim.lods[i];
		TEST_EXPECT(std::memcmp(a.model_type, b.model_type, sizeof(a.model_type)) == 0);
		TEST_EXPECT(a.lod_threshold == b.lod_threshold);
		TEST_EXPECT(a.rmdl_render_object_count == b.rmdl_render_object_count);
		TEST_EXPECT(
				a.strip_count == b.strip_count && a.render_object_count == b.render_object_count);
		TEST_EXPECT(a.part_animation_count == b.part_animation_count);
		if (a.strip_count == b.strip_count && a.strip_count > 0)
			TEST_EXPECT(std::memcmp(a.strips, b.strips, a.strip_count * sizeof(*a.strips)) == 0);
		if (a.render_object_count == b.render_object_count && a.render_object_count > 0)
			TEST_EXPECT(std::memcmp(a.render_objects, b.render_objects,
								a.render_object_count * sizeof(*a.render_objects)) == 0);
	}
	TEST_EXPECT(sim.user_point_count == full.user_point_count);
	if (sim.user_point_count == full.user_point_count && full.user_point_count > 0)
		TEST_EXPECT(std::memcmp(sim.user_points, full.user_points,
							full.user_point_count * sizeof(*full.user_points)) == 0);
	TEST_EXPECT((sim.collision == nullptr) == (full.collision == nullptr));
	if (sim.collision != nullptr && full.collision != nullptr) {
		const threedi::ThreediCollisionModel &a = *full.collision;
		const threedi::ThreediCollisionModel &b = *sim.collision;
		TEST_EXPECT(a.plane_count == b.plane_count && a.volume_count == b.volume_count);
		TEST_EXPECT(a.vertex_count == b.vertex_count && a.normal_count == b.normal_count);
		TEST_EXPECT(a.face_count == b.face_count && a.object_count == b.object_count);
		TEST_EXPECT(a.translation_count == b.translation_count);
		TEST_EXPECT(a.model_data.num_objects == b.model_data.num_objects);
		TEST_EXPECT(std::memcmp(a.model_data.bbox_fp16, b.model_data.bbox_fp16,
							sizeof(a.model_data.bbox_fp16)) == 0);
	}
	TEST_EXPECT(sim.occlusion_vertex_count == full.occlusion_vertex_count);
	TEST_EXPECT(sim.occlusion_face_count == full.occlusion_face_count);
	TEST_EXPECT(sim.occlusion_object_count == full.occlusion_object_count);
	TEST_EXPECT(sim.occlusion_plane_count == full.occlusion_plane_count);
	TEST_EXPECT(sim.part_animation_count == full.part_animation_count);
	TEST_EXPECT(sim.ctrl.count == full.ctrl.count && sim.mtrx.count == full.mtrx.count);

	TEST_EXPECT(world::model_bound_radius_q16_from_3di(sim) ==
			world::model_bound_radius_q16_from_3di(full));
	TEST_EXPECT(world::model_is_skinned(sim, 0) == world::model_is_skinned(full, 0));
	TEST_EXPECT(world::model_has_collision(sim) == world::model_has_collision(full));
	TEST_EXPECT(
			renderer::model_lod_thresholds_q16(sim) == renderer::model_lod_thresholds_q16(full));
	std::vector<anim::Vec3> full_origins, sim_origins;
	std::vector<int> full_parents, sim_parents;
	TEST_EXPECT(world::model_bone_table(sim, sim_origins, sim_parents) ==
			world::model_bone_table(full, full_origins, full_parents));
	TEST_EXPECT(sim_parents == full_parents && sim_origins.size() == full_origins.size());
	for (size_t i = 0; i < full_origins.size() && i < sim_origins.size(); ++i)
		TEST_EXPECT(sim_origins[i].x == full_origins[i].x &&
				sim_origins[i].y == full_origins[i].y && sim_origins[i].z == full_origins[i].z);
	float full_anchor[3] = {}, sim_anchor[3] = {};
	threedi::threedi_3di3_ground_anchor(&full, full_anchor);
	threedi::threedi_3di3_ground_anchor(&sim, sim_anchor);
	TEST_EXPECT(std::memcmp(full_anchor, sim_anchor, sizeof(full_anchor)) == 0);
	world::CollisionModel full_collision, sim_collision;
	TEST_EXPECT(world::collision_model_from_3di(sim.collision, sim_collision, true) ==
			world::collision_model_from_3di(full.collision, full_collision, true));
	TEST_EXPECT(sim_collision.vertices.size() == full_collision.vertices.size());
	TEST_EXPECT(sim_collision.face_vertices.size() == full_collision.face_vertices.size());
	world::OcclusionModel full_occlusion, sim_occlusion;
	TEST_EXPECT(world::occlusion_model_from_3di(sim, sim_occlusion) ==
			world::occlusion_model_from_3di(full, full_occlusion));
	TEST_EXPECT(sim_occlusion.vertices.size() == full_occlusion.vertices.size());
	return 0;
}

// opennova-serve's store (assets::ModelData::Simulation) frees what only a renderer reads, each
// LOD's vertex and index buffers, the materials and the lights, and keeps everything else as a
// Full store's model of the same file holds it; the game's store keeps it all. Every synthetic
// model, the refused ones refused by both.
static int a_simulation_store_keeps_what_the_simulation_reads(const fs::path &models) {
	ResourceIndex index;
	TEST_EXPECT(index.scan(models.string(), {}, VfsMountMode::LooseOnly));
	assets::AssetStore full{ &index };
	assets::AssetStore sim{ &index, assets::ModelData::Simulation };
	TEST_EXPECT(full.model_data() == assets::ModelData::Full);
	TEST_EXPECT(sim.model_data() == assets::ModelData::Simulation);
	size_t compared = 0, full_vertices = 0, full_indices = 0, full_materials = 0;
	for (const fs::directory_entry &entry : fs::directory_iterator(models)) {
		if (entry.path().extension() != ".3di")
			continue;
		const std::string name = entry.path().stem().string();
		const assets::Model a = full.model(name);
		const assets::Model b = sim.model(name);
		TEST_EXPECT((a == nullptr) == (b == nullptr));
		if (a == nullptr || b == nullptr)
			continue;
		++compared;
		for (size_t i = 0; i < a->lod_count; ++i) {
			full_vertices += a->lods[i].vertices.count;
			full_indices += a->lods[i].indices.count;
		}
		full_materials += a->material_count;
		for (size_t i = 0; i < b->lod_count; ++i) {
			TEST_EXPECT(b->lods[i].vertices.items == nullptr && b->lods[i].vertices.count == 0);
			TEST_EXPECT(b->lods[i].indices.indices == nullptr && b->lods[i].indices.count == 0);
		}
		TEST_EXPECT(b->materials == nullptr && b->material_count == 0);
		TEST_EXPECT(b->lights == nullptr && b->light_count == 0);
		if (expect_same_simulation_view(*a, *b) != 0)
			return 1;
	}
	TEST_EXPECT(compared >= 10);
	TEST_EXPECT(full_vertices > 0 && full_indices > 0 && full_materials > 0);
	return 0;
}

int main() {
	const fs::path repo = test_paths_repo_root(__FILE__);
	const fs::path models = repo / "fixtures/threedi/synth";
	const fs::path animations = repo / "fixtures/anim";
	TestRoot first;
	TestRoot second;
	fs::copy_file(models / "bird.3di", first.path / "thing.3di");
	fs::copy_file(models / "house.3di", second.path / "thing.3di");
	for (const char *name : {"idle.bad", "walk.bad", "soldier.adm"})
		fs::copy_file(animations / name, first.path / name);
	// The minted idle padded with zeros to the game's clip limit and one byte past it: the
	// load takes a file of exactly bad::kBadLoadMaxBytes and refuses a bigger one
	// [orig: BoneFile_Load @0x40FFF0 - `cmp edi, 7A120h; jbe` @0x41005A..0x410060].
	for (const auto &[name, size] : {std::pair<const char *, size_t>{"edge.bad", bad::kBadLoadMaxBytes},
				 std::pair<const char *, size_t>{"huge.bad", bad::kBadLoadMaxBytes + 1}}) {
		std::ifstream in(animations / "idle.bad", std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		bytes.resize(size, 0);
		std::ofstream out(first.path / name, std::ios::binary);
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	}
	{
		std::ofstream map(first.path / "variants.adm", std::ios::binary);
		map << "anim_reset \"idle\"\r\n"
			   "anim_idle \"walk\" \"walk\" \"idle\" \"missing\"\r\n";
	}

	assets::Model retained_model;
	assets::BoneAnimation retained_clip;
	assets::SkeletalRig retained_rig;
	{
		ResourceIndex index;
		TEST_EXPECT(index.scan(first.path.string(), {}, VfsMountMode::LooseOnly));
		assets::AssetStore store{&index};
		const auto model = store.model("thing");
		TEST_EXPECT(model && model->collision);
		TEST_EXPECT(store.model("THING.3DI") == model);
		TEST_EXPECT(store.model("models\\Thing.ext") == model);
		const auto map = store.animation_map("soldier");
		TEST_EXPECT(map && store.animation_map("SOLDIER.ADM") == map);
		const auto clip = store.bone_animation("idle");
		TEST_EXPECT(clip && store.bone_animation("IDLE.BAD") == clip);
		// A .bad load opens the name's stem plus .bad (anim::bad_file_name).
		TEST_EXPECT(store.bone_animation("idle.txt") == clip && store.bone_animation("clips/Idle.anim") == clip);
		// The game's clip limit: a file of the limit loads, one byte more loads nothing.
		const auto edge = store.bone_animation("edge");
		TEST_EXPECT(edge && edge->frame_count == clip->frame_count);
		TEST_EXPECT(!store.bone_animation("huge"));
		const auto rig = store.skeletal_rig("soldier");
		TEST_EXPECT(rig && rig->bone_count() > 0);
		TEST_EXPECT(store.skeletal_rig("SOLDIER.ADM") == rig);
		std::vector<anim::Vec3> origins(rig->bone_count());
		const auto model_rig = store.skeletal_rig("soldier", origins, rig->parents());
		TEST_EXPECT(model_rig && model_rig != rig);
		TEST_EXPECT(store.skeletal_rig("SOLDIER.ADM", origins, rig->parents()) == model_rig);
		origins[0].x = 2.0f;
		const auto other_model_rig = store.skeletal_rig("soldier", origins, rig->parents());
		TEST_EXPECT(other_model_rig && other_model_rig != model_rig);
		auto kernel = std::make_unique<mission::MissionKernel>();
		test_titles::jo_world(kernel->world);
		kernel->set_assets(&store);
		TEST_EXPECT(kernel->assets().model("thing") == model);
		TEST_EXPECT(kernel->assets().skeletal_rig("soldier") == rig);

		const auto variants = store.skeletal_rig("variants");
		TEST_EXPECT(variants);
		TEST_EXPECT(variants->clips().size() == 4); // reset + 3 live variants
		TEST_EXPECT(variants->find_clip_variant("anim_idle", 0)->clip.frame_count ==
					variants->find_clip_variant("anim_idle", 1)->clip.frame_count);
		TEST_EXPECT(variants->find_clip_variant("anim_idle", 3) ==
					variants->find_clip_variant("anim_idle", 0));

		// Adding files requires a rescan of the mounted directory. The new
		// source revision also discards negative model and rig cache entries.
		TEST_EXPECT(!store.model("late"));
		TEST_EXPECT(!store.skeletal_rig("late"));
		fs::copy_file(models / "bird.3di", first.path / "late.3di");
		fs::copy_file(animations / "soldier.adm", first.path / "late.adm");
		TEST_EXPECT(!store.model("late"));
		TEST_EXPECT(!store.skeletal_rig("late"));
		TEST_EXPECT(index.scan(first.path.string(), {}, VfsMountMode::LooseOnly));
		TEST_EXPECT(store.model("late"));
		TEST_EXPECT(store.skeletal_rig("late"));
		TEST_EXPECT(store.model("thing") != model);

		// Explicit global refresh must reach the mission's retained store,
		// without a presentation-side accessor having to trigger it first.
		const auto before_refresh = kernel->assets().model("thing");
		fs::copy_file(models / "house.3di", first.path / "thing.3di",
					  fs::copy_options::overwrite_existing);
		TEST_EXPECT(kernel->assets().model("thing") == before_refresh);
		bump_cache_epoch();
		TEST_EXPECT(kernel->assets().model("thing") != before_refresh);
		TEST_EXPECT(std::string(kernel->assets().model("thing")->header.name) !=
					before_refresh->header.name);
		TEST_EXPECT(kernel->assets().model("thing") == store.model("thing"));

		const std::vector<std::pair<std::string, std::string>> named = {{"Idle", "idle"}};
		const auto explicit_rig = store.skeletal_rig_from_files("idle", named);
		TEST_EXPECT(explicit_rig);
		TEST_EXPECT(store.skeletal_rig_from_files("IDLE.BAD", named) == explicit_rig);
		TEST_EXPECT(store.skeletal_rig_from_files("idle.txt", {{"Idle", "IDLE.anim"}}) == explicit_rig);
		TEST_EXPECT(explicit_rig->find_clip("idle")->key == "Idle");
		const auto renamed_rig = store.skeletal_rig_from_files("idle", {{"IDLE", "idle"}});
		TEST_EXPECT(renamed_rig && renamed_rig != explicit_rig);
		TEST_EXPECT(renamed_rig->find_clip("idle")->key == "IDLE");

		// Same ResourceIndex address, different mounted content. Existing
		// consumers remain safe snapshots while new lookups use the new mount.
		retained_model = model;
		retained_clip = clip;
		retained_rig = rig;
		TEST_EXPECT(index.scan(second.path.string(), {}, VfsMountMode::LooseOnly));
		const auto replacement = store.model("thing");
		TEST_EXPECT(replacement && replacement != model);
		TEST_EXPECT(std::string(replacement->header.name) != model->header.name);
		TEST_EXPECT(!store.animation_map("soldier"));
		TEST_EXPECT(!store.bone_animation("idle"));
		TEST_EXPECT(!store.skeletal_rig("soldier"));
		ResourceIndex moved_source;
		TEST_EXPECT(moved_source.scan(first.path.string(), {}, VfsMountMode::LooseOnly));
		index = std::move(moved_source);
		TEST_EXPECT(store.model("thing") != replacement);
		TEST_EXPECT(store.skeletal_rig("soldier"));
		TEST_EXPECT(!index.scan((second.path / "absent").string(), {}, VfsMountMode::LooseOnly));
		TEST_EXPECT(!store.model("thing"));
		TEST_EXPECT(retained_model->collision != nullptr);
		TEST_EXPECT(retained_clip->frame_count > 0);
	}
	TEST_EXPECT(retained_model->collision != nullptr);
	TEST_EXPECT(retained_clip->frame_count > 0);
	std::vector<anim::PoseBone> pose;
	retained_rig->eval_pose("anim_idle", 0.1, 0, pose);
	TEST_EXPECT(pose.size() == retained_rig->bone_count());

	if (the_mount_refuses_what_the_game_load_refuses(models) != 0) return 1;
	if (a_simulation_store_keeps_what_the_simulation_reads(models) != 0)
		return 1;

	assets::AssetStore empty;
	TEST_EXPECT(!empty.model("thing"));
	TEST_EXPECT(!empty.animation_map("soldier"));
	TEST_EXPECT(!empty.bone_animation("idle"));
	TEST_EXPECT(!empty.skeletal_rig("soldier"));
	return 0;
}
