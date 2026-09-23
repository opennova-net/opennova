// Shared native assets: identity, source replacement, negative-cache refresh,
// and snapshot lifetime through the same interface used by the mission and Godot.
#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/anim/skeletal_clips.h>

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <stdexcept>

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

	assets::AssetStore empty;
	TEST_EXPECT(!empty.model("thing"));
	TEST_EXPECT(!empty.animation_map("soldier"));
	TEST_EXPECT(!empty.bone_animation("idle"));
	TEST_EXPECT(!empty.skeletal_rig("soldier"));
	return 0;
}
