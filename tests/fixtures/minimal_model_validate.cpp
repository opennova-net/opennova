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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_paths.h"
#include <formats/threedi/threedi_3di3.h>
#include <runtime/renderer/material_descriptor.h>

#include "common/file_io.h"

using namespace opennova::threedi;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                              \
	do {                                                              \
		if (cond) {                                                   \
			std::printf("ok: %s\n", msg);                             \
		} else {                                                      \
			std::printf("FAIL: %s\n", msg);                           \
			++failures;                                               \
		}                                                             \
	} while (0)

const char *const kAuthoredModels[] = {"crate.3di"};

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

bool iequals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	return true;
}

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
