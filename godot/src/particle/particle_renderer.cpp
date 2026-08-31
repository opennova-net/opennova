#include "particle/particle_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/particle/emitter.h>
#include <runtime/particle/graphic_frames.h>

#include "util/texture_path_resolver.h"

using namespace godot;

namespace {

constexpr int kGraphicLayerCount = 4;
constexpr std::uint32_t kWorldVisibilityMask = 1u << 0;
constexpr std::uint32_t kFirstPersonVisibilityMask = 1u << 11;
// One MultiMesh instance is 12 transform + 4 color + 4 custom floats.
constexpr int kFloatsPerInstance = 20;
// Soft-particle fade span in world units (proximity fade replaces retail's
// hard depth intersection).
constexpr float kSoftParticleFadeDistance = 0.75f;
// Modern stand-in for the distort pipeline: screen-space refraction driven by
// the authored distortion texture as a normal map.
constexpr float kDistortRefractionScale = 0.05f;

using opennova::particle::BlendMode;
using opennova::particle::CurveRef;
using opennova::particle::GraphicLayer;
using opennova::particle::Particle;

std::string lower_ascii(std::string value) {
	for (char &c : value) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return value;
}

Ref<Image> load_particle_image(const Callable &provider,
		const String &texture_dir, const std::string &name) {
	const String candidate = String::utf8(name.c_str());
	Ref<Texture2D> texture;
	if (provider.is_valid())
		texture = provider.call(candidate);
	if (texture.is_null() && !texture_dir.is_empty())
		texture = opennova::load_texture_from_dir(texture_dir, candidate);
	if (texture.is_null())
		return Ref<Image>();
	Ref<Image> image = texture->get_image();
	if (image.is_null())
		return Ref<Image>();
	if (image->get_format() != Image::FORMAT_RGBA8)
		image->convert(Image::FORMAT_RGBA8);
	return image;
}

Ref<Image> make_fallback_image() {
	constexpr int side = 32;
	Ref<Image> image = Image::create(side, side, false, Image::FORMAT_RGBA8);
	if (image.is_null())
		return image;
	const float center = (static_cast<float>(side) - 1.0f) * 0.5f;
	for (int y = 0; y < side; ++y) {
		for (int x = 0; x < side; ++x) {
			const float dx = (static_cast<float>(x) - center) / center;
			const float dy = (static_cast<float>(y) - center) / center;
			const float alpha = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy),
					0.0f, 1.0f);
			image->set_pixel(x, y, Color(1.0f, 1.0f, 1.0f, alpha));
		}
	}
	return image;
}

bool blend_is_normal_mapped(BlendMode mode) {
	return mode == BlendMode::Bump || mode == BlendMode::Bumpadd ||
			mode == BlendMode::Distort;
}

// One presentable graphic layer: the flipbook composed as a horizontal strip
// texture (frame count is the material's particles_anim_h_frames), plus the
// derived normal strip for the bump/distort classes whose authored texture is
// the perturbation map, not the albedo.
struct LayerVisual {
	bool present = false;
	BlendMode blend = BlendMode::Blend;
	int flip_frames = 1;
	int flip_rate = 0;
	bool resolved = false;
	Ref<ImageTexture> albedo_strip;
	Ref<ImageTexture> normal_strip;
};

struct DefinitionVisual {
	std::array<LayerVisual, kGraphicLayerCount> layers;
};

struct EmitterBoundsReport {
	std::uint64_t emitter_id = 0;
	bool first_person = false;
	std::uint32_t quad_count = 0;
	AABB bounds;
	bool bounds_valid = false;
};

} // namespace

class ParticleRenderer::Impl {
public:
	// Presenter pool: one MultiMeshInstance3D per (emitter, layer) group with
	// live particles this frame; unused entries are hidden, capacity is kept.
	struct PoolEntry {
		MultiMeshInstance3D *instance = nullptr;
		Ref<MultiMesh> multimesh;
		PackedFloat32Array buffer;
		int capacity = 0;
	};

	std::vector<PoolEntry> pool;
	std::size_t pool_used = 0;
	Ref<QuadMesh> unit_quad;

	std::shared_ptr<const std::vector<opennova::particle::ParticleDef>> catalog_definitions;
	std::vector<DefinitionVisual> definition_visuals;
	// Keyed by definition_index * kGraphicLayerCount + layer_index.
	std::map<std::size_t, Ref<StandardMaterial3D>> materials;
	std::vector<std::string> unresolved_names;
	bool catalog_dirty = true;

	// Per-frame scratch: particle indices grouped by graphic layer, and the
	// camera-distance sort keys, reused across emitters.
	std::array<std::vector<std::size_t>, kGraphicLayerCount> layer_groups;
	std::vector<std::pair<float, std::size_t>> sort_scratch;

