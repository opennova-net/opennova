#include <runtime/world/death_piece_draw.h>

#include <base/io/fixed.h>
#include <runtime/renderer/object_lod.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/world.h>

#include <cstdlib>
#include <vector>

namespace opennova::world {

// [orig: DeathPiece_RenderSection @ 0x57b6c5..0x57b6f4 — `xor eax,
//  eax` then the first ecx with (1 << cl) clear in piece+0x7C, ecx < the level
//  mesh's +0x34]
int32_t death_piece_render_section(uint32_t hidden_mask, int32_t section_count) {
	for (int32_t section = 0; section < section_count; ++section) {
		if ((hidden_mask & (1u << (static_cast<uint32_t>(section) & 31u))) == 0)
			return section;
	}
	return 0;
}

// [orig: DeathPiece_RenderVisible @ 0x57b863..0x57b8dc (the model and the
//  level), DeathPiece_RenderSection @ 0x57b69b..0x57b81a (the level
//  mesh, the pivot, the collapse and the submit flags)]
bool death_piece_draw(const DeathPiece &piece, const DeathPieceModel &model,
		int32_t projected_radius_q16, float lod_scale, int32_t water_z_q16,
		DeathPieceDraw &out) {
	if (!piece.active || !model.loaded()) return false;
	const int level = renderer::death_piece_lod_level(
			projected_radius_q16, lod_scale, model.lod_threshold_q16);
	// The sub-pixel floor, and an empty level slot (the chain's level 3 on a
	// three-level model) [orig: @ 0x57b87b; `test ebx,ebx; jz` @ 0x57b6bc].
	if (level < 0 || static_cast<size_t>(level) >= model.lod_section_count.size())
		return false;
	const int32_t section_count = model.lod_section_count[static_cast<size_t>(level)];
	out = DeathPieceDraw{};
	out.item_id = piece.item_id;
	out.lod_level = level;
	out.hidden_mask = piece.hidden_mask;
	// With a hidden mask the matrix is EulerScale(pose) * T(-centre) of the
	// drawn section's COBJ row; without one the plain EulerScale(pose)
	// [orig: `test esi,esi; jz` @ 0x57b6cb; T(-centre) @ 0x57b6f6..0x57b71f;
	//  Matrix_Multiply3x4_FixedPoint @ 0x57b759].
	if (piece.hidden_mask != 0) {
		out.section = death_piece_render_section(piece.hidden_mask, section_count);
		out.pivoted = true;
		if (static_cast<size_t>(out.section) < model.section_origin_q16.size())
			out.pivot_q16 = model.section_origin_q16[static_cast<size_t>(out.section)];
	}
	out.pos = piece.pos;
	out.heading = piece.heading;
	out.pitch = piece.pitch;
	out.roll = piece.roll;
	// The scale rides the matrix as ftol(piece+0x88 * 65536.0)
	// [orig: @ 0x57b724..0x57b730].
	out.scale = static_cast<float>(static_cast<int32_t>(piece.render_scale * io::kFp16OneD)) *
			io::kInvFp16One;
	// Transparents go to the below-water queue unless the piece is strictly
	// above the water plane [orig: `cmp ecx, Env_WaterHeightFixed; setnle`
	// @ 0x57b7fb..0x57b80e].
	out.below_water = !(to_fixed(piece.pos.z) > water_z_q16);
	out.projected_radius_q16 = projected_radius_q16;
	return true;
}

// [orig: DeathPiece_CollectVisible @ 0x57b560 — per active slot (piece+0
//  nonzero @ 0x57b5a1): the per-axis x/y box against radius + fog
//  (@ 0x57b5b4..0x57b5de, `jg` skips), the view depth against radius + fog
//  (Math_FixedPointTransformPoint22 @ 0x57b5ef, `jge` @ 0x57b607), the
//  viewport clip (Viewport_TransformAndClipPoint @ 0x57b614, 1 = culled) and
//  the recorded projected radius dword_A784F0 @ 0x57b627; then
//  DeathPiece_RenderVisible @ 0x57b830 walks the rows at the frame's detail
//  level]
void OcclusionWorld::collect_death_piece_draws(const World &world,
		const OcclusionFrameCamera &cam, std::vector<DeathPieceDraw> &out) const {
	out.clear();
	// The locked profile runs the highest shipped detail level (the frame
	// scale's precedent, renderer/object_lod.h).
	const float lod_scale = renderer::death_piece_lod_scale(renderer::kObjectLodDetailLevelMax);
	const auto &pieces = world.death_pieces.pieces;
	for (size_t slot = 0; slot < pieces.size(); ++slot) {
		const DeathPiece &piece = pieces[slot];
		if (!piece.active) continue;
		const ItemDeathTraits *traits = world.tables.item_death_traits.get(piece.item_id);
		if (traits == nullptr || !traits->piece_model.loaded()) continue;
		const int32_t pos[3] = {to_fixed(piece.pos.x), to_fixed(piece.pos.y),
				to_fixed(piece.pos.z)};
		const int64_t reach = static_cast<int64_t>(piece.radius_q16) + cam.fog_dist;
		if (std::llabs(static_cast<int64_t>(pos[0]) - cam.pos_fixed[0]) > reach ||
				std::llabs(static_cast<int64_t>(pos[1]) - cam.pos_fixed[1]) > reach)
			continue;
		int32_t depth = 0;
		if (!sphere_in_view(cam, pos, piece.radius_q16, &depth)) continue;
		const int32_t projected = renderer::project_bound_sphere_radius_q16(
				piece.radius_q16, depth, cam.focal_pixels);
		DeathPieceDraw draw;
		if (!death_piece_draw(piece, traits->piece_model, projected, lod_scale, cam.water_z,
					draw))
			continue;
		draw.slot = static_cast<int32_t>(slot);
		draw.generation = piece.generation;
		out.push_back(draw);
	}
}

} // namespace opennova::world
