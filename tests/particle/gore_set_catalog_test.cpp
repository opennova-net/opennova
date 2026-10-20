// D-PTL-24: the effect catalog spans every `.ptl` PLUS one gore set — `.ptu` (US) or `.ptg` (German,
// where the mount carries fgn2.bin) — and the gore effects resolve to their own emitter profiles, not
// to the invisible stockeffect clone an unknown name degrades to (D-PTL-8). Effect_SGvBody follows the
// mounted data: where SHOTGNFX.PTL declares it with the `pdefs =GvBody_Drops` typo, the
// first-registration rule lets that copy win over US_BLOOD.PTU's working one and the engine clears the
// effect whole (rendering it would be a divergence); a mount whose first declaration resolves (JO:CA's,
// whose shotgnfx.ptl declares no Effect_SGvBody) renders it.
// [orig: CEffectSystem_Init @0x5f6070 scans `ptl\*.ptl` @0x5f6228, then walks
//  the archives' `.ptl` and gore-set entries interleaved @0x5f64c0..0x5f6560;
//  its loose gore leg (no wildcard @0x5f6387) reads nothing]
// Over minted mounts (loose effect files written from the text below, in the shipped files' layout):
// the US mount the retired retail row read (JO:CA), the same with the SHOTGNFX typo, and the German
// mount. The effect names are the shipped ones the old row probed.
#include "common/file_io.h"
#include "common/temp_dir.h"

#include <base/resource_index/resource_index.h>
#include <base/io/strutil.h>
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
	return s.size() >= suffix.size() &&
	       strutil::to_lower(s).compare(s.size() - suffix.size(), suffix.size(), strutil::to_lower(suffix)) == 0;
}

// --- the minted effect files, in the layout the shipped ones take -------------------------------------

std::string effect(const std::string &id, const std::string &pdefs_line) {
	return "\n[effectdef]\n{\n\tid = " + id + ";\n\t" + pdefs_line + "\n\n}\n";
}

std::string members(const std::string &pdefs) { return "pdefs = " + pdefs + ";"; }

std::string particle_def(const std::string &id) {
	return "\n[particledef]\n{\nid\t= " + id + ";\nflags\t=  EMITVECTOR ;\nmove\t=  NORMAL ;\nlod\t= 0.000;\n\n"
	       "\temit_dur\t= 0.200;\n\temit_rate\t= 40.000;\n\temit_burst\t= 2;\n\temit_dur\t= 0.200;\n"
	       "\tage\t= 0.500;\n\tscale\t= 0.500;\n\tspeed\t= 4.000;\n\tspread\t= 60.000;\n"
	       "\tgraphic1 =\tgore.tga, blend;\n\n}\n";
}

struct File {
	std::string name;
	std::string text;
};

// The base of every mount: the stock effect (one member, the clone an unknown name spawns), a plain
// .ptl effect, the blood mist the gore set's body hit borrows from a .ptl.
std::vector<File> base_files() {
	return {
	        {"stock.ptl", effect("stockeffect", members("Stock_Puff")) + particle_def("Stock_Puff")},
	        {"joAmmoHit.ptl", effect(kControlEffect, members("AmHit_Dirt, AmHit_Dust")) + particle_def("AmHit_Dirt") +
	                                  particle_def("AmHit_Dust") + particle_def("Blood_Mist")},
	};
}

// A gore set: the body hit (one member its own, one a .ptl's), the three .50cal hits, the shotgun body
// hit intact; `prefix` keeps the US and German definitions apart.
std::vector<File> gore_files(const std::string &blood, const std::string &fx50, const std::string &prefix) {
	return {
	        {blood, effect("Effect_AmHitBody", members(prefix + "Gore_Splat, Blood_Mist")) +
	                                effect(kRetailBrokenEffect, members(prefix + "SGvBody_Mist2, " + prefix + "Gore_Splat")) +
	                                particle_def(prefix + "Gore_Splat") + particle_def(prefix + "SGvBody_Mist2")},
	        {fx50, effect("Effect_FX50CalBody", members(prefix + "Cal_Body, " + prefix + "Cal_Puff")) +
	                               effect("Effect_FX50CalDirt", members(prefix + "Cal_Dirt, " + prefix + "Cal_Puff")) +
	                               effect("Effect_FX50CalMetal", members(prefix + "Cal_Metal, " + prefix + "Cal_Puff")) +
	                               particle_def(prefix + "Cal_Body") + particle_def(prefix + "Cal_Dirt") +
	                               particle_def(prefix + "Cal_Metal") + particle_def(prefix + "Cal_Puff")},
	};
}

