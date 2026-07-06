// Render-state parity vectors (REN-1, the T1 instrument — ADR 0023).
//
// Walks the full material input matrix — every shader tag in the canonical
// 45-entry descriptor table (plus unknown-tag probes) x the THREEDI material
// flag byte x emissive x glass x alpha-test byte — through the
// classification/composition chain (classify_object_material ->
// build_object_shader_key -> compose_object_shader_glsl) and compares the
// canonical text against the committed golden
// (tests/renderer/render_state_vectors.golden).
//
// POLICY (mirrors ENG-1's env parity vectors, godot/tests/env_parity_vectors_test.gd):
//
//  - The golden was dumped ONCE from the then-current implementation
//    (pinned-current). During the REN grills its rows converge to the
//    witnessed-expected state decoded from Jointops.exe
//    (docs/render/render-material-re.md when it lands).
//  - A mismatch means the producer chain changed. If the change was NOT a
//    deliberate, witnessed port step: it is a regression — fix the code,
//    never the golden.
//  - A deliberate change re-dumps ONLY because the producer changed, and the
//    commit that re-dumps carries the witness citation
//    ([orig: Name @ 0xADDR] or a D-RMAT/ledger row). Tolerances are never
//    widened; there are none here — comparison is exact text.
//  - Dump mode is LOUD: OPENNOVA_RENDER_VECTORS_DUMP=1 rewrites the golden
//    in the source tree and then FAILS the test, so a dump run can never be
//    mistaken for green.
//
// Later REN slices extend the vector set (sort-key/pass classification at
// REN-3, lighting scalars at REN-5) as their producers land in libs/renderer.

#include "renderer/material_classify.h"
#include "renderer/object_shader_template.h"
#include "oed/material_descriptor.h"
#include "threedi/threedi_3di3.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *blend_name(renderer::ObjectBlendMode blend) {
	switch (blend) {
		case renderer::ObjectBlendMode::Opaque: return "opaque";
		case renderer::ObjectBlendMode::AlphaBlend: return "alpha_blend";
		case renderer::ObjectBlendMode::Additive: return "additive";
		case renderer::ObjectBlendMode::Multiplicative: return "multiplicative";
	}
	return "?";
}

const char *normal_space_name(renderer::ObjectNormalSpace space) {
	switch (space) {
		case renderer::ObjectNormalSpace::None: return "none";
		case renderer::ObjectNormalSpace::Tangent: return "tangent";
		case renderer::ObjectNormalSpace::Object: return "object";
	}
	return "?";
}

uint64_t fnv1a64(const std::string &text) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (unsigned char c : text) {
		hash ^= c;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

std::string generate() {
	using namespace renderer;

	std::ostringstream out;
	out << "# render-state-vectors v1 (renderer_state_vectors_test; REN-1, ADR 0023)\n";

	// Tag list: the full canonical table, in table order, then the
	// unknown-tag probes (a non-OED tag, a case-mismatch probe, and empty —
	// classification is deliberately exact-tag only).
	std::vector<std::string> tags;
	for (size_t i = 0; i < oed::kMaterialInfoTableCount; ++i)
		tags.push_back(oed::kMaterialInfoTable[i].name);
	tags.push_back("VS_LEAVESWIND");
	tags.push_back("ff_st_op");
	tags.push_back("");

	// THREEDI_MATERIAL_FLAG_* byte combos: every subset of
	// ALPHA_TEST(0x01) | ALPHA_INVERT(0x02) | TWO_SIDED(0x04) that is
	// distinct for the classifier, including INVERT-without-TEST (0x02).
	const uint8_t flag_combos[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07 };
	const uint8_t emissive_types[] = { THREEDI_EMISSIVE_NONE, THREEDI_EMISSIVE_FULL };
	const uint8_t glass_flags[] = { 0, 1 };

	std::set<uint32_t> keys;
	char line[512];

	for (const std::string &tag : tags) {
		for (uint8_t flags : flag_combos) {
			// The alpha byte only feeds alpha_test_value; vary it where the
			// ALPHA_TEST flag is set, pin one midpoint sample elsewhere.
			std::vector<uint8_t> alpha_bytes;
			if (flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST)
				alpha_bytes = { 0, 200 };
			else
				alpha_bytes = { 128 };
			for (uint8_t em : emissive_types) {
				for (uint8_t gl : glass_flags) {
					for (uint8_t atb : alpha_bytes) {
						const auto cls = classify_object_material(tag, flags, em, gl, atb);
						const uint32_t key = build_object_shader_key(cls);
						keys.insert(key);
						std::snprintf(line, sizeof(line),
						              "tag=%s flags=%02x em=%u gl=%u atb=%u | "
						              "known=%d fam=%s blend=%s lum=%d emis=%d two=%d "
						              "nmap=%d glass=%d env=%d atest=%d atinv=%d atval=%.6f "
						              "det=%d skin=%d spec=%d nuv2=%d nspace=%s key=%08x\n",
						              tag.empty() ? "<empty>" : tag.c_str(),
						              flags, em, gl, atb,
						              cls.known_shader ? 1 : 0,
						              object_shader_family_name(cls.family),
						              blend_name(cls.blend),
						              cls.is_luminance ? 1 : 0,
						              cls.is_emissive ? 1 : 0,
						              cls.is_two_sided ? 1 : 0,
						              cls.needs_normal_map ? 1 : 0,
						              cls.is_glass ? 1 : 0,
						              cls.uses_environment ? 1 : 0,
						              cls.alpha_test ? 1 : 0,
						              cls.alpha_test_invert ? 1 : 0,
						              static_cast<double>(cls.alpha_test_value),
						              cls.has_detail ? 1 : 0,
						              cls.is_skinned ? 1 : 0,
						              cls.uses_specular ? 1 : 0,
						              cls.normal_uses_uv2 ? 1 : 0,
						              normal_space_name(cls.normal_space),
						              key);
						out << line;
					}
				}
			}
		}
	}

	// Section 2: every unique shader key's composed GLSL, pinned by hash +
	// length. A composer change re-hashes exactly the keys whose source
	// changed; the re-dump carries the citation for the change.
	out << "# composed-glsl (key -> fnv1a64, length)\n";
	for (uint32_t key : keys) {
		const std::string glsl = renderer::compose_object_shader_glsl(key);
		std::snprintf(line, sizeof(line), "key=%08x fnv64=%016llx len=%zu\n",
		              key,
		              static_cast<unsigned long long>(fnv1a64(glsl)),
		              glsl.size());
		out << line;
	}
	return out.str();
}

} // namespace

