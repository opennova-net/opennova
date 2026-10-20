#pragma once
// The fixtures the model-family editor tests open over each format's variants (the model document, its
// table, surfaces and collision; the clip and animation map documents; the face animation document): every
// file of the format's fixture directories, and the variants the format's own rows mint (the threedi,
// threedi_gp, bad/anim and grm sweeps), each of which must be there: a missing one fails its test, never
// skips it. The JO corpus these sweeps once read from a retail install is closed, so each of its forms is
// minted once by the format's generator and read here; the forms only the editor sees are minted in the
// tests themselves. Paths are the repository root's.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "editor/variant_files.h"

namespace model_variants {

using editor_test::repo_path;

inline std::vector<uint8_t> bytes_of(const std::string &relative) { return editor_test::repo_bytes(relative); }

struct Files {
	std::vector<std::string> paths;    // repository-relative, sorted, each once
	std::vector<std::string> required; // the variants the format's rows mint, there or not
	std::vector<std::string> missing;  // the required paths that are not there
	bool is_required(const std::string &path) const {
		return std::find(required.begin(), required.end(), path) != required.end();
	}
};

// The sweep of editor/variant_files.h, with the required paths kept.
inline Files files(const std::vector<std::string> &dirs, const std::vector<std::string> &extensions,
                   const std::vector<std::string> &required) {
	editor_test::VariantFiles found = editor_test::variant_files(dirs, extensions, required);
	Files out;
	out.paths = std::move(found.files);
	out.required = required;
	out.missing = std::move(found.missing);
	return out;
}

inline int report_missing(const Files &found) {
	for (const std::string &path : found.missing)
		std::fprintf(stderr, "FAIL %s: a variant its format's row mints is not there\n", path.c_str());
	return int(found.missing.size());
}

inline Files models() {
	return files({"fixtures/threedi", "fixtures/threedi/synth"}, {".3di"},
	             {
	                     // An opaque material wearing a constant AlphaGen, two-sided, no alpha test (SCrate1's MTRL row).
	                     "fixtures/threedi/crate_alphagen24.3di",
	                     // Bullet faces whose pad bytes are not zero (the exporter's stale bytes; most shipped
	                     // faces, ak47_3rd's among them).
	                     "fixtures/threedi/synth/crate_cfac_pad.3di",
	                     // Face flag words 0x100, 0x101, 0x400, 0x401, 0x500, 0x501 (Dblkhwk1, Barbrol1, Armry02,
	                     // Bsflgblu, Trash02, Comtowr1).
	                     "fixtures/threedi/synth/armory_cfac_flags.3di",
	                     // A LOD's PANM chunk there and empty beside LOD 0's rows (CNet01, Flsh_1st).
	                     "fixtures/threedi/synth/pump_panm_empty_lod1.3di",
	                     // A LOD with no parts and no strips (CNet01).
	                     "fixtures/threedi/synth/pump_lod1_empty.3di",
	                     // A strip whose material index names no material (Tent1).
	                     "fixtures/threedi/synth/house_strip_material_past.3di",
	                     // A texture row with no name (Det_1st).
	                     "fixtures/threedi/synth/crate_texture_unnamed.3di",
	                     // MTRX rows with a translation, row 0 not the identity (every shipped model's row 0).
	                     "fixtures/threedi/synth/mount_mtrx_translated.3di",
	                     // An MTRX row holding a NaN word (US01).
	                     "fixtures/threedi/synth/person_mtrx_nan.3di",
	                     // Omni lights of the shipped flag bytes (2, 3, 6, 16, 18, 64, 65, 66, 146, 160), one with a
	                     // -0.0 axis word (CmpFire1).
	                     "fixtures/threedi/synth/shed_lght_omni_flags.3di",
	                     // Volume flag words with bits above the blink letters' 0x3E (Armry02).
	                     "fixtures/threedi/synth/armory_bvol_flags_high.3di",
	                     // A flipbook on the register clock, its texture rows flagged and framed (ArmGlove).
	                     "fixtures/threedi/synth/person_flipbook_register.3di",
	                     // Volumes of the types 0, 5, 9, 10, 12, 13 and 16 (farp_lnd's 13, FAH6a's 16).
	                     "fixtures/threedi/synth/crate_bvol_types.3di",
	                     // A user point on part -1 (Bed01).
	                     "fixtures/threedi/synth/crate_usrp_no_part.3di",
	             });
}

// The Black Hawk Down GP models the threedi_gp rows mint (the variants of the 677 BHD ships), one of each kind
// BHD ships: a rigid GPM, the one skinned GPS (Oicw_1R) and a skinned GPP, each with the techniques the
// migration notes (a bump or environment-mapped, a flat-colour, an additive and a region-textured material,
// a flipbook on a BHD counter, a spinning part, a lens-flare user point, a face rounds pass through).
inline Files gp_models() {
	return files({"fixtures/threedi_gp"}, {".3di"},
	             {"fixtures/threedi_gp/synth_gpm.3di", "fixtures/threedi_gp/synth_gps.3di",
	              "fixtures/threedi_gp/synth_gpp.3di"});
}

// The clips: every clip of fixtures/anim (the authored one-bone ones and the minted rig19 and vm40 sets,
// tests/fixtures/minimal_bad_gen.cpp, all of which anim_rewrite sweeps) and the forms of the shipped clips the
// anim rows mint, each read and written back field for field.
inline Files clips() {
	return files({"fixtures/anim", "fixtures/bad"}, {".bad"},
	             {
	                     // A version 0 clip: events with no trigger word (m60_1d).
	                     "fixtures/anim/rig19_v0.bad",
	                     // Channels keyed sparsely, each on a duration table of its own (DVFLEE1E).
	                     "fixtures/anim/rig19_sprs.bad",
	                     // Two frames, two keys a bone, one fewer than the fence posts (DT1RST).
	                     "fixtures/anim/rig19_short.bad",
	                     // The exporter's leftovers the reader drops: bone names with bytes past their NUL, stale
	                     // addresses (most shipped bones; 357_1f); flags 0x9, a loop with bit 3 (CycDr23).
	                     "fixtures/anim/rig19_junk.bad",
	                     // Flag bit 3 on a translated clip, flags 0xA (47GL_1r).
	                     "fixtures/anim/vm40_swt.bad",
	                     // Flag bit 3 alone, flags 0x8 (Idl_rld).
	                     "fixtures/anim/rig19_b3.bad",
	                     // Flag bit 3 on a translated loop, flags 0xB (74gl_1d).
	                     "fixtures/anim/vm40_b3lp.bad",
	                     // A child bone's dead position field X-triplicated (ak47_1st's rig).
	                     "fixtures/anim/vm40_brk.bad",
	                     // More keys than frames, the translation rows to match (M60_1i).
	                     "fixtures/anim/vm40_long.bad",
	                     // A translated clip with no pad row past row frame_count (stgr_RST).
	                     "fixtures/anim/vm40_nopad.bad",
	             });
}

// The animation maps: the authored tables (fixtures/anim), the base game's (assets/: plain blobs, no LFS pull)
// and any the anim rows mint (fixtures/adm).
inline Files maps() { return files({"fixtures/anim", "fixtures/adm", "assets"}, {".adm"}, {}); }

// The face animations: the authored person.grm and the one the grm rows mint at the scale and in the layout
// Black Hawk Down's tool wrote its 13 faces in (tests/fixtures/minimal_grm_gen.cpp), which the writer must give
// back byte for byte: vertex and triangle numbers of one, two and three digits, triangle corners past 99, a
// gesture of ten and more parameters, a parameter of no group (its line ending in the two spaces before the
// group), mixed-case gesture names, negative offsets, and texture paths of a drive letter and backslashes
// (Delta01: D:\DFV\Characters\GRM\idltF01a.tga).
inline Files faces() { return files({"fixtures/grm"}, {".grm"}, {"fixtures/grm/face108.grm"}); }

} // namespace model_variants