	// Frame stats for the debug seam. present_index counts presentations so
	// capture seams can prove a frozen world re-presented for a new camera.
	std::int64_t rendered_quads = 0;
	std::int64_t draw_groups = 0;
	std::int64_t present_index = 0;
	std::vector<EmitterBoundsReport> emitter_reports;

	void invalidate_catalog() {
		catalog_dirty = true;
	}

	void ensure_unit_quad() {
		if (unit_quad.is_valid())
			return;
		unit_quad.instantiate();
		unit_quad->set_size(Vector2(1.0f, 1.0f));
	}

	void hide_pool_tail(std::size_t used) {
		for (std::size_t i = used; i < pool.size(); ++i) {
			if (pool[i].instance != nullptr)
				pool[i].instance->set_visible(false);
		}
	}

	void clear_draws() {
		pool_used = 0;
		rendered_quads = 0;
		draw_groups = 0;
		emitter_reports.clear();
		hide_pool_tail(0);
	}

	PoolEntry &acquire_pool_entry(ParticleRenderer *owner) {
		if (pool_used < pool.size() && pool[pool_used].instance != nullptr)
			return pool[pool_used++];
		if (pool_used < pool.size())
			pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(pool_used),
					pool.end());
		ensure_unit_quad();
		PoolEntry entry;
		entry.instance = memnew(MultiMeshInstance3D);
		entry.instance->set_name(String("ParticleBatch") +
				String::num_int64(static_cast<int64_t>(pool.size())));
		entry.instance->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		entry.instance->set_as_top_level(true);
		entry.multimesh.instantiate();
		entry.multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
		entry.multimesh->set_use_colors(true);
		entry.multimesh->set_use_custom_data(true);
		entry.multimesh->set_mesh(unit_quad);
		entry.instance->set_multimesh(entry.multimesh);
		owner->add_child(entry.instance);
		pool.push_back(entry);
		return pool[pool_used++];
	}

	// Composes a flipbook strip: every frame image blitted into one row, each
	// scaled to the first resolved frame's cell size. Returns null when no
	// frame resolves (the caller falls back or hides the layer).
	Ref<ImageTexture> build_strip(const std::vector<Ref<Image>> &frames) {
		Ref<Image> first;
		for (const Ref<Image> &frame : frames) {
			if (frame.is_valid()) {
				first = frame;
				break;
			}
		}
		if (first.is_null())
			return Ref<ImageTexture>();
		int cell_width = first->get_width();
		int cell_height = first->get_height();
		const int frame_count = static_cast<int>(frames.size());
		constexpr int max_strip_width = 8192;
		if (cell_width * frame_count > max_strip_width) {
			const float shrink = static_cast<float>(max_strip_width) /
					static_cast<float>(cell_width * frame_count);
			cell_width = std::max(1,
					static_cast<int>(static_cast<float>(cell_width) * shrink));
			cell_height = std::max(1,
					static_cast<int>(static_cast<float>(cell_height) * shrink));
		}
		Ref<Image> strip = Image::create(cell_width * frame_count, cell_height,
				false, Image::FORMAT_RGBA8);
		if (strip.is_null())
			return Ref<ImageTexture>();
		for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
			Ref<Image> frame = frames[static_cast<std::size_t>(frame_index)];
			if (frame.is_null())
				frame = first;
			if (frame->get_width() != cell_width ||
					frame->get_height() != cell_height) {
				frame = frame->duplicate();
				frame->resize(cell_width, cell_height, Image::INTERPOLATE_BILINEAR);
			}
			strip->blit_rect(frame,
					Rect2i(0, 0, cell_width, cell_height),
					Vector2i(frame_index * cell_width, 0));
		}
		strip->generate_mipmaps();
		return ImageTexture::create_from_image(strip);
	}

	static Ref<Image> white_albedo_from(const Ref<Image> &source) {
		if (source.is_null())
			return Ref<Image>();
		Ref<Image> result = source->duplicate();
		const int width = result->get_width();
		const int height = result->get_height();
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const Color pixel = result->get_pixel(x, y);
				result->set_pixel(x, y, Color(1.0f, 1.0f, 1.0f, pixel.a));
			}
		}
		return result;
	}

	void rebuild_catalog(
			const std::shared_ptr<const std::vector<opennova::particle::ParticleDef>> &definitions,
			const Callable &provider, const String &texture_dir,
			bool procedural_fallback) {
		catalog_definitions = definitions;
		definition_visuals.clear();
		materials.clear();
		unresolved_names.clear();
		catalog_dirty = false;
		if (!definitions)
			return;
		definition_visuals.resize(definitions->size());

		std::unordered_map<std::string, Ref<Image>> image_cache;
		std::unordered_set<std::string> unresolved_lookup;
		Ref<Image> fallback_image;

		auto frame_image = [&](const std::string &name) -> Ref<Image> {
			const std::string key = lower_ascii(name);
			const auto found = image_cache.find(key);
			if (found != image_cache.end())
				return found->second;
			Ref<Image> image;
			if (!name.empty())
				image = load_particle_image(provider, texture_dir, name);
			if (image.is_null() && !name.empty() &&
					unresolved_lookup.insert(key).second) {
				unresolved_names.push_back(name);
			}
			if (image.is_null() && procedural_fallback) {
				if (fallback_image.is_null())
					fallback_image = make_fallback_image();
				image = fallback_image;
			}
			image_cache.emplace(key, image);
			return image;
		};

		std::vector<Ref<Image>> frames;
		std::vector<Ref<Image>> albedo_frames;
		for (std::size_t definition_index = 0;
				definition_index < definitions->size(); ++definition_index) {
			const opennova::particle::ParticleDef &definition =
					(*definitions)[definition_index];
			DefinitionVisual &visual = definition_visuals[definition_index];
			bool any_present = false;
			for (int layer_index = 0; layer_index < kGraphicLayerCount;
					++layer_index) {
				const GraphicLayer &graphic =
						definition.graphics[static_cast<std::size_t>(layer_index)];
				if (!graphic.present)
					continue;
				any_present = true;
				LayerVisual &layer =
						visual.layers[static_cast<std::size_t>(layer_index)];
				layer.present = true;
				layer.blend = graphic.blend_mode;
				layer.flip_frames = std::clamp(graphic.flip_frames, 1,
						opennova::particle::kMaxParticleFlipFrames);
				layer.flip_rate = std::max(0, graphic.flip_rate);
				frames.clear();
				for (int frame = 1; frame <= layer.flip_frames; ++frame) {
					frames.push_back(frame_image(
							opennova::particle::retail_particle_frame_name(
									graphic.texture, layer.flip_frames, frame)));
				}
				if (blend_is_normal_mapped(layer.blend)) {
					layer.normal_strip = build_strip(frames);
					albedo_frames.clear();
					for (const Ref<Image> &frame : frames)
						albedo_frames.push_back(white_albedo_from(frame));
					layer.albedo_strip = build_strip(albedo_frames);
				} else {
					layer.albedo_strip = build_strip(frames);
				}
				layer.resolved = layer.albedo_strip.is_valid();
			}
			if (!any_present) {
				LayerVisual &layer = visual.layers[0];
				layer.present = true;
				frames.clear();
				frames.push_back(frame_image(std::string()));
				layer.albedo_strip = build_strip(frames);
				layer.resolved = layer.albedo_strip.is_valid();
			}
		}
	}

	Ref<StandardMaterial3D> material_for(std::size_t definition_index,
			int layer_index) {
		const std::size_t key = definition_index *
				static_cast<std::size_t>(kGraphicLayerCount) +
				static_cast<std::size_t>(layer_index);
		const auto found = materials.find(key);
		if (found != materials.end())
			return found->second;
		if (definition_index >= definition_visuals.size() || !catalog_definitions)
			return Ref<StandardMaterial3D>();
		const LayerVisual &layer = definition_visuals[definition_index]
				.layers[static_cast<std::size_t>(layer_index)];
		const opennova::particle::ParticleDef &definition =
				(*catalog_definitions)[definition_index];

		Ref<StandardMaterial3D> material;
		material.instantiate();
		material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		material->set_flag(BaseMaterial3D::FLAG_DONT_RECEIVE_SHADOWS, true);
		material->set_specular(0.0f);
		material->set_roughness(1.0f);
		if (layer.albedo_strip.is_valid())
			material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO,
					layer.albedo_strip);

		const bool world_oriented = (definition.flags &
				opennova::particle::particle_flag::YawAndPitch) != 0;
		if (!world_oriented) {
			material->set_billboard_mode(BaseMaterial3D::BILLBOARD_PARTICLES);
			material->set_flag(BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE, true);
			material->set_particles_anim_h_frames(std::max(1, layer.flip_frames));
			material->set_particles_anim_v_frames(1);
			material->set_particles_anim_loop(true);
		} else if (layer.flip_frames > 1) {
			// World-oriented quads have no particle billboard path to index the
			// flipbook; they hold the strip's first frame.
			material->set_uv1_scale(Vector3(
					1.0f / static_cast<float>(layer.flip_frames), 1.0f, 1.0f));
		}

		bool lit = false;
		bool fog_disabled = false;
		bool soft = true;
		switch (layer.blend) {
			case BlendMode::Blend:
				material->set_blend_mode(BaseMaterial3D::BLEND_MODE_MIX);
				break;
			case BlendMode::Additive:
				material->set_blend_mode(BaseMaterial3D::BLEND_MODE_ADD);
				fog_disabled = true;
				break;
			case BlendMode::Premult:
				material->set_blend_mode(
						BaseMaterial3D::BLEND_MODE_PREMULT_ALPHA);
				break;
			case BlendMode::Mod:
			case BlendMode::Mod2x:
				// Zero authored uses across the retail corpus; blend_mul keeps
				// the class presentable without a dedicated pipeline.
				material->set_blend_mode(BaseMaterial3D::BLEND_MODE_MUL);
				fog_disabled = true;
				soft = false;
				break;
			case BlendMode::Bump:
				material->set_blend_mode(BaseMaterial3D::BLEND_MODE_MIX);
				lit = true;
				break;
			case BlendMode::Bumpadd:
				material->set_blend_mode(BaseMaterial3D::BLEND_MODE_ADD);
				lit = true;
				fog_disabled = true;
				break;
			case BlendMode::Distort:
				// The authored distortion texture perturbs the screen behind
				// the quad through Godot's refraction path.
				material->set_blend_mode(BaseMaterial3D::BLEND_MODE_MIX);
				material->set_albedo(Color(1.0f, 1.0f, 1.0f, 0.0f));
				if (layer.normal_strip.is_valid()) {
					material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING,
							true);
					material->set_texture(BaseMaterial3D::TEXTURE_NORMAL,
							layer.normal_strip);
				}
				material->set_feature(BaseMaterial3D::FEATURE_REFRACTION, true);
				material->set_refraction(kDistortRefractionScale);
				fog_disabled = true;
				soft = false;
				break;
		}
		if (lit) {
			// The lit-smoke classes: scene lighting replaces the packed
			// per-vertex light retail computed from the view basis; the
			// authored texture is the normal map it always was.
			material->set_shading_mode(BaseMaterial3D::SHADING_MODE_PER_PIXEL);
			if (layer.normal_strip.is_valid()) {
				material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING,
						true);
				material->set_texture(BaseMaterial3D::TEXTURE_NORMAL,
						layer.normal_strip);
				material->set_normal_scale(
						std::clamp(definition.bump_scale, -16.0f, 16.0f));
			}
		} else if (layer.blend != BlendMode::Distort) {
			material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		}
		if (fog_disabled)
			material->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
		if (soft) {
			material->set_proximity_fade_enabled(true);
			material->set_proximity_fade_distance(kSoftParticleFadeDistance);
		}
		materials.emplace(key, material);
		return material;
	}

	void warm_materials() {
		if (!catalog_definitions)
			return;
		for (std::size_t definition_index = 0;
				definition_index < definition_visuals.size();
				++definition_index) {
			const DefinitionVisual &visual = definition_visuals[definition_index];
			for (int layer_index = 0; layer_index < kGraphicLayerCount;
					++layer_index) {
				if (visual.layers[static_cast<std::size_t>(layer_index)].present)
					material_for(definition_index, layer_index);
			}
		}
	}

	// Presents every live emitter from the fixed-tick snapshot. The witnessed
	// per-particle decode is unchanged from the retail port: curve LUT color/
	// alpha/scale multipliers, the flipbook clock, spawn-sampled colors, the
	// LOD divisor, and the camera-ward z_offset pull.
	void present_frame(ParticleRenderer *owner,
			const opennova::particle::ParticleFrameSnapshot &frame,
			const Vector3 &camera_position) {
		pool_used = 0;
		rendered_quads = 0;
		draw_groups = 0;
		++present_index;
		emitter_reports.clear();
		if (!frame.definitions ||
				definition_visuals.size() != frame.definitions->size()) {
			hide_pool_tail(0);
			return;
		}

		for (const opennova::particle::EffectEmitterFrameSnapshot &source_emitter :
				frame.emitters) {
			if (source_emitter.definition_index >= frame.definitions->size())
				continue;
			const opennova::particle::ParticleDef &definition =
					(*frame.definitions)[source_emitter.definition_index];
			const DefinitionVisual &visual =
					definition_visuals[source_emitter.definition_index];
			const bool first_person =
					source_emitter.group_index < frame.groups.size() &&
					frame.groups[source_emitter.group_index].render_domain ==
							opennova::particle::EffectRenderDomain::FirstPerson;
			const bool world_oriented = (definition.flags &
					opennova::particle::particle_flag::YawAndPitch) != 0;

			int fallback_layer = 0;
			for (int layer = 0; layer < kGraphicLayerCount; ++layer) {
				if (visual.layers[static_cast<std::size_t>(layer)].present) {
					fallback_layer = layer;
					break;
				}
			}

			const std::size_t first = std::min(source_emitter.first_particle,
					frame.particles.size());
			const std::size_t available = frame.particles.size() - first;
			const std::size_t count = std::min(source_emitter.particle_count,
					available);
			const std::uint32_t lod_divisor =
					std::max<std::uint32_t>(1u, source_emitter.lod_divisor);

			for (auto &group : layer_groups)
				group.clear();
			EmitterBoundsReport report;
			report.emitter_id = source_emitter.id;
			report.first_person = first_person;
			AABB bounds;
			bool bounds_valid = false;
			for (std::size_t particle_index = 0; particle_index < count;
					++particle_index) {
				const Particle &particle = frame.particles[first + particle_index];
				const Vector3 position(particle.position.x, particle.position.y,
						particle.position.z);
				const float radius = std::max(particle.size * 0.5f, 0.001f);
				const AABB particle_box(position - Vector3(radius, radius, radius),
						Vector3(radius, radius, radius) * 2.0f);
				bounds = bounds_valid ? bounds.merge(particle_box) : particle_box;
				bounds_valid = true;
				if (lod_divisor > 1u &&
						(static_cast<std::uint32_t>(particle.serial) %
								lod_divisor) != 0u)
					continue;
				int layer_index = std::clamp<int>(
						static_cast<int>(particle.graphic_layer), 0,
						kGraphicLayerCount - 1);
				if (!visual.layers[static_cast<std::size_t>(layer_index)].present)
					layer_index = fallback_layer;
				if (!visual.layers[static_cast<std::size_t>(layer_index)].resolved)
					continue;
				layer_groups[static_cast<std::size_t>(layer_index)].push_back(
						first + particle_index);
			}
			report.bounds = bounds;
			report.bounds_valid = bounds_valid;

			const Vector3 emitter_origin(source_emitter.position.x,
					source_emitter.position.y, source_emitter.position.z);
			for (int layer_index = 0; layer_index < kGraphicLayerCount;
					++layer_index) {
				std::vector<std::size_t> &group =
						layer_groups[static_cast<std::size_t>(layer_index)];
				if (group.empty())
					continue;
				const LayerVisual &layer =
						visual.layers[static_cast<std::size_t>(layer_index)];
				const GraphicLayer &graphic =
						definition.graphics[static_cast<std::size_t>(layer_index)];
				Ref<StandardMaterial3D> material = material_for(
						source_emitter.definition_index, layer_index);
				if (material.is_null())
					continue;

				// Intra-emitter transparency: back-to-front in instance order
				// (retail's per-leaf view-depth quicksort intent; cross-emitter
				// order falls to Godot's per-instance transparent sort keyed on
				// the emitter origin below).
				sort_scratch.clear();
				for (const std::size_t particle_slot : group) {
					const Particle &particle = frame.particles[particle_slot];
					const Vector3 position(particle.position.x,
							particle.position.y, particle.position.z);
					sort_scratch.emplace_back(
							-static_cast<float>(
									position.distance_squared_to(camera_position)),
							particle_slot);
				}
				std::sort(sort_scratch.begin(), sort_scratch.end(),
						[](const auto &a, const auto &b) {
							return a.first < b.first;
						});

				PoolEntry &entry = acquire_pool_entry(owner);
				const int needed = static_cast<int>(sort_scratch.size());
				if (needed > entry.capacity) {
					entry.capacity = std::max(needed, entry.capacity * 2);
					entry.multimesh->set_instance_count(entry.capacity);
					entry.buffer.resize(entry.capacity * kFloatsPerInstance);
				}
				float *buffer = entry.buffer.ptrw();
				int written = 0;
				for (const auto &[sort_key, particle_slot] : sort_scratch) {
					const Particle &particle = frame.particles[particle_slot];
					const float phase = std::max(0.0f, particle.curve_phase);
					const int lut_index = static_cast<int>(phase) & 0xff;
					const float lut_fraction = phase - std::floor(phase);
					auto channel_multiplier = [&](std::uint32_t flag,
							const CurveRef &graphic_curve,
							const CurveRef &definition_curve) -> float {
						if ((particle.flags & flag) == 0)
							return 1.0f;
						const CurveRef &curve = graphic_curve.baked ?
								graphic_curve : definition_curve;
						if (!curve.baked)
							return 1.0f;
						return static_cast<float>(curve.baked_lut[
								static_cast<std::size_t>(lut_index)]) / 256.0f;
					};

					using namespace opennova::particle::particle_runtime_flag;
					const float alpha_multiplier = channel_multiplier(
							AlphaCurve, graphic.alpha_func, definition.alpha_func);
					const float red_multiplier = channel_multiplier(
							RedCurve, graphic.red_func, definition.red_func);
					const float green_multiplier = channel_multiplier(
							GreenCurve, graphic.green_func, definition.green_func);
					const float blue_multiplier = channel_multiplier(
							BlueCurve, graphic.blue_func, definition.blue_func);

					float scale_multiplier = 1.0f;
					if ((particle.flags & ScaleCurve) != 0) {
						const CurveRef &curve = graphic.scale_func.baked ?
								graphic.scale_func : definition.scale_func;
						if (curve.baked) {
							const std::uint8_t first_sample = curve.baked_lut[
									static_cast<std::size_t>(lut_index)];
							const std::uint8_t second_sample = curve.baked_lut[
									static_cast<std::size_t>(
											std::min(lut_index + 1, 255))];
							const float interpolated =
									static_cast<float>(first_sample) +
									(static_cast<float>(second_sample) -
											static_cast<float>(first_sample)) *
											lut_fraction;
							scale_multiplier = interpolated / 128.0f;
						}
					}
					const float size = std::max(
							particle.size * scale_multiplier, 0.0001f);

					constexpr float byte_to_unit = 1.0f / 255.0f;
					const float red = std::clamp(
							static_cast<float>(particle.color.r) * byte_to_unit *
									red_multiplier * source_emitter.color_tint.x,
							0.0f, 1.0f);
					const float green = std::clamp(
							static_cast<float>(particle.color.g) * byte_to_unit *
									green_multiplier * source_emitter.color_tint.y,
							0.0f, 1.0f);
					const float blue = std::clamp(
							static_cast<float>(particle.color.b) * byte_to_unit *
									blue_multiplier * source_emitter.color_tint.z,
							0.0f, 1.0f);
					const float alpha = std::clamp(
							static_cast<float>(particle.alpha) * byte_to_unit *
									alpha_multiplier,
							0.0f, 1.0f);

					// Flipbook clock: frame = int(4 * flip_rate * elapsed
					// seconds) wrapped over the frame count, with GFXFLIPRAND
					// offsetting by a serial-derived start frame [orig:
					// CParticleEmitter_BuildBillboardQuads @ 0x5e6f17 —
					// (256 / phase_rate) * flip_rate * phase / 64].
					int frame_index = 0;
					if (layer.flip_frames > 1 && layer.flip_rate > 0) {
						int random_offset = 0;
						if ((definition.flags &
								opennova::particle::particle_flag::GfxFlipRand) != 0) {
							random_offset = static_cast<int>(particle.serial * 9u) %
									layer.flip_frames;
						}
						const float elapsed = particle.phase_rate > 0.0f ?
								phase / particle.phase_rate : 0.0f;
						frame_index = (random_offset + static_cast<int>(
								4.0f * static_cast<float>(layer.flip_rate) *
										elapsed)) %
								layer.flip_frames;
					}

					Vector3 center(particle.position.x, particle.position.y,
							particle.position.z);
					// Camera-ward pull [orig: the quad center z_offset shift in
					// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60].
					if (definition.z_offset != 0.0f) {
						const Vector3 to_camera = camera_position - center;
						const float distance = to_camera.length();
						if (distance > 0.0001f) {
							center += to_camera * (std::min(definition.z_offset,
									distance) / distance);
						}
					}

					Basis basis;
					if (world_oriented) {
						basis = Basis::from_euler(Vector3(particle.pitch,
								particle.yaw, particle.rotation));
					}
					basis = basis.scaled(Vector3(size, size, size));
					const Vector3 local = center - emitter_origin;

					float *row = buffer + written * kFloatsPerInstance;
					row[0] = basis.rows[0][0];
					row[1] = basis.rows[0][1];
					row[2] = basis.rows[0][2];
					row[3] = local.x;
					row[4] = basis.rows[1][0];
					row[5] = basis.rows[1][1];
					row[6] = basis.rows[1][2];
					row[7] = local.y;
					row[8] = basis.rows[2][0];
					row[9] = basis.rows[2][1];
					row[10] = basis.rows[2][2];
					row[11] = local.z;
					row[12] = red;
					row[13] = green;
					row[14] = blue;
					row[15] = alpha;
					// BILLBOARD_PARTICLES consumes CUSTOM.x as roll and
					// CUSTOM.z as the flipbook phase.
					row[16] = particle.rotation;
					row[17] = 0.0f;
					row[18] = layer.flip_frames > 1 ?
							(static_cast<float>(frame_index) + 0.5f) /
									static_cast<float>(layer.flip_frames) :
							0.0f;
					row[19] = 0.0f;
					++written;
				}
				for (int extra = written; extra < entry.capacity; ++extra) {
					float *row = buffer + extra * kFloatsPerInstance;
					std::memset(row, 0, sizeof(float) *
							static_cast<std::size_t>(kFloatsPerInstance));
				}
				entry.multimesh->set_buffer(entry.buffer);
				entry.multimesh->set_visible_instance_count(written);
				if (bounds_valid) {
					AABB local_bounds(bounds.position - emitter_origin,
							bounds.size);
					entry.multimesh->set_custom_aabb(local_bounds.grow(1.0f));
				}
				entry.instance->set_material_override(material);
				entry.instance->set_layer_mask(first_person ?
						kFirstPersonVisibilityMask : kWorldVisibilityMask);
				entry.instance->set_global_transform(
						Transform3D(Basis(), emitter_origin));
				entry.instance->set_visible(written > 0);
				rendered_quads += written;
				report.quad_count += static_cast<std::uint32_t>(written);
				++draw_groups;
			}
			emitter_reports.push_back(report);
		}
		hide_pool_tail(pool_used);
	}
};