int main() {
	const std::string generated = generate();
	const char *golden_path = RENDER_STATE_VECTORS_GOLDEN;

	if (std::getenv("OPENNOVA_RENDER_VECTORS_DUMP") != nullptr) {
		std::ofstream out(golden_path, std::ios::binary);
		if (!out) {
			std::cerr << "FAIL: dump mode could not open golden for write: "
			          << golden_path << '\n';
			return 1;
		}
		out << generated;
		out.close();
		std::cerr <<
			"================ RENDER VECTOR DUMP ================\n"
			"Golden rewritten at " << golden_path << " (" <<
			generated.size() << " bytes).\n"
			"THIS RUN IS DELIBERATELY RED. Re-dumping is only legitimate\n"
			"when the producer chain changed on purpose, and the commit\n"
			"that re-dumps must carry the witness citation\n"
			"([orig: Name @ 0xADDR] / D-RMAT row) for the change (ADR 0023).\n"
			"====================================================\n";
		return 1;
	}

	std::ifstream in(golden_path, std::ios::binary);
	if (!in) {
		std::cerr << "FAIL: golden missing: " << golden_path << "\n"
		          << "Initial dump: set OPENNOVA_RENDER_VECTORS_DUMP=1 and rerun.\n";
		return 1;
	}
	std::stringstream buf;
	buf << in.rdbuf();
	const std::string golden = buf.str();

	if (golden == generated) {
		std::cerr << "renderer_state_vectors ok ("
		          << generated.size() << " bytes)\n";
		return 0;
	}

	// Report the first differing lines compactly, then the policy.
	std::istringstream a(golden), b(generated);
	std::string la, lb;
	int line_no = 0, reported = 0;
	while (reported < 8) {
		const bool has_a = static_cast<bool>(std::getline(a, la));
		const bool has_b = static_cast<bool>(std::getline(b, lb));
		if (!has_a && !has_b)
			break;
		++line_no;
		if (!has_a) la.clear();
		if (!has_b) lb.clear();
		if (la != lb) {
			std::cerr << "line " << line_no << ":\n  golden:    "
			          << (la.empty() ? "<eof>" : la) << "\n  generated: "
			          << (lb.empty() ? "<eof>" : lb) << '\n';
			++reported;
		}
	}
	std::cerr <<
		"FAIL: render-state vectors diverged from the committed golden.\n"
		"If this was NOT a deliberate witnessed port step, it is a\n"
		"regression: fix the code, never the golden. A deliberate change\n"
		"re-dumps (OPENNOVA_RENDER_VECTORS_DUMP=1) and cites its witness in\n"
		"the same commit (ADR 0023; docs/render/).\n";
	return 1;
}
