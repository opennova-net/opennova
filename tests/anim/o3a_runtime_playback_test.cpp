// The engine plays a rebuilt clip set the way it plays the shipped one: take a
// retail `.adm`, write it out as `.o3a` and mint it again through
// `opennova-3di anim`, then load BOTH sets through the runtime's own loader
// (runtime/anim/skeletal_clips) over the paired model's bone table, as
// production does (runtime/world/entity_pose.cpp, the placer's skeletal
// rigs), and compare the pose it evaluates, clip by clip and tick by tick.
// `anim compare` reads the files; this reads what the game would draw from
// them.
// Gated on OPENNOVA_JO_ASSETS (docs/asset-gated-tests.md).
//
//   o3a_runtime_playback_test <opennova-3di> <scratch dir>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <base/resource_index/resource_index.h>
#include <formats/adm/adm.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/entity_pose.h>

#include "common/retail_paths.h"
#include "common/run_command.h"

using namespace opennova;

namespace {

using test_cmd::quoted;
using test_cmd::run;

int failures = 0;
void expect(bool ok, const std::string &what) {
	if (ok) return;
	std::fprintf(stderr, "FAIL: %s\n", what.c_str());
	++failures;
}

double rotation_degrees(const anim::Quat &a, const anim::Quat &b) {
	// Normalized: a pose quaternion is not unit length -- the slerp's linear
	// path leaves it a little short, as the original's does
	// [orig: Math_QuaternionSlerp @0x615e20, the linear weights @0x615ea6], and
	// |q| of 0.996 alone reads as seven degrees of rotation.
	const double la = std::sqrt(static_cast<double>(a.w) * a.w + static_cast<double>(a.x) * a.x +
			static_cast<double>(a.y) * a.y + static_cast<double>(a.z) * a.z);
	const double lb = std::sqrt(static_cast<double>(b.w) * b.w + static_cast<double>(b.x) * b.x +
			static_cast<double>(b.y) * b.y + static_cast<double>(b.z) * b.z);
	if (la <= 0.0 || lb <= 0.0) return la == lb ? 0.0 : 180.0;
	double dot = (static_cast<double>(a.w) * b.w + static_cast<double>(a.x) * b.x +
						 static_cast<double>(a.y) * b.y + static_cast<double>(a.z) * b.z) /
			(la * lb);
	dot = std::fabs(dot);
	if (dot > 1.0) dot = 1.0;
	return 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846;
}

double origin_gap(const anim::Vec3 &a, const anim::Vec3 &b) {
	const double dx = static_cast<double>(a.x) - b.x;
	const double dy = static_cast<double>(a.y) - b.y;
	const double dz = static_cast<double>(a.z) - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::fprintf(stderr, "usage: o3a_runtime_playback_test <opennova-3di> <scratch dir>\n");
		return 2;
	}
	const std::string cli = quoted(argv[1]);
	// The body rig's table, and the two first-person sets the weapon's own
	// parts and the skinned arms share, each with the model whose bone table
	// the runtime poses it over: a clip pairs with a rig's parts by index, so
	// one `.bad` drives the gun's parts and the arms' bones alike
	// [orig: BoneAnim_BuildWorldMatrices @ 0x40c400].
	const std::vector<std::pair<std::string, std::string>> sets = {
			{"US01.ADM", "US01"}, {"mp5_1st.adm", "Mp5b_1st"}, {"357_1st.adm", "357_1st"}};
	int ran = 0;
	for (const auto &[table, model_name] : sets) {
		// One directory per set, named after its stem: the rebuilt table keeps
		// its own name inside it.
		const std::string stem = table.substr(0, table.find('.'));
		const std::string dir = std::string(argv[2]) + "/o3a-playback-" + stem;
		const std::string source = retail::asset_file(table.c_str());
		const std::string model_file = retail::asset_file((model_name + ".3di").c_str());
		if (source.empty() || model_file.empty()) {
			std::printf("SKIP-LEG: needs %s and %s.3di in OPENNOVA_JO_ASSETS\n", table.c_str(),
					model_name.c_str());
			continue;
		}
		++ran;
#ifdef _WIN32
		run("if not exist " + quoted(dir) + " mkdir " + quoted(dir));
#else
		run("mkdir -p " + quoted(dir));
#endif
		const std::string scene = dir + "/set.o3a";
		const std::string rebuilt = dir + "/" + table;
		if (run(cli + " anim scene " + quoted(source) + " -o " + quoted(scene)) != 0 ||
				run(cli + " anim build " + quoted(scene) + " -o " + quoted(rebuilt)) != 0) {
			expect(false, table + " did not rebuild");
			continue;
		}

		// Two stores: the shipped set beside its own clips, and the rebuilt one
		// beside the clips `anim build` wrote next to it.
		ResourceIndex shipped_index;
		ResourceIndex rebuilt_index;
		expect(shipped_index.scan(retail::assets()), "the retail asset root scans");
		expect(rebuilt_index.scan(dir), table + "'s rebuilt directory scans");
		assets::AssetStore shipped_assets{&shipped_index};
		assets::AssetStore rebuilt_assets{&rebuilt_index};

		// The rig is the MODEL's bone table (its parents and pivots); the
		// clips contribute rotations and translations only.
		const auto model = shipped_assets.model(model_name);
		std::vector<anim::Vec3> origins;
		std::vector<int> parents;
		expect(model != nullptr && world::model_bone_table(*model, origins, parents),
				model_name + " carries a bone table");
		if (origins.empty()) continue;
		anim::SkeletalClips stock;
		anim::SkeletalClips minted;
		expect(stock.load_from_adm(&shipped_assets, table, origins, parents),
				table + " loads as shipped");
		expect(minted.load_from_adm(&rebuilt_assets, table, origins, parents),
				table + " loads as rebuilt");
		if (!stock.loaded() || !minted.loaded()) continue;
		expect(stock.bone_count() == minted.bone_count(),
				table + " keeps its " + std::to_string(stock.bone_count()) + " bones");
		expect(stock.fk_valid() && minted.fk_valid(), table + "'s rigs are FK-safe");

		// Every key the table authors, every variant, sampled across each
		// clip's length. The tolerances are the comparator's: a twentieth of a
		// degree and a millimetre.
		std::vector<std::string> keys;
		{
			opennova::adm::AdmFile parsed{};
			expect(opennova::adm::adm_parse(rebuilt.c_str(), &parsed) == 0, table + " parses");
			for (size_t i = 0; i < parsed.count; ++i) keys.push_back(parsed.entries[i].key);
			opennova::adm::adm_free(&parsed);
		}
		std::vector<anim::PoseBone> a;
		std::vector<anim::PoseBone> b;
		double worst_rotation = 0.0;
		double worst_origin = 0.0;
		std::string worst_at;
		int checked = 0;
		for (const std::string &key : keys) {
			if (!stock.has_clip(key)) continue;
			expect(minted.has_clip(key), table + " keeps " + key);
			if (!minted.has_clip(key)) continue;
			const int variants = stock.clip_variant_count(key);
			expect(minted.clip_variant_count(key) == variants,
					key + " keeps its " + std::to_string(variants) + " variants");
			for (int variant = 0; variant < variants; ++variant) {
				const float length = stock.clip_length(key, variant);
				expect(std::fabs(length - minted.clip_length(key, variant)) <= 1e-4f,
						key + " keeps its length");
				expect(stock.clip_fps(key, variant) == minted.clip_fps(key, variant),
						key + " keeps its rate");
				for (int step = 0; step <= 8; ++step) {
					const double at = static_cast<double>(length) * step / 8.0;
					stock.eval_pose(key, at, variant, a);
					minted.eval_pose(key, at, variant, b);
					if (a.size() != b.size()) {
						expect(false, key + " poses the same bone count");
						continue;
					}
					++checked;
					for (size_t i = 0; i < a.size(); ++i) {
						const double rot = rotation_degrees(a[i].rotation, b[i].rotation);
						const double gap = origin_gap(a[i].origin, b[i].origin);
						if (rot > worst_rotation) {
							worst_rotation = rot;
							worst_at = key + " bone " + std::to_string(i) + " at " +
									std::to_string(at) + "s";
						}
						if (gap > worst_origin) worst_origin = gap;
					}
				}
			}
		}
		expect(checked > 0, table + " authors keys its rig carries");
		expect(worst_rotation <= 0.05, table + ": every pose turns the same (worst " +
						std::to_string(worst_rotation) + " degrees at " + worst_at + ")");
		expect(worst_origin <= 1e-3, table + ": every pose sits the same (worst " +
						std::to_string(worst_origin) + " m)");
		std::printf("%s: %zu bones, %zu keys, %d poses, worst %g degrees and %g m\n",
				table.c_str(), stock.bone_count(), keys.size(), checked, worst_rotation,
				worst_origin);
	}
	if (ran == 0)
		return retail::skip("OPENNOVA_JO_ASSETS with US01.ADM, mp5_1st.adm and 357_1st.adm and "
							"their models");
	if (failures == 0) std::printf("o3a_runtime_playback_test: ok (%d clip sets)\n", ran);
	return failures == 0 ? 0 : 1;
}