ParticleRenderer::ParticleRenderer() : impl_(std::make_unique<Impl>()) {}

ParticleRenderer::~ParticleRenderer() = default;

void ParticleRenderer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("warm_pipelines", "position"),
			&ParticleRenderer::warm_pipelines);
	ClassDB::bind_method(D_METHOD("clear_warm_pipelines"),
			&ParticleRenderer::clear_warm_pipelines);
	ClassDB::bind_method(D_METHOD("set_scene", "scene"),
			&ParticleRenderer::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"),
			&ParticleRenderer::get_scene);
	ClassDB::bind_method(D_METHOD("set_texture_provider", "provider"),
			&ParticleRenderer::set_texture_provider);
	ClassDB::bind_method(D_METHOD("get_texture_provider"),
			&ParticleRenderer::get_texture_provider);
	ClassDB::bind_method(D_METHOD("set_texture_dir", "texture_dir"),
			&ParticleRenderer::set_texture_dir);
	ClassDB::bind_method(D_METHOD("get_texture_dir"),
			&ParticleRenderer::get_texture_dir);
	ClassDB::bind_method(D_METHOD("set_hidden", "hidden"),
			&ParticleRenderer::set_hidden);
	ClassDB::bind_method(D_METHOD("get_hidden"),
			&ParticleRenderer::get_hidden);
	ClassDB::bind_method(D_METHOD("set_procedural_fallback_enabled", "enabled"),
			&ParticleRenderer::set_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("get_procedural_fallback_enabled"),
			&ParticleRenderer::get_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("render_now"),
			&ParticleRenderer::render_now);
	ClassDB::bind_method(D_METHOD("shutdown"), &ParticleRenderer::shutdown);
	ClassDB::bind_method(D_METHOD("get_rendered_quad_count"),
			&ParticleRenderer::get_rendered_quad_count);
	ClassDB::bind_method(D_METHOD("get_draw_command_count"),
			&ParticleRenderer::get_draw_command_count);
	ClassDB::bind_method(D_METHOD("get_debug_draw_list_report"),
			&ParticleRenderer::get_debug_draw_list_report);
	ClassDB::bind_method(D_METHOD("get_debug_emitter_bounds"),
			&ParticleRenderer::get_debug_emitter_bounds);
	ClassDB::bind_method(D_METHOD("get_unresolved_texture_names"),
			&ParticleRenderer::get_unresolved_texture_names);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "scene"),
			"set_scene", "get_scene");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "texture_provider"),
			"set_texture_provider", "get_texture_provider");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "texture_dir", PROPERTY_HINT_DIR),
			"set_texture_dir", "get_texture_dir");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hidden"),
			"set_hidden", "get_hidden");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "procedural_fallback_enabled"),
			"set_procedural_fallback_enabled",
			"get_procedural_fallback_enabled");
}

