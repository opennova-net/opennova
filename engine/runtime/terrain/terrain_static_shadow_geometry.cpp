#include <runtime/terrain/terrain_static_shadow_geometry.h>

#include <base/io/strutil.h>
#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/material_eval.h>
#include <runtime/renderer/object_shader_template.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_strip_decode.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace opennova::terrain {
namespace {

constexpr uint64_t kFnvOffset = UINT64_C(1469598103934665603);
constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);

uint64_t hash_bytes(uint64_t hash, const void *data, std::size_t size) {
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (std::size_t i = 0; i < size; ++i) {
		hash = (hash ^ bytes[i]) * kFnvPrime;
	}
	return hash;
}

template <typename T>
uint64_t hash_value(uint64_t hash, const T &value) {
	return hash_bytes(hash, &value, sizeof(value));
}

// The adapter hashed Godot Strings as to_lower().utf8() bytes; authored 3DI
// names are ASCII, so ascii-lowering preserves the byte stream (and the page
// content stamps built on it).
uint64_t hash_lowered_string(uint64_t hash, std::string_view value) {
	for (const char c : value) {
		const char lowered = strutil::ascii_tolower(c);
		hash = hash_bytes(hash, &lowered, 1);
	}
	return hash;
}

uint64_t hash_alpha_pyramid(uint64_t hash,
		const std::shared_ptr<const TerrainStaticShadowAlphaPyramid>
				&pyramid) {
	const bool present = pyramid != nullptr;
	hash = hash_value(hash, present);
	if (!present) return hash;
	const auto filter = TerrainStaticShadowMipFilter::Linear;
	hash = hash_value(hash, filter);
	for (const auto &mip : pyramid->mips) {
		hash = hash_value(hash, mip.width);
		hash = hash_value(hash, mip.height);
		hash = hash_value(hash, mip.row_stride);
	}
	for (const std::vector<uint8_t> &alpha : pyramid->storage) {
		hash = hash_value(hash, alpha.size());
		if (!alpha.empty()) {
			hash = hash_bytes(hash, alpha.data(), alpha.size());
		}
	}
	return hash;
}

// The four framebuffer blend classes [orig: decode_blend_mode_to_d3d_states @0x680f00].
TerrainStaticShadowBlend map_blend(opennova::renderer::ObjectBlendMode blend) {
	switch (blend) {
		case opennova::renderer::ObjectBlendMode::Opaque:
			return TerrainStaticShadowBlend::Opaque;
		case opennova::renderer::ObjectBlendMode::AlphaBlend:
			return TerrainStaticShadowBlend::Alpha;
		case opennova::renderer::ObjectBlendMode::Additive:
			return TerrainStaticShadowBlend::Additive;
		case opennova::renderer::ObjectBlendMode::Multiplicative:
			return TerrainStaticShadowBlend::Multiply;
	}
	return TerrainStaticShadowBlend::Opaque;
}

