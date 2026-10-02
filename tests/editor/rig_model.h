#pragma once
// A model whose records name its CTRL registers and its MTRX rows by index (S13 D8's Record
// references), built with the construction API as the fixture generators build every model
// (ThreediBuildModel through our own writer, ADR 0003; never retail bytes): what the model
// document's and the asset graph's tests renumber.
#include <cstdint>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>

#include "fixtures/minimal_3di_builder.h"

namespace rig_model {

// Four registers: HEAT_GLOW, EWEAP_GUNYAW (named by nothing), EWEAP_GUNPITCH (named by the glow
// material's RGB generator and the arm's Y track) and FLICKER (by a light and the arm's X track),
// each above style 0x70; the arm's Euler row turning through MTRX row 2 of three (the identity and
// two frames). `second_generator`: the paint material's second RGB generator, which no field of
// the editor sets, names register 3 too.
inline std::vector<uint8_t> bytes(bool second_generator = false) {
	using namespace opennova::threedi;
	synth3di::Model m;
	m.name = "rig";
	const int lod = m.add_lod();
	const int paint = m.add_material("FF_ST_OP", "rig.tga");
	const int glow = m.add_material("FF_ST_AD_LUM", "glow.tga");
	const int p_base = m.add_part(lod, 0, ThreediBuildVec3{});
	m.add_box(lod, p_base, paint, ThreediBuildBox{{-0.5, -0.5, 0.0}, {0.5, 0.5, 0.2}});
	const int p_arm = m.add_part(lod, 0, ThreediBuildVec3{0.0, 0.0, 0.2});
	m.add_box(lod, p_arm, glow, ThreediBuildBox{{-0.1, -0.1, 0.2}, {0.1, 0.1, 1.0}}, true);
	m.add_control_register("HEAT_GLOW");
	m.add_control_register("EWEAP_GUNYAW");
	m.add_control_register("EWEAP_GUNPITCH");
	m.add_control_register("FLICKER");
	const int black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
	m.set_rgb_gen(glow, THREEDI_PANM_STYLE_CONTROL_REGISTER, 2, 0.0, black, white);
	m.add_light(ThreediBuildVec3{0.0, 0.0, 1.0}, 0.0, 3.0, THREEDI_PANM_STYLE_CONTROL_REGISTER, p_arm, white, black,
	            0, 3);
	m.add_panm(lod, p_base, 0);
	ThreediPartAnimation &turn = m.add_panm(lod, p_arm, 0, threedi_panm_pack_flags(0, 2, 0, 0));
	turn.rotation_x = synth3di::track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 3, 0, 0, 16384);
	turn.rotation_y = synth3di::track(THREEDI_PANM_STYLE_CONTROL_REGISTER, 2, 0, 0, 16384);
	turn.matrix_index = 2;
	ThreediMatrix4x4 frame;
	threedi_mat4_identity(&frame);
	m.frames = {frame, frame};
	if (second_generator) {
		m.materials[paint].rgb_gen2.style = 0x72;
		m.materials[paint].rgb_gen2.reg = 3;
	}
	std::vector<uint8_t> out;
	if (!synth3di::mint(m, out)) out.clear();
	return out;
}

} // namespace rig_model