void ParticleRenderer::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		set_process(false);
		render_now();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		shutdown();
	}
}

void ParticleRenderer::_invalidate_catalog() {
	if (impl_)
		impl_->invalidate_catalog();
}

void ParticleRenderer::set_scene(const Ref<EffectScene> &p_scene) {
	if (scene_ == p_scene)
		return;
	scene_ = p_scene;
	_invalidate_catalog();
	if (scene_.is_null() && impl_)
		impl_->clear_draws();
}

Ref<EffectScene> ParticleRenderer::get_scene() const {
	return scene_;
}

void ParticleRenderer::set_texture_provider(const Callable &p_provider) {
	texture_provider_ = p_provider;
	_invalidate_catalog();
}

Callable ParticleRenderer::get_texture_provider() const {
	return texture_provider_;
}

void ParticleRenderer::set_texture_dir(const String &p_texture_dir) {
	if (texture_dir_ == p_texture_dir)
		return;
	texture_dir_ = p_texture_dir;
	_invalidate_catalog();
}

String ParticleRenderer::get_texture_dir() const {
	return texture_dir_;
}

void ParticleRenderer::warm_pipelines(const Vector3 &p_position) {
	(void)p_position;
	if (!impl_ || scene_.is_null())
		return;
	const opennova::particle::ParticleFrameSnapshot &frame =
			scene_->native_frame_snapshot();
	if (impl_->catalog_dirty ||
			impl_->catalog_definitions.get() != frame.definitions.get()) {
		impl_->rebuild_catalog(frame.definitions, texture_provider_,
				texture_dir_, procedural_fallback_enabled_);
	}
	impl_->warm_materials();
}

