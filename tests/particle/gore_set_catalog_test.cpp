// D-PTL-24 on the retail data: the effect catalog spans every `.ptl` PLUS one
// gore set — `.ptu` (US) or `.ptg` (German) — and the gore effects resolve to
// their own emitter profiles, not to the invisible stockeffect clone an unknown
// name degrades to (D-PTL-8). Effect_SGvBody follows the mounted data: where
// SHOTGNFX.PTL declares it with the `pdefs =GvBody_Drops` typo the
// first-registration rule lets that copy win over US_BLOOD.PTU's working one
// and the engine clears the effect whole (rendering it would be a divergence);
// a mount whose first declaration resolves renders it.
// [orig: CEffectSystem_Init @0x5f6070 scans `ptl\*.ptl` @0x5f6228 then
//  `ptl\*<ext>` @0x5f6356]
// Gated on OPENNOVA_JO_DIR (a retail JO install).
#include "common/retail_paths.h"

#include <base/resource_index/resource_index.h>
#include <base/vfs/vfs.h>
#include <formats/particle/parser.h>
#include <formats/particle/particle.h>
#include <runtime/particle/effect_scene.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

const char *const kGoreSetEffects[] = {
	"Effect_AmHitBody", "Effect_FX50CalBody", "Effect_FX50CalDirt", "Effect_FX50CalMetal"};
const char *const kRetailBrokenEffect = "Effect_SGvBody";
const char *const kControlEffect = "Effect_AmHitDirt";
const char *const kAbsentEffect = "Effect_NoSuchEffect_ProbeSentinel";

bool ends_with(const std::string &s, const std::string &suffix) {
	return s.size() >= suffix.size() && retail::lower_ascii(s).compare(s.size() - suffix.size(), suffix.size(), retail::lower_ascii(suffix)) == 0;
}

struct Probe {
	uint32_t handle = 0;
	bool spawned = false;
	size_t emitters = 0;
};