// SHOTGNFX.PTL's copy of the shotgun body hit as the typo left it: `pdefs =GvBody_Drops`, a name no
// definition has (its particles are SGvBody_*).
File shotgun_with_typo() {
	return {"shotgnfx.ptl", effect(kRetailBrokenEffect, "pdefs =GvBody_Drops;") + particle_def("SGvBody_Drops") +
	                                particle_def("SGvBody_Mist")};
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

// One mount: `files` written loose under a fresh root (fgn2.bin too when `german`), the catalog loaded
// as the retail row loaded it, and its rules held: the gore extension `want_ext`, the gore set indexed,
// the controls, every gore effect its own profile, and the shotgun body hit rendering exactly when
// `sg_renders`.
void check_mount(const char *label, const std::vector<File> &files, bool german, const char *want_ext,
                 bool sg_renders) {
	std::printf("gore_set: --- %s ---\n", label);
	test_temp::TempDir root("gore_set_catalog");
	for (const File &file : files) expect(test_io::write_text(root.file(file.name.c_str()), file.text), "a mount file written");
	if (german) expect(test_io::write_file(root.file("fgn2.bin"), {0x12, 0x34}), "the German marker written");
	ResourceIndex index;
	if (!expect(index.scan(root.root()), "the minted mount scans")) return;

	// 1. The mount selects a gore extension.
	const std::string ext = index.particle_extension();
	std::printf("gore_set: particle_extension() = %s\n", ext.c_str());
	expect(ext == want_ext, "the mount selects its gore set's extension");

	// 2. The index carries the gore set; the catalog loads every .ptl then the
	//    gore files, in name order (first registration wins on duplicate ids).
	particle::EffectSceneConfig config;
	size_t ptl_files = 0, gore_files_read = 0;
	for (int pass = 0; pass < 2; ++pass) {
		const std::string want = pass == 0 ? ".ptl" : ext;
		std::vector<ResourceFileEntry> group;
		for (const ResourceFileEntry &entry : index.resource_files("*"))
			if (!entry.logical_name.empty() && ends_with(entry.logical_name, want)) group.push_back(entry);
		std::sort(group.begin(), group.end(), [](const ResourceFileEntry &a, const ResourceFileEntry &b) {
			return strutil::to_lower(a.logical_name) < strutil::to_lower(b.logical_name);
		});
		for (const ResourceFileEntry &entry : group) {
			std::vector<uint8_t> bytes;
			if (!index.read_file(entry.logical_name, bytes) || bytes.empty()) continue;
			particle::EffectCatalogDocument doc;
			doc.source = entry.logical_name;
			particle::ParseError error;
			if (!particle::load_particles_from_buffer(
						reinterpret_cast<const char *>(bytes.data()), bytes.size(), doc.file, error)) {
				std::printf("gore_set: %s did not parse: %s\n", entry.logical_name.c_str(), error.message.c_str());
				expect(false, "every mounted effect file parses");
				continue;
			}
			if (pass == 0) ++ptl_files;
			else {
				++gore_files_read;
				std::printf("gore_set:   %s\n", entry.logical_name.c_str());
			}
			config.documents.push_back(std::move(doc));
		}
	}
	std::printf("gore_set: indexed %zu .ptl + %zu %s\n", ptl_files, gore_files_read, ext.c_str());
	expect(gore_files_read == 2, "the gore set is indexed (the loader can see it), the other set not");
	particle::EffectScene scene;
	const particle::EffectLoadReport report = scene.open(config);
	if (!expect(report.effect_count > 0, "the mount carries particle effects")) return;

	// 3. Controls: an absent name still interns and spawns as the stockeffect
	//    clone; a plain .ptl effect spawns.
	const Probe absent = probe(scene, kAbsentEffect);
	std::printf("gore_set: control absent %s -> handle %u, %zu emitter(s) (the stockeffect clone)\n",
			kAbsentEffect, absent.handle, absent.emitters);
	expect(absent.emitters == 1, "an absent name spawns the stockeffect clone");
	const Probe control = probe(scene, kControlEffect);
	expect(control.emitters > 0, "Effect_AmHitDirt (a plain .ptl effect) spawns");

	// 4. Every gore effect resolves to its own profile.
	for (const char *name : kGoreSetEffects) {
		const Probe got = probe(scene, name);
		std::printf("gore_set: %s -> handle %u, spawned %d, %zu emitter(s)\n", name, got.handle, int(got.spawned),
				got.emitters);
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
		for (const particle::EffectDef &def : doc.file.effects) {
			if (strutil::to_lower(def.id) != strutil::to_lower(kRetailBrokenEffect)) continue;
			declared = true;
			first_source = doc.source;
			for (const std::string &pdef : def.pdefs) {
				bool found = false;
				for (const particle::EffectCatalogDocument &other : config.documents)
					if (other.file.find_particle(pdef) != nullptr) {
						found = true;
						break;
					}
				if (!found) first_intact = false;
			}
			break;
		}
		if (declared) break;
	}
	expect(declared, "the mount declares Effect_SGvBody");
	expect(first_intact == sg_renders, "the first Effect_SGvBody declaration is the one the mount should meet");
	const Probe broken = probe(scene, kRetailBrokenEffect);
	std::printf("gore_set: %s first declared by %s (%s) -> handle %u, spawned %d, %zu emitter(s)\n",
			kRetailBrokenEffect, first_source.c_str(), first_intact ? "intact" : "typo: cleared whole",
			broken.handle, int(broken.spawned), broken.emitters);
	if (first_intact)
		expect(broken.emitters == 2, "an intact first Effect_SGvBody declaration renders");
	else
		expect(broken.emitters == 0,
				"Effect_SGvBody resolves to nothing (the typo'd first declaration wins on name order)");
}

} // namespace

int main() {
	// Both gore sets on every mount, as the shipped archives carry both; the marker picks one.
	std::vector<File> us = base_files();
	for (File &f : gore_files("US_Blood.ptu", "FX50Cal.ptu", "")) us.push_back(f);
	for (File &f : gore_files("Euro_Blood.ptg", "FX50Cal.ptg", "Euro_")) us.push_back(f);
	check_mount("the US mount (JO:CA's: no typo'd copy)", us, false, ".ptu", true);

	std::vector<File> typo = us;
	typo.push_back(shotgun_with_typo());
	check_mount("the US mount with SHOTGNFX's typo'd copy", typo, false, ".ptu", false);

	check_mount("the German mount", us, true, ".ptg", true);

	if (failures == 0) std::printf("gore_set_catalog: OK\n");
	return failures == 0 ? 0 : 1;
}