void ParticleRenderer::clear_warm_pipelines() {
}

void ParticleRenderer::shutdown() {
	if (impl_)
		impl_->clear_draws();
}

void ParticleRenderer::set_hidden(bool p_hidden) {
	if (hidden_ == p_hidden)
		return;
	hidden_ = p_hidden;
	if (hidden_ && impl_)
		impl_->clear_draws();
}

bool ParticleRenderer::get_hidden() const {
	return hidden_;
}

void ParticleRenderer::set_procedural_fallback_enabled(bool p_enabled) {
	if (procedural_fallback_enabled_ == p_enabled)
		return;
	procedural_fallback_enabled_ = p_enabled;
	_invalidate_catalog();
}

bool ParticleRenderer::get_procedural_fallback_enabled() const {
	return procedural_fallback_enabled_;
}

void ParticleRenderer::render_now() {
	if (!impl_)
		return;
	if (hidden_) {
		impl_->clear_draws();
		return;
	}
	if (scene_.is_null()) {
		impl_->clear_draws();
		return;
	}
	if (!is_inside_tree())
		return;
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	const Vector3 camera_position = camera != nullptr ?
			camera->get_camera_transform().origin : Vector3();

	const opennova::particle::ParticleFrameSnapshot &frame =
			scene_->native_frame_snapshot();
	if (impl_->catalog_dirty ||
			impl_->catalog_definitions.get() != frame.definitions.get()) {
		impl_->rebuild_catalog(frame.definitions, texture_provider_,
				texture_dir_, procedural_fallback_enabled_);
	}
	impl_->present_frame(this, frame, camera_position);
}

