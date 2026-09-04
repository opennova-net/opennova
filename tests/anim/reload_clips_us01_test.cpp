// The remote-player reload asset chain on the retail data: the wire type
// 0x14B9 resolves to the US01 visual item, its graphic/anim_def name the US01
// rig, the infantry anim-key table maps 65/66 to reload/reload2, the US01 clip
// set carries both reload clips with more than one frame, each clip actually
// moves the pose, and the weapon-channel splice consumes the reload key — the
// masked forearm leaves the locomotion pose while the reload plays.
// Gated on OPENNOVA_JO_ASSETS (an extracted JO tree carrying items.def, US01.3di
// and US01.adm).
#include "common/retail_paths.h"

#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/aim_overlay.h>
#include <runtime/simassets/adm_skeletal_clips.h>
#include <runtime/simassets/collision_resolve.h>
#include <runtime/simassets/sim_pose_provider.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/world/infantry.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::def;

namespace {

using namespace opennova;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kPlayerRuntimeType = 0x14B9;
constexpr int kMaskBoneForearm = 10; // BN10, the right forearm

bool iequals(const char *a, const char *b) {
	for (;; ++a, ++b) {
		const int ca = std::tolower(static_cast<unsigned char>(*a));
		const int cb = std::tolower(static_cast<unsigned char>(*b));
		if (ca != cb) return false;
		if (ca == '\0') return true;
	}
}

// The rotation between two bone samples in degrees.
double rot_delta_deg(const anim::Quat &a, const anim::Quat &b) {
	double dot = double(a.w) * b.w + double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
	if (dot < 0.0) dot = -dot;
	if (dot > 1.0) dot = 1.0;
	return 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846;
}

double max_rot_delta(const std::vector<anim::PoseBone> &a, const std::vector<anim::PoseBone> &b,
		int *r_bone = nullptr) {
	double best = 0.0;
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i) {
		const double d = rot_delta_deg(a[i].rotation, b[i].rotation);
		if (d > best) {
			best = d;
			if (r_bone != nullptr) *r_bone = static_cast<int>(i);
		}
	}
	return best;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
			"OPENNOVA_JO_ASSETS (an extracted JO tree carrying items.def and US01)");
	ResourceIndex index;
	if (!index.scan(assets) && !index.scan(assets, std::string(), VfsMountMode::LooseOnly))
		return retail::skip("a mountable OPENNOVA_JO_ASSETS tree");
	std::vector<uint8_t> items_bytes;
	if (!index.read_file("items.def", items_bytes)) return retail::skip("items.def under OPENNOVA_JO_ASSETS");
	DefItemsFile items{};
	if (def_parse_items_memory(items_bytes.data(), items_bytes.size(), &items) != 0)
		return retail::skip("a parseable items.def");

	// --- 65/66 are the reload keys.
	expect(std::strcmp(world::kInfantryAnimNames[world::anim_state::kReload], "reload") == 0,
			"infantry anim key 65 is reload");
	expect(std::strcmp(world::kInfantryAnimNames[world::anim_state::kReload2], "reload2") == 0,
			"infantry anim key 66 is reload2");

	// --- 0x14B9 -> the US01 visual item.
	const int visual = simassets::visual_item_id_for_runtime_type(kPlayerRuntimeType, items);
	const DefItemDef *def = simassets::find_item_def(items, visual);
	if (!expect(def != nullptr, "wire type 0x14B9 resolves to a visual item")) {
		def_free_items(&items);
		return 1;
	}
	std::printf("reload_clips: 0x14B9 -> item %d graphic=%s anim_def=%s\n", visual, def->graphic,
			def->anim_def);
	expect(iequals(def->graphic, "US01"), "the wire remote player's graphic is US01");
	expect(std::strlen(def->anim_def) >= 4 && iequals(std::string(def->anim_def, 4).c_str(), "US01"),
			"the wire remote player's anim_def is US01");

	// --- The rig: US01.3di bone table + US01.adm clips.
	simassets::SimModelCache models;
	models.set_index(&index);
	const Threedi3di3 *model = models.model_for(def->graphic);
	if (!expect(model != nullptr, "US01.3di loads")) {
		def_free_items(&items);
		return 1;
	}
	std::vector<anim::Vec3> origins;
	std::vector<int> parents;
	expect(simassets::model_bone_table(*model, origins, parents), "US01 carries a bone table");
	std::string adm(def->anim_def);
	if (adm.size() < 4 || !iequals(adm.substr(adm.size() - 4).c_str(), ".adm")) adm += ".adm";
	simassets::AdmSkeletalClips clips;
	if (!expect(clips.load_from_adm(&index, adm, origins, parents), "US01.adm loads over the bone table")) {
		def_free_items(&items);
		return 1;
	}
	std::printf("reload_clips: %s bones=%zu fk_valid=%d\n", clips.adm_name().c_str(),
			clips.bone_count(), int(clips.fk_valid()));
	def_free_items(&items);

	static const char *const kReloadKeys[] = {"anim_reload", "anim_reload2"};
	std::vector<anim::PoseBone> idle, start, mid;
	clips.eval_pose("anim_idle", 0.0, 0, idle);
	for (const char *key : kReloadKeys) {
		char msg[160];
		std::snprintf(msg, sizeof(msg), "US01 clip set carries %s", key);
		if (!expect(clips.has_clip(key), msg)) continue;
		const simassets::AdmSkeletalClips::LoadedClip *clip = clips.find_clip(key);
		const uint32_t frames = clip != nullptr ? clip->clip.frame_count : 0;
		const float fps = clips.clip_fps(key, 0);
		std::printf("reload_clips: %-13s frames=%u fps=%.2f\n", key, frames, fps);
		std::snprintf(msg, sizeof(msg), "%s has more than one frame", key);
		expect(frames > 1, msg);
		const double mid_seconds = fps > 0.0f ? (frames / 2.0) / fps : 0.0;
		clips.eval_pose(key, 0.0, 0, start);
		clips.eval_pose(key, mid_seconds, 0, mid);
		std::snprintf(msg, sizeof(msg), "%s differs from the idle pose at mid-clip", key);
		expect(max_rot_delta(idle, mid) > 0.01, msg);
		std::snprintf(msg, sizeof(msg), "%s actually animates over its length", key);
		expect(max_rot_delta(start, mid) > 0.01, msg);

		// The weapon-channel splice consumes the reload key: the masked bones
		// leave the locomotion pose, the forearm most of all.
		// (through the composed-pose seam the presenters use, identity overlay)
		std::vector<anim::PoseBone> spliced;
		const anim::Quat identity[anim::kOverlayClassCount] = {};
		clips.eval_composed_pose("anim_idle", 0.0, false, std::string(), 0.0, 0.0f, identity,
				key, mid_seconds, spliced);
		int moved_bone = -1;
		const double moved = max_rot_delta(spliced, idle, &moved_bone);
		const double forearm = spliced.size() > static_cast<size_t>(kMaskBoneForearm)
				? rot_delta_deg(spliced[kMaskBoneForearm].rotation, idle[kMaskBoneForearm].rotation)
				: 0.0;
		std::printf("reload_clips: splice %-13s max delta %.1f deg (bone %d), forearm BN%02d %.1f deg\n",
				key, moved, moved_bone, kMaskBoneForearm, forearm);
		std::snprintf(msg, sizeof(msg), "the splice consumes %s", key);
		expect(moved > 0.01, msg);
		std::snprintf(msg, sizeof(msg), "the right forearm leaves the locomotion pose under %s", key);
		expect(forearm > 5.0, msg);
	}

	if (failures == 0) std::printf("reload_clips_us01: OK\n");
	return failures == 0 ? 0 : 1;
}
