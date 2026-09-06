// The models authored in Godot: godot/authoring/<name>/ exported into assets/<name>.3di
// by the opennova_model addon (godot/tests/model_export_guard_test.gd byte-guards the
// export). This guard checks what the ENGINE and RETAIL need from each shipped file rather
// than its bytes: the header names the file, the collision block is runtime-safe, every
// shader tag resolves in the renderer's descriptor table AND is served by an authored .fx
// in the set (a missing effect makes retail fall back to fixed-function silently), every
// texture the materials name is an allowlisted asset, and the VERT flags carry the
// tangent format iff a material's tag reads the TANGENT semantic (the witnessed writer
// rule, docs/threedi/3di-gp-format-re.md).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_paths.h"
#include <formats/bad/bad.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/anim_sample.h>
#include <runtime/renderer/material_descriptor.h>

#include "common/file_io.h"

using namespace opennova::threedi;

namespace {

int failures = 0;

bool iequals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	return true;
}

#define CHECK(cond, msg)                                              \
	do {                                                              \
		if (cond) {                                                   \
			std::printf("ok: %s\n", msg);                             \
		} else {                                                      \
			std::printf("FAIL: %s\n", msg);                           \
			++failures;                                               \
		}                                                             \
	} while (0)

const char *const kAuthoredModels[] = {"crate.3di", "person.3di", "person_hd.3di", "akm.3di", "arms.3di"};

// The reset clip whose bone rows a rigged model must reproduce (ADR 0046
// decision 5, ADR 0047): the clips pair with model rows by index, and every
// clip is measured against this one bind. The row count is the model's own
// (the arms carry 38 of the FP rig's 46 rows, the rifle 45); the coupled rows
// exclude the arms' ground proxy, which sits on the clip's gun-root row (the
// shape retail's ArmsG.3di had too).
struct RigCoupling {
	const char *model;
	const char *reset_clip;
	int rows;
	int coupled;
};
const RigCoupling kRigCouplings[] = {
    {"person.3di", "person_rst.bad", 20, 20},
    {"person_hd.3di", "person_rst.bad", 20, 20},
    {"akm.3di", "akm_rst.bad", 45, 45},
    {"arms.3di", "akm_rst.bad", 38, 37},
};

const RigCoupling *rig_coupling_for(const char *file) {
	for (const RigCoupling &row : kRigCouplings)
		if (iequals(row.model, file)) return &row;
	return nullptr;
}

// Shader tag -> the authored .fx artifact that serves it (assets/README.md "Shaders").
struct TagEffect {
	const char *tag;
	const char *effect;
};
const TagEffect kTagEffects[] = {
	{"VS_PHONGT", "phongt.fx"},          {"VS_SKBASIC", "skbasic.fx"},
	{"VS_SKBUMPDIFFT2", "skbdifft2.fx"}, {"VS_SKBUMPDIFFOBJ", "skbdiffo.fx"},
	{"VS_SKBUMPDIFFOBJ2", "skbdiffo2.fx"}, {"VS_SKBUMPPHONGT", "skbphongt.fx"},
	{"VS_SKBUMPPHONGOBJ", "skbphongo.fx"},
};


// The authored allowlist (assets/.gitignore "!/name" lines), the set's one manifest.
std::vector<std::string> allowlist(const std::string &assets) {
	std::vector<std::string> names;
	std::ifstream manifest(assets + "/.gitignore");
	std::string line;
	while (std::getline(manifest, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("!/", 0) == 0) names.push_back(line.substr(2));
	}
	return names;
}

bool allowlisted(const std::vector<std::string> &names, const std::string &name) {
	return std::any_of(names.begin(), names.end(), [&](const std::string &n) { return iequals(n, name); });
}

const char *effect_for_tag(const std::string &tag) {
	if (tag.rfind("FF_", 0) == 0) return "_ffp.fx";
	for (const TagEffect &row : kTagEffects)
		if (iequals(row.tag, tag)) return row.effect;
	return nullptr;
}

} // namespace