Probe probe(particle::EffectScene &scene, const char *name) {
	Probe out;
	scene.reset_runtime_state();
	const particle::EffectHandle h = scene.intern(name);
	out.handle = h.value;
	if (!h) return out;
	particle::EffectSpawnRequest req;
	req.effect = h;
	const particle::EffectSpawnReceipt rc = scene.spawn(req);
	out.spawned = rc.status == particle::EffectSpawnStatus::Spawned;
	if (!out.spawned) return out;
	particle::EffectAdvanceRequest step;
	step.delta_seconds = 1.0f / 62.5f;
	scene.advance_simulation(step);
	const particle::EffectDebugSnapshot snap = scene.inspect(false);
	for (const particle::EffectGroupDebugSnapshot &g : snap.groups) out.emitters += g.emitters.size();
	return out;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(), "OPENNOVA_JO_DIR (a retail JO install)");
	ResourceIndex index;
	if (!index.scan(install)) return retail::skip("a mountable OPENNOVA_JO_DIR install");

	// 1. The mount selects a gore extension.
	const std::string ext = index.particle_extension();
	std::printf("gore_set: particle_extension() = %s\n", ext.c_str());
	expect(ext == ".ptu" || ext == ".ptg", "the mount selects .ptu or .ptg");

	// 2. The index carries the gore set; the catalog loads every .ptl then the
	//    gore files, in VFS order (first registration wins on duplicate ids).
	particle::EffectSceneConfig config;
	size_t ptl_files = 0, gore_files = 0;
	for (int pass = 0; pass < 2; ++pass) {
		const std::string want = pass == 0 ? ".ptl" : ext;
		std::vector<ResourceFileEntry> group;
		for (const ResourceFileEntry &entry : index.resource_files("*"))
			if (!entry.logical_name.empty() && ends_with(entry.logical_name, want)) group.push_back(entry);
		std::sort(group.begin(), group.end(), [](const ResourceFileEntry &a, const ResourceFileEntry &b) {
			return retail::lower_ascii(a.logical_name) < retail::lower_ascii(b.logical_name);
		});
		for (const ResourceFileEntry &entry : group) {
			std::vector<uint8_t> bytes;
			if (!index.read_file(entry.logical_name, bytes) || bytes.empty()) continue;
			particle::EffectCatalogDocument doc;
			doc.source = entry.logical_name;
			particle::ParseError error;
			if (!particle::load_particles_from_buffer(
						reinterpret_cast<const char *>(bytes.data()), bytes.size(), doc.file, error)) {
				std::printf("gore_set: %s did not parse: %s\n", entry.logical_name.c_str(),
						error.message.c_str());
				continue;
			}
			if (pass == 0) ++ptl_files;
			else {
				++gore_files;
				std::printf("gore_set:   %s\n", entry.logical_name.c_str());
			}
			config.documents.push_back(std::move(doc));
		}
	}
	std::printf("gore_set: indexed %zu .ptl + %zu %s\n", ptl_files, gore_files, ext.c_str());
	expect(gore_files > 0, "the gore set is indexed (the loader can see it)");
	particle::EffectScene scene;
	const particle::EffectLoadReport report = scene.open(config);
	std::printf("gore_set: catalog %zu effects across %zu files (%zu pdefs, %zu unresolved refs)\n",
			report.effect_count, report.document_count, report.particle_definition_count,
			report.unresolved_particle_reference_count);
	if (!expect(report.effect_count > 0, "the mount carries particle effects")) return 1;

	// 3. Controls: an absent name still interns and spawns as the stockeffect
	//    clone; a plain .ptl effect spawns.
	const Probe absent = probe(scene, kAbsentEffect);
	std::printf("gore_set: control absent %s -> handle %u, %zu emitter(s) (the stockeffect clone)\n",
			kAbsentEffect, absent.handle, absent.emitters);
	const Probe control = probe(scene, kControlEffect);
	std::printf("gore_set: control %s -> %zu emitter(s)\n", kControlEffect, control.emitters);
	expect(control.emitters > 0, "Effect_AmHitDirt (a plain .ptl effect) spawns");

	// 4. Every gore effect resolves to its own profile.
	for (const char *name : kGoreSetEffects) {
		const Probe got = probe(scene, name);
		const bool ok = got.emitters > 0 && got.emitters != absent.emitters;
		std::printf("gore_set: %s %s -> handle %u, spawned %d, %zu emitter(s)\n", ok ? "PASS" : "FAIL",
				name, got.handle, int(got.spawned), got.emitters);
		char msg[200];
		std::snprintf(msg, sizeof(msg), "%s spawns (its pdefs resolved)", name);
		expect(got.emitters > 0, msg);
		std::snprintf(msg, sizeof(msg), "%s carries its own emitter profile, not the clone's", name);
		expect(got.emitters != absent.emitters, msg);
	}

	// 5. The shotgun body hit follows the first declaration the walk meets: a
	//    copy with an unresolvable pdef clears the effect whole; an intact one
	//    renders. Decide from the documents themselves, then hold the scene to it.
	std::string first_source;
	bool first_intact = true;
	bool declared = false;
	for (const particle::EffectCatalogDocument &doc : config.documents) {
		for (const particle::EffectDef &effect : doc.file.effects) {
			if (retail::lower_ascii(effect.id) != retail::lower_ascii(kRetailBrokenEffect)) continue;
			declared = true;
			first_source = doc.source;
			for (const std::string &pdef : effect.pdefs) {
				bool found = false;
				for (const particle::EffectCatalogDocument &other : config.documents)
					if (other.file.find_particle(pdef) != nullptr) {
						found = true;
						break;
					}
				if (!found) {
					std::printf("gore_set: %s declares %s with the unresolvable pdef %s\n",
							doc.source.c_str(), kRetailBrokenEffect, pdef.c_str());
					first_intact = false;
				}
			}
			break;
		}
		if (declared) break;
	}
	expect(declared, "the mount declares Effect_SGvBody");
	const Probe broken = probe(scene, kRetailBrokenEffect);
	std::printf("gore_set: %s first declared by %s (%s) -> handle %u, spawned %d, %zu emitter(s)\n",
			kRetailBrokenEffect, first_source.c_str(), first_intact ? "intact" : "typo: cleared whole",
			broken.handle, int(broken.spawned), broken.emitters);
	if (first_intact)
		expect(broken.emitters > 0, "an intact first Effect_SGvBody declaration renders");
	else
		expect(broken.emitters == 0,
				"Effect_SGvBody resolves to nothing (the typo'd first declaration wins on name order)");

	if (failures == 0) std::printf("gore_set_catalog: OK (%s)\n", ext.c_str());
	return failures == 0 ? 0 : 1;
}