// First frame-zero texture name in the diffuse slot — the adapter's
// `diffuse_a` selection.
std::string static_diffuse_name(const ThreediMaterial &mat) {
	for (uint32_t i = 0; i < mat.texture_count && i < 24u; ++i) {
		const ThreediMaterialTexture &tex = mat.textures[i];
		if ((tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0 && tex.frame != 0) {
			continue;
		}
		if (tex.slot == THREEDI_TEX_SLOT_DIFFUSE) {
			return std::string(tex.name);
		}
	}
	return {};
}

// Animated frame names for one slot, by authored frame ordinal; missing
// frames stay empty.
std::vector<std::string> material_anim_frames(const ThreediMaterial &mat,
		uint8_t slot) {
	std::vector<std::string> out;
	const int frames = static_cast<int>(mat.animation.num_frames);
	if (frames <= 0) return out;
	out.resize(static_cast<std::size_t>(frames));
	for (uint32_t i = 0; i < mat.texture_count && i < 24u; ++i) {
		const ThreediMaterialTexture &tex = mat.textures[i];
		if (tex.slot == slot && (tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0 &&
				tex.frame < frames) {
			out[static_cast<std::size_t>(tex.frame)] = std::string(tex.name);
		}
	}
	return out;
}

void expand_bounds(TerrainStaticShadowResolvedGeometry &geometry,
		const std::array<float, 3> &point) {
	if (!geometry.has_bounds) {
		geometry.local_min = point;
		geometry.local_max = point;
		geometry.has_bounds = true;
		return;
	}
	for (int axis = 0; axis < 3; ++axis) {
		geometry.local_min[axis] = std::min(geometry.local_min[axis],
				point[axis]);
		geometry.local_max[axis] = std::max(geometry.local_max[axis],
				point[axis]);
	}
}

bool raw_strip_ranges_are_valid(const ThreediLod &lod,
		const ThreediTriangleStrip &strip) {
	if (lod.indices.indices == nullptr || lod.vertices.items == nullptr ||
			strip.num_indices == 0 || strip.num_vertices <= 0 ||
			strip.index_offset < 0 || strip.start_vertex < 0) {
		return false;
	}
	const uint64_t index_end = static_cast<uint64_t>(strip.index_offset) +
			strip.num_indices;
	const uint64_t vertex_end = static_cast<uint64_t>(strip.start_vertex) +
			static_cast<uint32_t>(strip.num_vertices);
	return index_end <= lod.indices.count && vertex_end <= lod.vertices.count;
}

bool all_finite(const std::array<float, 3> &value) {
	return std::isfinite(value[0]) && std::isfinite(value[1]) &&
			std::isfinite(value[2]);
}

// Raw vertex ranges give conservative bounds even for a strip whose indices
// are bad; presentation-world (-x, y, z) throughout.
void expand_authored_strip_bounds(
		TerrainStaticShadowResolvedGeometry &geometry, const ThreediLod &lod,
		const ThreediTriangleStrip &strip,
		const ThreediRenderObject &render_object) {
	if (!raw_strip_ranges_are_valid(lod, strip)) {
		geometry.bounds_exact = false;
		return;
	}
	const std::array<float, 3> offset{-render_object.abs[0],
			render_object.abs[1], render_object.abs[2]};
	if (!all_finite(offset)) {
		geometry.bounds_exact = false;
		return;
	}
	const uint32_t first = static_cast<uint32_t>(strip.start_vertex);
	const uint32_t count = static_cast<uint32_t>(strip.num_vertices);
	for (uint32_t index = 0; index < count; ++index) {
		const ThreediVertex &vertex = lod.vertices.items[first + index];
		const std::array<float, 3> point{-vertex.position[0],
				vertex.position[1], vertex.position[2]};
		if (!all_finite(point)) {
			geometry.bounds_exact = false;
			continue;
		}
		expand_bounds(geometry, {point[0] + offset[0], point[1] + offset[1],
										point[2] + offset[2]});
	}
}

} // namespace

std::vector<const char *> terrain_static_shadow_unsupported_reason_names(
		uint32_t issues) {
	std::vector<const char *> names;
	if ((issues & kTerrainStaticShadowUnsupportedMissingAlphaTexture) != 0) {
		names.push_back("missing_alpha_texture");
	}
	if ((issues & kTerrainStaticShadowUnsupportedFlipbook) != 0) {
		names.push_back("flipbook");
	}
	if ((issues & kTerrainStaticShadowUnsupportedInvalidMaterial) != 0) {
		names.push_back("invalid_material");
	}
	if ((issues & kTerrainStaticShadowUnsupportedIncompleteGeometry) != 0) {
		names.push_back("incomplete_geometry");
	}
	if ((issues & kTerrainStaticShadowUnsupportedMalformedIndices) != 0) {
		names.push_back("malformed_indices");
	}
	if ((issues & kTerrainStaticShadowUnsupportedMissingRequiredUvs) != 0) {
		names.push_back("missing_required_uvs");
	}
	return names;
}

TerrainStaticShadowMaterialState terrain_static_shadow_evaluate_material(
		const TerrainStaticShadowResolvedGeometry &geometry,
		const TerrainStaticShadowResolvedMaterial &material,
		uint32_t time_ms,
		const opennova::renderer::ControlRegisterValues &control_values) {
	TerrainStaticShadowMaterialState state;
	state.issues = material.unsupported_issues;
	if (!material.casts_projected_shadow ||
			!material.samples_diffuse_alpha) {
		return state;
	}

	const opennova::renderer::MaterialRuntime runtime = opennova::renderer::eval_material_runtime(
			material.runtime_material, time_ms,
			geometry.control_register_names, control_values);
	state.uv = runtime.uv;
	if (material.uses_material_alpha) {
		state.alpha_scale = runtime.alpha;
	}

	if (material.diffuse_alpha_frames.empty()) {
		state.issues |= kTerrainStaticShadowUnsupportedMissingAlphaTexture;
		return state;
	}
	if (material.diffuse_alpha_frames.size() > 1) {
		state.diffuse_frame = opennova::renderer::compute_anim_frame(
				material.runtime_material,
				static_cast<uint32_t>(material.diffuse_alpha_frames.size()),
				time_ms, geometry.control_register_names, control_values);
	}
	if (state.diffuse_frame < 0 ||
			static_cast<std::size_t>(state.diffuse_frame) >=
					material.diffuse_alpha_frames.size()) {
		state.issues |= kTerrainStaticShadowUnsupportedFlipbook;
		return state;
	}
	state.alpha_texture = material.diffuse_alpha_frames[
			static_cast<std::size_t>(state.diffuse_frame)].get();
	if (state.alpha_texture == nullptr) {
		state.issues |= kTerrainStaticShadowUnsupportedMissingAlphaTexture;
	}
	return state;
}

std::shared_ptr<TerrainStaticShadowResolvedGeometry>
resolve_terrain_static_shadow_geometry(const Threedi3di3 &model,
		std::string_view graphic,
		TerrainStaticShadowTextureProvider &textures) {
	auto geometry = std::make_shared<TerrainStaticShadowResolvedGeometry>();
	uint64_t hash = hash_lowered_string(kFnvOffset, graphic);
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr) {
		return {};
	}
	geometry->control_register_names.reserve(model.ctrl.count);
	for (uint32_t index = 0; index < model.ctrl.count; ++index) {
		geometry->control_register_names.emplace_back(
				model.ctrl.registers[index].name);
		hash = hash_lowered_string(hash,
				geometry->control_register_names.back());
	}

	if (model.material_count > 0 && model.materials == nullptr) {
		return {};
	}
	const int material_count = static_cast<int>(model.material_count);
	geometry->materials.reserve(static_cast<std::size_t>(
			std::max(material_count, 0)));
	for (int material_index = 0; material_index < material_count;
			++material_index) {
		const ThreediMaterial &mat = model.materials[material_index];
		// Only these three flag bits reach the PROJSHAD raster state: alpha
		// test/invert select the cutout compare and two-sided drops the cull —
		// the technique's other inputs are forced (black lighting, silhouette
		// color) [orig: the per-material PROJSHAD technique block (+336)
		// consumed by the FlushBatches pass loop @ 0x5d9f50, class selection
		// @ 0x5d9ff3; docs/render/render-material-re.md §Pass execution].
		// The mask also keeps geometry keys (and page content stamps)
		// unchanged across the pushdown.
		int material_flags = 0;
		if ((mat.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0) {
			material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
		}
		if ((mat.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0) {
			material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_INVERT;
		}
		if ((mat.material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) != 0) {
			material_flags |= THREEDI_MATERIAL_FLAG_TWO_SIDED;
		}
		const int alpha_ref = std::clamp(
				static_cast<int>(mat.alpha_test_value_byte), 0, 255);
		// Empty shader names resolve to the runtime tag table's first entry,
		// FF_ST_OP (engine/runtime/renderer/material_descriptor.h).
		const std::string shader_tag =
				mat.shader_name[0] != '\0' ? std::string(mat.shader_name)
										   : std::string("FF_ST_OP");
		const opennova::renderer::ObjectMaterialClassification classification =
				opennova::renderer::classify_object_material(shader_tag,
						static_cast<uint8_t>(material_flags),
						mat.emissive_type == THREEDI_EMISSIVE_FULL
								? THREEDI_EMISSIVE_FULL
								: 0,
						mat.is_glass != 0 ? 1 : 0,
						static_cast<uint8_t>(alpha_ref));
		const opennova::renderer::ObjectShaderPipelineDescriptor pipeline =
				opennova::renderer::describe_object_shader_pipeline(
						opennova::renderer::build_object_shader_key(classification));
		const opennova::renderer::ObjectProjectedShadowPolicy projected_policy =
				opennova::renderer::object_projected_shadow_policy(pipeline.technique);
		TerrainStaticShadowResolvedMaterial material;
		material.casts_projected_shadow = projected_policy !=
				opennova::renderer::ObjectProjectedShadowPolicy::NoPass;
		material.blend = projected_policy ==
				opennova::renderer::ObjectProjectedShadowPolicy::MaterialBlend
				? map_blend(classification.blend)
				: TerrainStaticShadowBlend::Opaque;
		material.alpha_test_enabled =
				material.casts_projected_shadow &&
				(material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0;
		material.alpha_test_inverted =
				material.alpha_test_enabled &&
				(material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0;
		material.alpha_ref = static_cast<uint8_t>(alpha_ref);
		material.two_sided =
				material.casts_projected_shadow &&
				(material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) != 0;
		const bool needs_alpha = material.alpha_test_enabled ||
				material.blend == TerrainStaticShadowBlend::Alpha;
		material.samples_diffuse_alpha = needs_alpha;
		material.uses_material_alpha = needs_alpha && projected_policy ==
				opennova::renderer::ObjectProjectedShadowPolicy::MaterialBlend;
		material.runtime_material = mat;
		const int anim_frames = static_cast<int>(mat.animation.num_frames);
		const int anim_type = static_cast<int>(mat.animation.animation_type);
		const int anim_control = static_cast<int>(
				mat.animation.cycle_frame_time);
		if (needs_alpha && anim_frames > 1) {
			const std::vector<std::string> frames = material_anim_frames(
					mat, THREEDI_TEX_SLOT_DIFFUSE);
			if (static_cast<int>(frames.size()) != anim_frames) {
				material.unsupported_issues |=
						kTerrainStaticShadowUnsupportedFlipbook;
			} else {
				material.diffuse_alpha_frames.reserve(
						static_cast<std::size_t>(anim_frames));
				for (const std::string &frame : frames) {
					std::shared_ptr<const TerrainStaticShadowAlphaPyramid>
							alpha = textures.load_alpha(frame);
					material.diffuse_alpha_frames.push_back(std::move(alpha));
				}
			}
		} else if (needs_alpha) {
			material.diffuse_alpha_frames.push_back(textures.load_alpha(
					static_diffuse_name(mat)));
		}
		hash = hash_lowered_string(hash, shader_tag);
		hash = hash_value(hash, material_flags);
		hash = hash_value(hash, alpha_ref);
		hash = hash_value(hash, material.casts_projected_shadow);
		hash = hash_value(hash, material.blend);
		hash = hash_value(hash, material.alpha_test_enabled);
		hash = hash_value(hash, material.alpha_test_inverted);
		hash = hash_value(hash, material.two_sided);
		hash = hash_value(hash, material.samples_diffuse_alpha);
		hash = hash_value(hash, material.uses_material_alpha);
		hash = hash_value(hash, material.unsupported_issues);
		hash = hash_value(hash, mat.alpha_gen.style);
		hash = hash_value(hash, mat.alpha_gen.phase);
		hash = hash_value(hash, mat.alpha_gen.reg);
		hash = hash_value(hash, mat.alpha_gen.rate);
		hash = hash_value(hash, mat.alpha_gen.start);
		hash = hash_value(hash, mat.alpha_gen.end);
		// RGB itself is forced black in PROJSHAD, but retail evaluates RgbGen
		// between AlphaGen and UV. A noise RgbGen therefore advances the shared
		// waveform stream before a noise UV channel and remains a runtime input.
		hash = hash_value(hash, mat.rgb_gen.style);
		hash = hash_value(hash, mat.rgb_gen.phase);
		hash = hash_value(hash, mat.rgb_gen.reg);
		hash = hash_value(hash, mat.rgb_gen.rate);
		for (const float component : mat.rgb_gen.start_color) {
			hash = hash_value(hash, component);
		}
		for (const float component : mat.rgb_gen.end_color) {
			hash = hash_value(hash, component);
		}
		hash = hash_value(hash, mat.u_params.style);
		hash = hash_value(hash, mat.u_params.phase);
		hash = hash_value(hash, mat.u_params.reg);
		hash = hash_value(hash, mat.u_params.gen_rate);
		hash = hash_value(hash, mat.u_params.start);
		hash = hash_value(hash, mat.u_params.end);
		hash = hash_value(hash, mat.v_params.style);
		hash = hash_value(hash, mat.v_params.phase);
		hash = hash_value(hash, mat.v_params.reg);
		hash = hash_value(hash, mat.v_params.gen_rate);
		hash = hash_value(hash, mat.v_params.start);
		hash = hash_value(hash, mat.v_params.end);
		hash = hash_value(hash, anim_frames);
		hash = hash_value(hash, anim_type);
		hash = hash_value(hash, anim_control);
		hash = hash_value(hash, material.diffuse_alpha_frames.size());
		for (const auto &alpha : material.diffuse_alpha_frames) {
			hash = hash_alpha_pyramid(hash, alpha);
		}
		geometry->materials.push_back(std::move(material));
	}

	const int lod_count = static_cast<int>(model.lod_count);
	for (int lod = 0; lod < 2; ++lod) {
		if (lod >= lod_count) continue;
		uint32_t lod_issues = kTerrainStaticShadowUnsupportedNone;
		// Terrain_CollectAndRenderTileModels copies the same entity transform
		// into all ROBJ/bone matrix slots before the PROJSHAD submit. The shipped
		// skinned vertex path computes a weight sum whose weights total one, so
		// identical matrices collapse exactly to this rigid source geometry;
		// PANM is not an input to this pass.
		// [orig: @0x60D926..0x60D971; _vsSkPost.fx
		// vsSkinPostBlackT1; _BaseInc.fx CalcSkinWorldPosAndNormal]
		geometry->lod_unsupported_issues[static_cast<std::size_t>(lod)] =
				lod_issues;
		const ThreediLod *native_lod =
				static_cast<std::size_t>(lod) < model.lod_count &&
						model.lods != nullptr
				? &model.lods[lod]
				: nullptr;
		const int render_objects = native_lod != nullptr
				? std::clamp(static_cast<int>(native_lod->render_object_count),
						0, static_cast<int>(
								std::numeric_limits<uint16_t>::max()))
				: 0;
		geometry->render_object_counts[static_cast<std::size_t>(lod)] =
				static_cast<uint16_t>(render_objects);
		auto &coverage = geometry->coverage[static_cast<std::size_t>(lod)];
		coverage.resize(static_cast<std::size_t>(render_objects));
		hash = hash_value(hash, lod);
		hash = hash_value(hash, render_objects);
		hash = hash_value(hash, lod_issues);

		// Preserve the authored strip population independently of the curated
		// triangle soup below: the soup correctly drops malformed strips, but
		// the projection provider must then report the lost silhouette
		// instead of mistaking the shortened list for exact geometry.
		if (native_lod == nullptr) {
			for (auto &state : coverage) {
				state.authored_surface_count = 1;
				state.malformed_indices = true;
			}
			geometry->bounds_exact = false;
		} else {
			std::size_t strip_cursor = 0;
			for (int render_object = 0; render_object < render_objects;
					++render_object) {
				auto &state = coverage[
						static_cast<std::size_t>(render_object)];
				if (native_lod->render_objects == nullptr ||
						static_cast<std::size_t>(render_object) >=
								native_lod->render_object_count) {
					state.authored_surface_count = 1;
					state.malformed_indices = true;
					geometry->bounds_exact = false;
					continue;
				}
				const ThreediRenderObject &robj =
						native_lod->render_objects[render_object];
				const int64_t authored =
						static_cast<int64_t>(robj.num_strips) +
						robj.num_alpha_strips;
				if (authored < 0 || authored >
						static_cast<int64_t>(
								std::numeric_limits<uint32_t>::max())) {
					state.authored_surface_count = 1;
					state.malformed_indices = true;
					geometry->bounds_exact = false;
					continue;
				}
				state.authored_surface_count =
						static_cast<uint32_t>(authored);
				for (int64_t surface = 0; surface < authored;
						++surface, ++strip_cursor) {
					if (native_lod->strips == nullptr ||
							strip_cursor >= native_lod->strip_count) {
						state.malformed_indices = true;
						geometry->bounds_exact = false;
						continue;
					}
					const ThreediTriangleStrip &strip =
							native_lod->strips[strip_cursor];
					if (!terrain_static_shadow_strip_indices_are_valid(
							native_lod->indices.indices,
							native_lod->indices.count, strip.index_offset,
							strip.num_indices, native_lod->vertices.count,
							strip.start_vertex, strip.num_vertices)) {
						state.malformed_indices = true;
					}
					expand_authored_strip_bounds(*geometry, *native_lod,
							strip, robj);
				}
			}
			if (strip_cursor != native_lod->strip_count) {
				for (auto &state : coverage) {
					state.malformed_indices = true;
				}
				geometry->bounds_exact = false;
			}
		}

		// The curated triangle soup: per-part cursor walk in authored strip
		// order (opaque then alpha per ROBJ), loader index decode, and
		// per-triangle unroll — sequential indices, presentation (-x, y, z).
		std::vector<TerrainStaticShadowResolvedSurface> surfaces;
		if (native_lod != nullptr && native_lod->vertices.items != nullptr &&
				native_lod->indices.indices != nullptr &&
				native_lod->strips != nullptr) {
			std::size_t strip_cursor = 0;
			for (int part_idx = 0; part_idx < render_objects; ++part_idx) {
				const ThreediRenderObject &robj =
						native_lod->render_objects[part_idx];
				const std::size_t part_strip_count =
						static_cast<std::size_t>(robj.num_strips) +
						robj.num_alpha_strips;
				for (std::size_t s = 0; s < part_strip_count &&
						strip_cursor < native_lod->strip_count;
						++s, ++strip_cursor) {
					const ThreediTriangleStrip &strip =
							native_lod->strips[strip_cursor];
					std::vector<uint16_t> decoded;
					if (!threedi_decode_strip_indices(*native_lod, strip, decoded)) {
						continue;
					}
					const uint32_t vertex_offset =
							static_cast<uint32_t>(strip.start_vertex);
					TerrainStaticShadowResolvedSurface resolved;
					resolved.render_object = static_cast<uint16_t>(part_idx);
					resolved.render_object_offset = {-robj.abs[0],
							robj.abs[1], robj.abs[2]};
					resolved.material_index = threedi_material_array_index_for_id(
							model, strip.material_index);
					for (std::size_t i = 0; i + 2 < decoded.size(); i += 3) {
						bool triangle_ok = true;
						for (int corner = 0; corner < 3; ++corner) {
							const uint32_t src = vertex_offset +
									decoded[i + static_cast<std::size_t>(
											corner)];
							if (src >= native_lod->vertices.count) {
								triangle_ok = false;
								break;
							}
						}
						if (!triangle_ok) continue;
						for (int corner = 0; corner < 3; ++corner) {
							const ThreediVertex &v = native_lod->vertices
									.items[vertex_offset + decoded[
											i + static_cast<std::size_t>(
													corner)]];
							resolved.vertices.push_back({-v.position[0],
									v.position[1], v.position[2]});
							resolved.uvs.push_back({v.uv0[0], v.uv0[1]});
							resolved.indices.push_back(static_cast<int32_t>(
									resolved.vertices.size() - 1));
						}
					}
					if (resolved.vertices.empty()) continue;
					surfaces.push_back(std::move(resolved));
				}
			}
		}

		// The adapter hashed Godot Array::size() — a 64-bit count.
		hash = hash_value(hash, static_cast<int64_t>(surfaces.size()));
		auto &out_surfaces = geometry->surfaces[
				static_cast<std::size_t>(lod)];
		out_surfaces.reserve(surfaces.size());
		for (TerrainStaticShadowResolvedSurface &resolved : surfaces) {
			const int render_object = resolved.render_object;
			if (render_object < 0 || render_object >= render_objects) {
				for (auto &state : coverage) {
					state.malformed_indices = true;
				}
				continue;
			}
			auto &surface_coverage = coverage[
					static_cast<std::size_t>(render_object)];
			if (resolved.vertices.empty() || resolved.indices.size() < 3) {
				surface_coverage.malformed_indices = true;
				continue;
			}
			hash = hash_value(hash, resolved.render_object);
			hash = hash_value(hash, resolved.render_object_offset[0]);
			hash = hash_value(hash, resolved.render_object_offset[1]);
			hash = hash_value(hash, resolved.render_object_offset[2]);
			hash = hash_value(hash, resolved.material_index);
			for (std::size_t vertex_index = 0;
					vertex_index < resolved.vertices.size(); ++vertex_index) {
				const std::array<float, 3> &vertex =
						resolved.vertices[vertex_index];
				const std::array<float, 2> &uv = resolved.uvs[vertex_index];
				hash = hash_value(hash, uv[0]);
				hash = hash_value(hash, uv[1]);
				hash = hash_value(hash, vertex[0]);
				hash = hash_value(hash, vertex[1]);
				hash = hash_value(hash, vertex[2]);
			}
			bool valid_indices = true;
			for (const int32_t value : resolved.indices) {
				if (value < 0 || static_cast<std::size_t>(value) >=
						resolved.vertices.size()) {
					valid_indices = false;
					break;
				}
				hash = hash_value(hash, value);
			}
			if (!valid_indices) {
				surface_coverage.malformed_indices = true;
				continue;
			}
			const bool material_requires_uvs = resolved.material_index >= 0 &&
					resolved.material_index <
							static_cast<int>(geometry->materials.size()) &&
					geometry->materials[static_cast<std::size_t>(
							resolved.material_index)].casts_projected_shadow &&
					(geometry->materials[static_cast<std::size_t>(
							resolved.material_index)].alpha_test_enabled ||
							geometry->materials[static_cast<std::size_t>(
									resolved.material_index)].blend ==
									TerrainStaticShadowBlend::Alpha);
			if (material_requires_uvs &&
					resolved.uvs.size() < resolved.vertices.size()) {
				surface_coverage.missing_required_uvs = true;
			}
			++surface_coverage.valid_surface_count;
			out_surfaces.push_back(std::move(resolved));
		}
		for (const auto &state : coverage) {
			hash = hash_value(hash, state.authored_surface_count);
			hash = hash_value(hash, state.valid_surface_count);
			hash = hash_value(hash, state.malformed_indices);
			hash = hash_value(hash, state.missing_required_uvs);
		}
	}
	geometry->key = hash == 0 ? 1 : hash;
	return geometry;
}

} // namespace opennova::terrain
