// The model and animation blanks (ADR 0046 DI-33): a model, a clip and an animation map, each the
// smallest file the game's loader takes, made from scratch through the engine's own construction seams
// (formats/threedi threedi_o3d_build over the retail target, the editor's .o3d converter's path;
// formats/bad bad_build_mint and bad_build_mint_table, the .o3a converter's).
#include "blank_makers.h"

#include <sstream>

#include <base/io/strutil.h>
#include <editor/project/project_files.h>
#include <formats/bad/bad_build.h>
#include <formats/threedi/threedi_o3d_lower.h>
#include <runtime/renderer/model_target.h>

namespace opennova::editor {

namespace {

std::string stem_of(const std::string &logical_name) { return utf8_of(path_of(logical_name).stem()); }

// The one-bone rest pose every blank clip is: one looping interval at 30 fps, the shape of retail's reset
// clips (docs/threedi/scene-naming-contract.md), its two keys the identity and its two events still (no
// ground step, no trigger bit, no height). The events are written, never left out: the channel reads the
// frame's event record with no count or null test [orig: AnimChannel_InterpolateKeyframe @ 0x40B230,
// @ 0x40B2AA], so a clip without them would be read from its own header.
bad::BadBuildClip rest_clip(const std::string &name) {
	bad::BadBuildClip clip;
	clip.name = name;
	clip.version = 1;
	clip.fps = 30;
	clip.flags = bad::BAD_FLAG_LOOP;
	clip.frame_count = 1;
	bad::BadBuildBone root;
	root.name = "root";
	root.parent = -1;
	root.keys = {bad::BadBuildQuat{}, bad::BadBuildQuat{}};
	clip.bones.push_back(std::move(root));
	clip.events = {bad::BadBuildEvent{}, bad::BadBuildEvent{}};
	return clip;
}

Diagnostic refusal(CoreFinding code, const BlankRequest &request, const std::string &what, const std::string &why) {
	return make_finding(code, DiagnosticSeverity::Error, what + " could not be made: " + why + ".", request.logical_name);
}

} // namespace

// A model the game loads (ThreediGp_LoadFromFile @ 0x5B5780 refuses no 3DI3 of version 0x103 or later whose
// header's mesh type is 1 or 2 and whose header counts its LODs; GPM_LoadRenderModel @ 0x5B5000 reads RMDL,
// INDX, STRP and ROBJ unguarded, every count loop guarded): one LOD, one root part, one material with the
// fixed-function opaque shader and no texture, and one triangle a metre wide and a metre tall standing on the
// model's origin across its forward axis, written as two faces back to back (each wound counter-clockwise about
// its own normal: a face seen from behind is culled), so the model shows where it stands from any side and
// nothing divides by a count of none (a foliage graphic sizes its cell cache by its vertices,
// docs/foliage/foliage-re.md). Written as the .o3d scene
// text the editor's converter reads and built through threedi_o3d_build for the retail target: the converter's
// path, so the bytes are the engine's own writer's.
bool make_blank_model(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::string name = stem_of(request.logical_name);
	if (name.size() > 15) name.resize(15); // the model's name field (threedi_build: the scene's `model`)
	std::istringstream text("o3d 2\nmodel " + name +
	                        "\nmaterial FF_ST_OP\nlod 0\npart 0 0 0 0\nmesh 0\n"
	                        "v 0 -0.5 0 1 0 0 0 1\nv 0 0.5 0 1 0 0 1 1\nv 0 0 1 1 0 0 0.5 0\n"
	                        "v 0 -0.5 0 -1 0 0 0 1\nv 0 0.5 0 -1 0 0 1 1\nv 0 0 1 -1 0 0 0.5 0\n"
	                        "t 0 1 2\nt 4 3 5\npanm 0 0\n");
	std::vector<threedi::SceneFinding> findings;
	if (!threedi::threedi_o3d_build(text, renderer::retail_model_target(), out, findings)) {
		out.clear();
		std::string why = "the build refused it";
		for (const threedi::SceneFinding &finding : findings)
			if (finding.error) {
				why = finding.message;
				break;
			}
		error = refusal(CoreFinding::BlankModel, request, "The model", why);
		return false;
	}
	return true;
}

// A clip the game loads and plays (BoneFile_Load @ 0x40FFF0: no magic or version check, at least one bone,
// 500,000 bytes at most): the one-bone rest pose (rest_clip), minted alone through the seam, which reads it
// back first. A model's parts past its one bone take that bone's matrix [orig: BoneAnim_BuildWorldMatrices @
// 0x40c400, the padding loop @ 0x40c5a1], so it poses any rig at rest.
bool make_blank_animation(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::string why;
	if (!bad::bad_build_mint(rest_clip(stem_of(request.logical_name)), nullptr, bad::bad_retail_limits(), out, &why)) {
		out.clear();
		error = refusal(CoreFinding::BlankAnimation, request, "The clip", why);
		return false;
	}
	return true;
}

std::string blank_reset_clip_name(const std::string &map_name) {
	// <table>_rst, the exporter's name for a table's reset clip (docs/threedi/scene-naming-contract.md), cut to
	// what a packed name holds (15 bytes with its extension [orig: PFF_FindEntry @ 0x7685D0]).
	constexpr const char *kSuffix = "_rst.bad";
	std::string stem = stem_of(map_name);
	const size_t room = bad::bad_retail_limits().packed_name_bytes - std::char_traits<char>::length(kSuffix);
	if (stem.size() > room) stem.resize(room);
	return strutil::to_lower(stem) + kSuffix;
}

// An animation map the game loads: one anim_reset row, the row without which it loads none [orig:
// AnimMap_LoadAdmFile @ 0x40CC40, the reset row's test @ 0x40CE03, slot 0's head read @ 0x40CE11], naming its
// reset clip (blank_reset_clip_name), the one-bone rest pose made with it where the project has none of that
// name (blank_companion). Minted through the seam's table writer, which reads the table back.
bool make_blank_animation_map(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const std::string clip = blank_reset_clip_name(request.logical_name);
	bad::BadBuildSet set;
	set.adm_name = request.logical_name;
	set.rows.push_back({"anim_reset", {clip}});
	set.clips.push_back(rest_clip(bad::bad_build_clip_stem(clip)));
	std::string text, why;
	if (!bad::bad_build_mint_table(set, bad::bad_retail_limits(), text, &why)) {
		out.clear();
		error = refusal(CoreFinding::BlankAnimation, request, "The animation map", why);
		return false;
	}
	out.assign(text.begin(), text.end());
	return true;
}

} // namespace opennova::editor