int64_t ParticleRenderer::get_rendered_quad_count() const {
	return impl_ ? impl_->rendered_quads : 0;
}

int64_t ParticleRenderer::get_draw_command_count() const {
	return impl_ ? impl_->draw_groups : 0;
}

Dictionary ParticleRenderer::get_debug_draw_list_report() const {
	Dictionary result;
	if (!impl_)
		return result;
	result["presenter"] = "multimesh_scene";
	result["present_index"] = impl_->present_index;
	result["rendered_quad_count"] = impl_->rendered_quads;
	result["draw_command_count"] = impl_->draw_groups;
	result["pool_size"] = static_cast<int64_t>(impl_->pool.size());
	result["pool_used"] = static_cast<int64_t>(impl_->pool_used);
	result["material_count"] = static_cast<int64_t>(impl_->materials.size());
	result["unresolved_texture_count"] =
			static_cast<int64_t>(impl_->unresolved_names.size());
	result["hidden"] = hidden_;
	result["procedural_fallback_enabled"] = procedural_fallback_enabled_;
	return result;
}

Array ParticleRenderer::get_debug_emitter_bounds() const {
	Array result;
	if (!impl_)
		return result;
	for (const EmitterBoundsReport &report : impl_->emitter_reports) {
		Dictionary value;
		int64_t token = 0;
		std::memcpy(&token, &report.emitter_id, sizeof(token));
		value["emitter_id"] = token;
		value["render_domain"] = report.first_person ? 1 : 0;
		value["quad_count"] = static_cast<int64_t>(report.quad_count);
		value["bounds_valid"] = report.bounds_valid;
		value["bounds"] = report.bounds_valid ? report.bounds : AABB();
		result.push_back(value);
	}
	return result;
}

PackedStringArray ParticleRenderer::get_unresolved_texture_names() const {
	PackedStringArray result;
	if (!impl_)
		return result;
	result.resize(static_cast<int64_t>(impl_->unresolved_names.size()));
	for (std::size_t i = 0; i < impl_->unresolved_names.size(); ++i)
		result[static_cast<int64_t>(i)] =
				String::utf8(impl_->unresolved_names[i].c_str());
	return result;
}