int main() {
	const std::string assets = std::string(test_paths_repo_root(__FILE__)) + "/assets";
	const std::vector<std::string> names = allowlist(assets);
	static const char kLfsSentinel[] = "version https://git-lfs";
	for (const char *file : kAuthoredModels) {
		const std::string path = assets + "/" + file;
		std::vector<uint8_t> bytes;
		if (!test_io::read_file(path, bytes) || bytes.empty()) {
			std::printf("FAIL: %s missing (export it: --export-models)\n", file);
			++failures;
			continue;
		}
		if (bytes.size() >= sizeof(kLfsSentinel) - 1 &&
				std::memcmp(bytes.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", file);
			continue;
		}
		std::printf("== %s\n", file);
		CHECK(allowlisted(names, file), "the model is allowlisted in assets/.gitignore");
		Threedi3di3 model = {};
		if (threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) != 0) {
			std::printf("FAIL: %s does not parse\n", file);
			++failures;
			continue;
		}
		const std::string stem = std::string(file).substr(0, std::strlen(file) - 4);
		CHECK(iequals(model.header.name, stem), "the GHDR name is the file's stem");
		CHECK(model.lod_count >= 1, "the model carries at least one LOD");
		if (model.collision != nullptr) {
			CHECK(threedi_3di3_collision_is_runtime_safe(model.collision) == 1,
					"the collision block is safe for runtime queries");
		}
		bool any_tangent_tag = false;
		for (uint32_t i = 0; i < model.material_count; ++i) {
			const ThreediMaterial &m = model.materials[i];
			const std::string tag(m.shader_name);
			const opennova::renderer::MaterialDescriptorRecord *descriptor =
					opennova::renderer::find_material_descriptor(tag);
			CHECK(descriptor != nullptr, ("material tag resolves in the descriptor table: " + tag).c_str());
			if (descriptor != nullptr && (descriptor->shader_flags & opennova::renderer::MATERIAL_FLAG_TANGENT) != 0)
				any_tangent_tag = true;
			const char *effect = effect_for_tag(tag);
			CHECK(effect != nullptr && allowlisted(names, effect),
					("an authored .fx serves the tag: " + tag).c_str());
			for (uint32_t t = 0; t < m.texture_count; ++t) {
				const std::string texture(m.textures[t].name);
				CHECK(allowlisted(names, texture), ("the texture is an allowlisted asset: " + texture).c_str());
			}
		}
		const RigCoupling *coupling = rig_coupling_for(file);
		CHECK((coupling != nullptr) || model.header.mesh_type != THREEDI_MESH_SKINNED,
				"a skinned model names the reset clip it is coupled to");
		if (coupling != nullptr && model.lod_count >= 1) {
			// The rig coupling: the model's parent table and pivots, run through the
			// engine's own bind relation, reproduce the reset clip's bone table.
			opennova::bad::BadFile reset = {};
			const std::string clip_path = assets + "/" + coupling->reset_clip;
			if (opennova::bad::bad_parse(clip_path.c_str(), &reset) != 0) {
				std::printf("FAIL: %s does not parse\n", coupling->reset_clip);
				++failures;
			} else {
				const ThreediLod &lod0 = model.lods[0];
				CHECK(static_cast<int>(lod0.render_object_count) == coupling->rows,
						"the rig carries the row count the coupling table names");
				const size_t coupled = std::min<size_t>(static_cast<size_t>(coupling->coupled), reset.num_bones);
				std::vector<int> parents;
				std::vector<opennova::anim::Vec3> rel;
				for (size_t pi = 0; pi < lod0.render_object_count; ++pi) {
					const ThreediRenderObject &ro = lod0.render_objects[pi];
					parents.push_back(ro.parent_index == static_cast<int32_t>(pi) ? -1 : ro.parent_index);
					rel.push_back(opennova::anim::Vec3{ro.rel[0], ro.rel[1], ro.rel[2]});
				}
				bool parents_match = true;
				for (size_t b = 0; b < coupled && b < parents.size(); ++b) {
					const int clip_parent = reset.bones[b].parent_index;
					if ((clip_parent < 0 ? -1 : clip_parent) != parents[b]) parents_match = false;
				}
				CHECK(parents_match, "the coupled rows' parents equal the reset clip's bone parents");
				const std::vector<opennova::anim::Vec3> derived =
						opennova::anim::positions_from_model(reset, parents, rel);
				float worst = 0.0f;
				for (size_t b = 0; b < coupled && b < derived.size(); ++b) {
					const float dx = derived[b].x - reset.bones[b].position[0];
					const float dy = derived[b].y - reset.bones[b].position[1];
					const float dz = derived[b].z - reset.bones[b].position[2];
					const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
					if (d > worst) worst = d;
				}
				char msg[160];
				std::snprintf(msg, sizeof(msg),
						"the pivots reproduce the reset clip's bone positions (worst %.4f m)", worst);
				CHECK(worst < 0.002f, msg);
				opennova::bad::bad_free(&reset);
			}
		}
		for (size_t li = 0; li < model.lod_count; ++li) {
			const bool has_tangents = (model.lods[li].vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0;
			CHECK(has_tangents == any_tangent_tag,
					"VERT carries the tangent format iff a material tag reads TANGENT");
			const bool skinned = (model.lods[li].vertices.flags & THREEDI_VERTEX_FLAG_SKINNED) != 0;
			CHECK(skinned == (model.header.mesh_type == THREEDI_MESH_SKINNED),
					"VERT's skinned flag agrees with the GHDR mesh type");
		}
		threedi_3di3_free(&model);
	}
	if (failures == 0) std::printf("OK: the Godot-authored models are runtime- and retail-ready\n");
	return failures == 0 ? 0 : 1;
}
