#include "nova_particle_emitter.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include "util/texture_path_resolver.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace godot;

namespace {

Ref<NovaParticleCurveRef> choose_curve(const Ref<NovaParticleCurveRef> &primary,
		const Ref<NovaParticleCurveRef> &fallback) {
	if (primary.is_valid() && primary->get_present()) {
		return primary;
	}
	return fallback;
}

struct RenderParticle {
	int layer_idx = 0;
	float depth = 0.0f;
	float rotation = 0.0f;
	float scale = 1.0f;
	int frame = 0;
	int flip_frames = 1;
	int blend_mode = 0;
	Vector3 position;
	Color color;
	// Engine's `D3DFVF_DIFFUSE` slot becomes lit color when
	// `particle.flags & 0x80` (LitColor) is set in
	// `BuildBillboardQuads @ 0x5e6d60`. Default white = no tint;
	// shader multiplies texture × color × lit_color (matches engine
	// `texture × DIFFUSE × SPECULAR` combiner intent for Bump mode).
	Color lit_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
};

} // namespace

NovaParticleEmitter::NovaParticleEmitter() {
	emitter.def = nullptr;
}

NovaParticleEmitter::~NovaParticleEmitter() = default;

void NovaParticleEmitter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_def", "p_def"), &NovaParticleEmitter::set_def);
	ClassDB::bind_method(D_METHOD("get_def"), &NovaParticleEmitter::get_def);
	ClassDB::bind_method(D_METHOD("set_tables", "p_tables"), &NovaParticleEmitter::set_tables);
	ClassDB::bind_method(D_METHOD("get_tables"), &NovaParticleEmitter::get_tables);
	ClassDB::bind_method(D_METHOD("set_seed", "p_seed"), &NovaParticleEmitter::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &NovaParticleEmitter::get_seed);
	ClassDB::bind_method(D_METHOD("set_auto_advance", "p_value"), &NovaParticleEmitter::set_auto_advance);
	ClassDB::bind_method(D_METHOD("get_auto_advance"), &NovaParticleEmitter::get_auto_advance);
	ClassDB::bind_method(D_METHOD("set_time_scale", "p_value"), &NovaParticleEmitter::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &NovaParticleEmitter::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_texture_dir", "p_dir"), &NovaParticleEmitter::set_texture_dir);
	ClassDB::bind_method(D_METHOD("get_texture_dir"), &NovaParticleEmitter::get_texture_dir);
	ClassDB::bind_method(D_METHOD("get_resolved_texture_path", "layer_index"),
			&NovaParticleEmitter::get_resolved_texture_path);
	ClassDB::bind_method(D_METHOD("set_color_tint", "p_tint"), &NovaParticleEmitter::set_color_tint);
	ClassDB::bind_method(D_METHOD("get_color_tint"), &NovaParticleEmitter::get_color_tint);
	ClassDB::bind_method(D_METHOD("set_spring_const", "p_value"), &NovaParticleEmitter::set_spring_const);
	ClassDB::bind_method(D_METHOD("get_spring_const"), &NovaParticleEmitter::get_spring_const);
	ClassDB::bind_method(D_METHOD("set_lod_divisor", "p_value"), &NovaParticleEmitter::set_lod_divisor);
	ClassDB::bind_method(D_METHOD("get_lod_divisor"), &NovaParticleEmitter::get_lod_divisor);
	ClassDB::bind_method(D_METHOD("set_kill_plane_mode", "p_value"), &NovaParticleEmitter::set_kill_plane_mode);
	ClassDB::bind_method(D_METHOD("get_kill_plane_mode"), &NovaParticleEmitter::get_kill_plane_mode);
	ClassDB::bind_method(D_METHOD("set_kill_plane_y", "p_value"), &NovaParticleEmitter::set_kill_plane_y);
	ClassDB::bind_method(D_METHOD("get_kill_plane_y"), &NovaParticleEmitter::get_kill_plane_y);

	ClassDB::bind_method(D_METHOD("play"), &NovaParticleEmitter::play);
	ClassDB::bind_method(D_METHOD("stop"), &NovaParticleEmitter::stop);
	ClassDB::bind_method(D_METHOD("restart"), &NovaParticleEmitter::restart);
	ClassDB::bind_method(D_METHOD("advance", "dt"), &NovaParticleEmitter::advance);
	ClassDB::bind_method(D_METHOD("is_finite"), &NovaParticleEmitter::is_finite);
	ClassDB::bind_method(D_METHOD("is_finished"), &NovaParticleEmitter::is_finished);
	ClassDB::bind_method(D_METHOD("get_alive_count"), &NovaParticleEmitter::get_alive_count);
	ClassDB::bind_method(D_METHOD("get_visual_layer_count"), &NovaParticleEmitter::get_visual_layer_count);
	ClassDB::bind_method(D_METHOD("get_rendered_instance_count"), &NovaParticleEmitter::get_rendered_instance_count);
	ClassDB::bind_method(D_METHOD("get_textured_layer_count"), &NovaParticleEmitter::get_textured_layer_count);
	ClassDB::bind_method(D_METHOD("get_render_batch_count"), &NovaParticleEmitter::get_render_batch_count);
	ClassDB::bind_method(D_METHOD("get_sorted_depth_count"), &NovaParticleEmitter::get_sorted_depth_count);
	ClassDB::bind_method(D_METHOD("get_debug_first_rotation"), &NovaParticleEmitter::get_debug_first_rotation);
	ClassDB::bind_method(D_METHOD("get_debug_first_flip_frame"), &NovaParticleEmitter::get_debug_first_flip_frame);
	ClassDB::bind_method(D_METHOD("get_debug_first_blend_mode"), &NovaParticleEmitter::get_debug_first_blend_mode);
	ClassDB::bind_method(D_METHOD("get_debug_static_billboard"), &NovaParticleEmitter::get_debug_static_billboard);
	ClassDB::bind_method(D_METHOD("get_debug_first_shader_path"),
			&NovaParticleEmitter::get_debug_first_shader_path);
	ClassDB::bind_method(D_METHOD("get_debug_first_color"),
			&NovaParticleEmitter::get_debug_first_color);
	ClassDB::bind_method(D_METHOD("get_debug_first_lit_color"),
			&NovaParticleEmitter::get_debug_first_lit_color);
	ClassDB::bind_method(D_METHOD("get_debug_last_translation_delta"),
			&NovaParticleEmitter::get_debug_last_translation_delta);
	ClassDB::bind_method(D_METHOD("get_debug_first_layer_aabb_center"),
			&NovaParticleEmitter::get_debug_first_layer_aabb_center);
	ClassDB::bind_method(D_METHOD("get_debug_first_quad_vertices"),
			&NovaParticleEmitter::get_debug_first_quad_vertices);
	ClassDB::bind_method(D_METHOD("get_debug_atlas_texture"),
			&NovaParticleEmitter::get_debug_atlas_texture);
	ClassDB::bind_method(D_METHOD("get_debug_layer_material", "layer_index"),
			&NovaParticleEmitter::get_debug_layer_material);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "def", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleDef"),
			"set_def", "get_def");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "tables", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":NovaParticleTable"),
			"set_tables", "get_tables");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_advance"), "set_auto_advance", "get_auto_advance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale"), "set_time_scale", "get_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "texture_dir", PROPERTY_HINT_DIR), "set_texture_dir", "get_texture_dir");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color_tint"), "set_color_tint", "get_color_tint");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spring_const"), "set_spring_const", "get_spring_const");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_divisor",
			PROPERTY_HINT_RANGE, "1,16,1"),
			"set_lod_divisor", "get_lod_divisor");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "kill_plane_mode",
			PROPERTY_HINT_ENUM, "Disabled,Kill Above,Kill At/Below"),
			"set_kill_plane_mode", "get_kill_plane_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "kill_plane_y"),
			"set_kill_plane_y", "get_kill_plane_y");
}

void NovaParticleEmitter::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
			set_notify_transform(true);
			_ensure_visual_setup();
			if (def.is_valid()) {
				_refresh_emitter();
			}
			set_process(true);
			break;
		case NOTIFICATION_PROCESS: {
			if (!playing || !auto_advance) {
				return;
			}
			advance(static_cast<float>(get_process_delta_time()) * time_scale);
			break;
		}
		case NOTIFICATION_TRANSFORM_CHANGED: {
			// Engine: CParticleEmitter_TranslatePosition @ 0x5efe90 — when
			// the emitter's world origin moves, push the new position into
			// the simulator so the delta accumulators stay in sync. Note
			// that the rendering pipeline still applies global_transform at
			// vertex emission, so this currently only feeds the delta
			// metadata; switching the renderer to world-space coordinates
			// is a follow-up gated on `PositionRelative` flag handling.
			if (emitter.def != nullptr) {
				const Vector3 origin = get_global_transform().origin;
				opennova::particle::emitter_translate(emitter,
						opennova::particle::Vec3{origin.x, origin.y, origin.z});
			}
			break;
		}
		default:
			break;
	}
}

void NovaParticleEmitter::_ensure_visual_setup() {
	_ensure_fallback_texture();
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (mesh_layers[i] == nullptr) {
			mesh_layers[i] = memnew(MeshInstance3D);
			mesh_layers[i]->set_name(String("ParticleLayer") + String::num_int64(i + 1));
			mesh_layers[i]->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			mesh_layers[i]->set_extra_cull_margin(200.0f);
			// World-space rendering: vertex positions are already in world
			// coords (simulator runs in world space, seeded from the
			// NovaParticleEmitter's global origin). `set_as_top_level(true)`
			// makes the MeshInstance3D ignore the parent transform so the
			// vertices are interpreted directly as world coordinates,
			// matching the engine default where particles are "left behind"
			// when the emitter Node3D moves. PositionRelative flag (bit 18)
			// opts back into local-space behaviour at the simulator level
			// via `emitter_translate` carrying alive particles.
			mesh_layers[i]->set_as_top_level(true);
			mesh_layers[i]->set_transform(Transform3D());
			add_child(mesh_layers[i]);
			mesh_layers[i]->set_owner(get_owner());
		}
		if (layer_materials[i].is_null()) {
			layer_materials[i] = _make_layer_material(layer_blend_modes[i]);
		}
		mesh_layers[i]->set_material_override(layer_materials[i]);
	}
}

void NovaParticleEmitter::_ensure_fallback_texture() {
	if (fallback_texture.is_valid()) {
		return;
	}
	constexpr int FALLBACK_SIZE = 32;
	Ref<Image> image = Image::create(FALLBACK_SIZE, FALLBACK_SIZE, false, Image::FORMAT_RGBA8);
	if (image.is_null()) {
		return;
	}
	const float center = (static_cast<float>(FALLBACK_SIZE) - 1.0f) * 0.5f;
	for (int y = 0; y < FALLBACK_SIZE; ++y) {
		for (int x = 0; x < FALLBACK_SIZE; ++x) {
			const float dx = (static_cast<float>(x) - center) / center;
			const float dy = (static_cast<float>(y) - center) / center;
			const float r = std::sqrt(dx * dx + dy * dy);
			const float alpha = std::clamp(1.0f - r, 0.0f, 1.0f);
			image->set_pixel(x, y, Color(1.0f, 1.0f, 1.0f, alpha));
		}
	}
	fallback_texture = ImageTexture::create_from_image(image);
}

String NovaParticleEmitter::_shader_path_for_blend(int p_blend_mode) {
	using opennova::particle::BlendMode;
	switch (static_cast<BlendMode>(std::clamp(p_blend_mode, 0, 7))) {
		case BlendMode::Blend:    return "res://modtools/particle/shaders/particle_blend_blend.gdshader";
		case BlendMode::Additive: return "res://modtools/particle/shaders/particle_blend_additive.gdshader";
		case BlendMode::Premult:  return "res://modtools/particle/shaders/particle_blend_premult.gdshader";
		case BlendMode::Bump:     return "res://modtools/particle/shaders/particle_blend_bump.gdshader";
		case BlendMode::Mod:      return "res://modtools/particle/shaders/particle_blend_mod.gdshader";
		case BlendMode::Mod2x:    return "res://modtools/particle/shaders/particle_blend_mod2x.gdshader";
		case BlendMode::Bumpadd:  return "res://modtools/particle/shaders/particle_blend_bumpadd.gdshader";
		case BlendMode::Distort:  return "res://modtools/particle/shaders/particle_blend_distort.gdshader";
	}
	return "res://modtools/particle/shaders/particle_blend_blend.gdshader";
}

Ref<Shader> NovaParticleEmitter::_get_blend_shader(int p_blend_mode) {
	// Per-emitter cache. Lazy-load each blend mode on first use; the Ref<>s
	// release with the emitter (via _clear_meshes / dtor), so the
	// RenderingServer is still alive when shader RIDs are freed even at
	// engine shutdown.
	const int idx = std::clamp(p_blend_mode, 0, 7);
	if (blend_shader_cache[static_cast<std::size_t>(idx)].is_null()) {
		ResourceLoader *loader = ResourceLoader::get_singleton();
		if (loader != nullptr) {
			Ref<Resource> res = loader->load(_shader_path_for_blend(idx));
			blend_shader_cache[static_cast<std::size_t>(idx)] = res;
		}
	}
	return blend_shader_cache[static_cast<std::size_t>(idx)];
}

Ref<ShaderMaterial> NovaParticleEmitter::_make_layer_material(int p_blend_mode) {
	Ref<ShaderMaterial> mat;
	mat.instantiate();
	mat->set_shader(_get_blend_shader(p_blend_mode));
	mat->set_shader_parameter("has_texture", false);
	return mat;
}

void NovaParticleEmitter::_clear_meshes() {
	last_render_batch_count = 0;
	last_sorted_depth_count = 0;
	debug_first_rotation = 0.0f;
	debug_first_flip_frame = 0;
	debug_first_blend_mode = 0;
	debug_static_billboard = false;
	debug_first_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
	debug_first_lit_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
	debug_first_quad_vertices.clear();
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		layer_quad_counts[i] = 0;
		layer_last_flip_frames[i] = 1;
		layer_last_flip_frame[i] = 0;
		layer_meshes[i].unref();
		if (mesh_layers[i] != nullptr) {
			mesh_layers[i]->set_mesh(Ref<Mesh>());
			mesh_layers[i]->set_visible(false);
		}
	}
}

void NovaParticleEmitter::_refresh_emitter() {
	if (def.is_valid()) {
		native_def = std::make_unique<opennova::particle::ParticleDef>(def->to_native());
		emitter.max_particles = native_def->emit_maxoverride > 0 ?
				static_cast<std::size_t>(native_def->emit_maxoverride) : 256u;

		// Bake per-graphic curve LUTs (and any other runtime-resolved
		// graphic state) before init. Engine analogue:
		// `CEffectDef_ResolveAllReferences @ 0x5e9d70` runs once per def
		// after parse — resolving every CurveRef name against the tabledef
		// pool, baking flat 256-byte LUTs, and populating the runtime UV
		// rect array used by the flipbook UV strip. Without this call the
		// simulator's `compute_spawn_flags` saw no `CurveRef::baked` set
		// and silently emitted particles with `flags = 0`, suppressing all
		// curve modulation in the editor preview.
		std::vector<opennova::particle::TableDef> native_tables;
		native_tables.reserve(static_cast<std::size_t>(tables.size()));
		for (int i = 0; i < tables.size(); ++i) {
			Ref<NovaParticleTable> table = tables[i];
			if (table.is_valid()) {
				native_tables.push_back(table->to_native());
			}
		}
		opennova::particle::bake_particle_def_curves(*native_def, native_tables);

		// Seed the simulator from the Node3D world origin so the delta
		// accumulators (last_translation_delta / cumulative_translation) are
		// referenced against the actual world position, not (0,0,0). Engine
		// equivalent: CEffectEmitter_Initialize @ 0x5e6020 reads
		// `spawnParams[0..2]` for emitter+24..32.
		opennova::particle::Vec3 origin{0.0f, 0.0f, 0.0f};
		if (is_inside_tree()) {
			const Vector3 world = get_global_transform().origin;
			origin = {world.x, world.y, world.z};
		}
		opennova::particle::emitter_init(emitter, native_def.get(), origin,
				static_cast<std::uint32_t>(seed));
	} else {
		native_def.reset();
		emitter.def = nullptr;
		emitter.particles.clear();
		atlas_texture.unref();
		atlas_layer_widths.fill(0);
		atlas_layer_heights.fill(0);
		atlas_layer_present.fill(false);
	}
	_clear_meshes();
}

Ref<NovaParticleTable> NovaParticleEmitter::_find_table(const String &id) const {
	if (id.is_empty()) {
		return Ref<NovaParticleTable>();
	}
	for (int i = 0; i < tables.size(); ++i) {
		Ref<NovaParticleTable> table = tables[i];
		if (table.is_valid() && table->get_id() == id) {
			return table;
		}
	}
	return Ref<NovaParticleTable>();
}

float NovaParticleEmitter::_sample_curve(const Ref<NovaParticleCurveRef> &curve, float t, float fallback) const {
	if (curve.is_null() || !curve->get_present()) {
		return fallback;
	}
	Ref<NovaParticleTable> table = _find_table(curve->get_name());
	if (table.is_null()) {
		return fallback;
	}
	float sample_t = std::clamp(t, 0.0f, 1.0f);
	if (curve->get_reverse()) {
		sample_t = 1.0f - sample_t;
	}
	float value = static_cast<float>(table->sample(sample_t)) / 255.0f;
	if (curve->get_inverse()) {
		value = 1.0f - value;
	}
	return value;
}

Color NovaParticleEmitter::_layer_color(const Ref<NovaParticleGraphicLayer> &layer, std::uint8_t slot) const {
	if (layer.is_valid() && layer->get_present() && layer->get_color_overrides_set()) {
		switch (slot & 3u) {
			case 0:
				return layer->get_color1();
			case 1:
				return layer->get_color2();
			case 2:
				return layer->get_color3_prop();
			default:
				return layer->get_color4();
		}
	}
	if (def.is_valid()) {
		switch (slot & 3u) {
			case 0:
				return def->get_color1();
			case 1:
				return def->get_color2();
			case 2:
				return def->get_color3_prop();
			default:
				return def->get_color4();
		}
	}
	return Color(1.0f, 1.0f, 1.0f, 1.0f);
}

void NovaParticleEmitter::_refresh_layer_materials(
		const std::array<Ref<NovaParticleGraphicLayer>, MAX_VISUAL_LAYERS> &layers,
		const std::array<bool, MAX_VISUAL_LAYERS> &present) {
	// Phase 1: per-layer blend-mode + source-texture load (cached on
	// texture_name change). Engine analogue: each particle graphic carries
	// its own loose-texture name; the atlas builder consumes the loaded
	// images.
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		const int blend_mode = present[i] && layers[i].is_valid() ?
				std::clamp(layers[i]->get_blend_mode(), 0, 7) : 0;
		if (layer_materials[i].is_null() || layer_blend_modes[i] != blend_mode) {
			layer_blend_modes[i] = blend_mode;
			layer_materials[i] = _make_layer_material(blend_mode);
		}
		if (mesh_layers[i] != nullptr) {
			mesh_layers[i]->set_material_override(layer_materials[i]);
		}

		String texture_name;
		if (present[i] && layers[i].is_valid()) {
			texture_name = layers[i]->get_texture();
		}

		if (texture_name != layer_texture_names[i]) {
			layer_texture_names[i] = texture_name;
			layer_texture_paths[i] = String();
			layer_textures[i].unref();
			if (!texture_dir.is_empty() && !texture_name.is_empty()) {
				layer_texture_paths[i] = opennova::resolve_texture_path(texture_dir, texture_name);
				layer_textures[i] = opennova::load_texture_from_dir(texture_dir, texture_name);
			}
		}
	}

	// Phase 2: build/refresh the per-emitter atlas from the loaded source
	// textures. Updates `native_def->graphics[i].baked_uv_rects` to atlas
	// coordinates so the renderer reads atlas-relative UVs without any
	// further plumbing. Engine: CParticleManager_BuildTextureAtlases @ 0x5e8db0.
	_rebuild_atlas_texture(present);

	// Phase 3: bind the shared atlas texture to every layer material's
	// `albedo_tex`. `has_texture` stays per-layer (drives the procedural
	// soft-disc fallback path in shaders).
	const Ref<Texture2D> atlas_or_fallback = atlas_texture.is_valid() ?
			Ref<Texture2D>(atlas_texture) : fallback_texture;
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		const bool layer_has_texture = layer_textures[i].is_valid();
		layer_materials[i]->set_shader_parameter("albedo_tex", atlas_or_fallback);
		layer_materials[i]->set_shader_parameter("has_texture", layer_has_texture);
	}
}

void NovaParticleEmitter::_rebuild_atlas_texture(
		const std::array<bool, MAX_VISUAL_LAYERS> &present) {
	if (native_def == nullptr) {
		atlas_texture.unref();
		atlas_layer_widths.fill(0);
		atlas_layer_heights.fill(0);
		atlas_layer_present.fill(false);
		return;
	}

	// Snapshot the per-layer (width, height, present) signature so we can
	// skip the rebuild if nothing meaningful changed (texture cache miss
	// without dimension change is the common steady-state path).
	std::array<int, MAX_VISUAL_LAYERS> widths{};
	std::array<int, MAX_VISUAL_LAYERS> heights{};
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (present[i] && layer_textures[i].is_valid()) {
			widths[i] = layer_textures[i]->get_width();
			heights[i] = layer_textures[i]->get_height();
		}
	}
	bool unchanged = atlas_texture.is_valid();
	for (int i = 0; i < MAX_VISUAL_LAYERS && unchanged; ++i) {
		if (atlas_layer_widths[i] != widths[i] ||
				atlas_layer_heights[i] != heights[i] ||
				atlas_layer_present[i] != present[i]) {
			unchanged = false;
		}
	}
	if (unchanged) {
		return;
	}

	atlas_layer_widths = widths;
	atlas_layer_heights = heights;
	atlas_layer_present = present;

	std::array<opennova::particle::AtlasInputSize, 4> sizes{};
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		sizes[static_cast<std::size_t>(i)] = {widths[i], heights[i]};
	}

	const opennova::particle::AtlasLayout layout =
			opennova::particle::bake_atlas_layout(*native_def, sizes);

	if (layout.atlas_width <= 0 || layout.atlas_height <= 0) {
		atlas_texture.unref();
		return;
	}

	Ref<Image> atlas_image = Image::create(layout.atlas_width, layout.atlas_height,
			false, Image::FORMAT_RGBA8);
	if (atlas_image.is_null()) {
		atlas_texture.unref();
		return;
	}

	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (widths[i] <= 0 || heights[i] <= 0) {
			continue;
		}
		// ImageTexture::get_image returns a decompressed copy; safe to
		// blit into our RGBA8 atlas without further processing. Convert
		// to RGBA8 if the source format differs (PCX/TGA decoders may
		// hand back RGB8 or grayscale formats).
		Ref<Image> layer_image = layer_textures[i]->get_image();
		if (layer_image.is_null()) {
			continue;
		}
		if (layer_image->get_format() != Image::FORMAT_RGBA8) {
			layer_image->convert(Image::FORMAT_RGBA8);
		}
		atlas_image->blit_rect(layer_image,
				Rect2i(Vector2i(0, 0), Vector2i(widths[i], heights[i])),
				Vector2i(layout.layer_x_offset[i], 0));
	}

	atlas_texture = ImageTexture::create_from_image(atlas_image);
}

void NovaParticleEmitter::_update_meshes() {
	if (def.is_null() || native_def == nullptr) {
		_clear_meshes();
		return;
	}
	_ensure_visual_setup();

	std::array<Ref<NovaParticleGraphicLayer>, MAX_VISUAL_LAYERS> layers;
	std::array<bool, MAX_VISUAL_LAYERS> present{};
	bool has_present_layer = false;
	TypedArray<NovaParticleGraphicLayer> graphics = def->get_graphics();
	const int graphic_count = std::min<int>(graphics.size(), MAX_VISUAL_LAYERS);
	for (int i = 0; i < graphic_count; ++i) {
		Ref<NovaParticleGraphicLayer> layer = graphics[i];
		layers[i] = layer;
		if (layer.is_valid() && layer->get_present()) {
			present[i] = true;
			has_present_layer = true;
		}
	}
	if (!has_present_layer) {
		present[0] = true;
	}
	_refresh_layer_materials(layers, present);

	const int alive = static_cast<int>(emitter.particles.size());
	int fallback_layer = 0;
	for (int layer_idx = 0; layer_idx < MAX_VISUAL_LAYERS; ++layer_idx) {
		if (present[layer_idx]) {
			fallback_layer = layer_idx;
			break;
		}
	}
	auto render_layer_for_particle = [&](const opennova::particle::Particle &p) {
		const int particle_layer = std::clamp<int>(static_cast<int>(p.graphic_layer), 0, MAX_VISUAL_LAYERS - 1);
		return present[particle_layer] ? particle_layer : fallback_layer;
	};

	if (alive == 0) {
		_clear_meshes();
		return;
	}

	// World-space rendering. The simulator's `Particle::position` is in
	// world coordinates (emitter is seeded from `get_global_transform().origin`
	// in `_refresh_emitter`, kept in sync via NOTIFICATION_TRANSFORM_CHANGED
	// → `emitter_translate`). MeshInstance3D children are configured with
	// `set_as_top_level(true)` in `_ensure_visual_setup`, so the vertex
	// positions are interpreted directly as world coordinates without
	// applying the NovaParticleEmitter's transform. Camera basis is read in
	// world space so billboards face the camera regardless of parent
	// rotation. PositionRelative flag carries alive particles with the
	// emitter at the simulator level (see emitter_translate).
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	Vector3 right(1.0f, 0.0f, 0.0f);
	Vector3 up(0.0f, 1.0f, 0.0f);
	Transform3D camera_view;
	if (camera != nullptr) {
		const Transform3D camera_global = camera->get_global_transform();
		camera_view = camera_global.affine_inverse();
		right = camera_global.basis.get_column(0);
		up = camera_global.basis.get_column(1);
		if (right.length_squared() > 0.0f) {
			right.normalize();
		} else {
			right = Vector3(1.0f, 0.0f, 0.0f);
		}
		if (up.length_squared() > 0.0f) {
			up.normalize();
		} else {
			up = Vector3(0.0f, 1.0f, 0.0f);
		}
	}

	std::vector<RenderParticle> sorted;
	sorted.reserve(static_cast<std::size_t>(alive));

	// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 LOD decimation: when
	// the manager-set divisor is > 1, skip particles whose `serial`
	// (per-particle byte at +0) is not aligned. divisor=1 = render all
	// (engine behaviour at full perf budget); divisor>1 = uniform skip
	// without touching simulation state. See Emitter::lod_divisor doc.
	const std::uint32_t lod_divisor = std::max<std::uint32_t>(emitter.lod_divisor, 1u);

	for (int i = 0; i < alive; ++i) {
		const opennova::particle::Particle &p = emitter.particles[static_cast<std::size_t>(i)];
		if (lod_divisor > 1u && (static_cast<std::uint32_t>(p.serial) % lod_divisor) != 0u) {
			continue;
		}
		const int layer_idx = render_layer_for_particle(p);
		if (!present[layer_idx]) {
			continue;
		}
		const float t = p.lifetime > 0.0f ?
				std::clamp(1.0f - (p.age / p.lifetime), 0.0f, 1.0f) : 1.0f;
		const Ref<NovaParticleGraphicLayer> layer = layers[layer_idx];
		float base_scale = def->get_scale_value();
		float layer_alpha = 1.0f;
		Ref<NovaParticleCurveRef> scale_curve = def->get_scale_func();
		Ref<NovaParticleCurveRef> alpha_curve = def->get_alpha_func();
		Ref<NovaParticleCurveRef> red_curve = def->get_red_func();
		Ref<NovaParticleCurveRef> green_curve = def->get_green_func();
		Ref<NovaParticleCurveRef> blue_curve = def->get_blue_func();
		if (layer.is_valid() && layer->get_present()) {
			if (layer->get_scale_value() > 0.0f) {
				base_scale = layer->get_scale_value();
			}
			layer_alpha = layer->get_alpha();
			scale_curve = choose_curve(layer->get_scale_func(), scale_curve);
			alpha_curve = choose_curve(layer->get_alpha_func(), alpha_curve);
			red_curve = choose_curve(layer->get_red_func(), red_curve);
			green_curve = choose_curve(layer->get_green_func(), green_curve);
			blue_curve = choose_curve(layer->get_blue_func(), blue_curve);
		}
		if (base_scale <= 0.0f) {
			base_scale = 1.0f;
		}

		const float scale_mult = _sample_curve(scale_curve, t, 1.0f);
		const float alpha_mult = _sample_curve(alpha_curve, t, 1.0f);
		const float red_mult = _sample_curve(red_curve, t, 1.0f);
		const float green_mult = _sample_curve(green_curve, t, 1.0f);
		const float blue_mult = _sample_curve(blue_curve, t, 1.0f);

		const float s = std::max(0.01f, p.scale * base_scale * scale_mult);

		// Manager-level RGB tint — CParticleEmitter_BuildBillboardQuads @
		// 0x5e6d60 multiplies each channel by `(emitter_byte * channel) >> 7`
		// (so engine byte 128 = 1.0). Our portable simulator stores the tint
		// as a Vec3 in [0..2] range. Default {1, 1, 1} = no change. Applied
		// AFTER the per-curve modulation, BEFORE the 0..1 clamp.
		const opennova::particle::Vec3 tint = emitter.color_tint;

		Color color = _layer_color(layer, p.color_slot);
		color.r = std::clamp(color.r * red_mult * tint.x, 0.0f, 1.0f);
		color.g = std::clamp(color.g * green_mult * tint.y, 0.0f, 1.0f);
		color.b = std::clamp(color.b * blue_mult * tint.z, 0.0f, 1.0f);
		color.a = std::clamp((static_cast<float>(p.alpha) / 255.0f) * layer_alpha * alpha_mult,
				0.0f, 1.0f);

		const int flip_frames = layer.is_valid() && layer->get_present() ?
				std::max(1, layer->get_flip_frames()) : 1;
		const int flip_rate = layer.is_valid() && layer->get_present() ?
				std::max(0, layer->get_flip_rate()) : 0;
		const float elapsed = p.lifetime > 0.0f ? std::max(0.0f, p.lifetime - p.age) : 0.0f;
		const int frame = flip_frames > 1 && flip_rate > 0 ?
				static_cast<int>(std::floor(elapsed * static_cast<float>(flip_rate))) % flip_frames : 0;

		RenderParticle rp;
		rp.layer_idx = layer_idx;
		rp.rotation = p.rotation;
		rp.scale = s;
		rp.frame = frame;
		rp.flip_frames = flip_frames;
		rp.blend_mode = layer_blend_modes[layer_idx];
		rp.position = Vector3(p.position.x, p.position.y, p.position.z);
		rp.color = color;

		// Engine-faithful lit color computation when the LitColor flag is
		// set (Bump=3 or Bumpadd=6 blend modes). Engine reference:
		// `CParticleEmitter_BuildBillboardQuads @ 0x5e6d60`, second-color
		// branch when `particle.flags & 0x80`. Engine pipeline (RE 2026-04-28):
		//   1. Build `D3DXMatrixRotationX(rotation × π/180)` (engine stores
		//      rotation in degrees; flt_7DCB00 = π/180).
		//   2. Multiply with the emitter's view matrix at emitter+8+664.
		//   3. `D3DXMatrixTranspose` the composite (= sub_68BF44 @ 0x68bf4a).
		//   4. Multiply hardcoded light direction (-1/√3, -1/√3, +1/√3)
		//      (flt_848D34/D38/D3C) by the transposed matrix.
		//   5. Scale by def.bump_scale and encode per channel:
		//      `byte = clamp((value + 1) × 0.5, 0, 1) × 255`.
		//
		// Our portable form rotates the camera right/up axes by rp.rotation
		// around the view direction (matches our billboard frame
		// construction exactly), then projects the engine light direction
		// into the local frame via dot products with the orthonormal axes.
		// Result: lit_color varies with particle rotation, matching the
		// engine's intent of direction-dependent shading.
		//
		// **Bounded deviation**: the engine rotates around the X axis of a
		// composite view-space matrix (D3DXMatrixRotationX), while we rotate
		// around the view direction (Z axis of our billboard frame). Both
		// paths produce direction-dependent variation that responds to
		// rotation, but exact per-channel values differ vs the engine.
		// Closing this gap requires a 4×4 matrix port + axis-convention RE.
		if ((p.flags & opennova::particle::particle_runtime_flag::LitColor) != 0) {
			const float bump_scale = native_def->bump_scale;
			constexpr float k = 0.5773503f;  // 1/√3 (engine: flt_848D34/D38/D3C)
			const Vector3 light_world(-k, -k, +k);

			const float rc = std::cos(rp.rotation);
			const float rs = std::sin(rp.rotation);
			const Vector3 local_right = right * rc + up * rs;
			const Vector3 local_up = -right * rs + up * rc;
			Vector3 local_forward = right.cross(up);
			if (local_forward.length_squared() > 0.0f) {
				local_forward.normalize();
			}

			const Vector3 light_local(
					local_right.dot(light_world),
					local_up.dot(light_world),
					local_forward.dot(light_world));

			const float lit_r = std::clamp(
					(bump_scale * light_local.x + 1.0f) * 0.5f, 0.0f, 1.0f);
			const float lit_g = std::clamp(
					(bump_scale * light_local.y + 1.0f) * 0.5f, 0.0f, 1.0f);
			const float lit_b = std::clamp(
					(bump_scale * light_local.z + 1.0f) * 0.5f, 0.0f, 1.0f);
			rp.lit_color = Color(lit_r, lit_g, lit_b, color.a);
		} else {
			rp.lit_color = Color(1.0f, 1.0f, 1.0f, 1.0f);  // neutral (multiply identity)
		}
		// Particle position is already world-space (top_level mesh +
		// world-space simulator). Project directly to view space without
		// applying the NovaParticleEmitter's local transform.
		if (camera != nullptr) {
			rp.depth = -camera_view.xform(rp.position).z;
		} else {
			rp.depth = -rp.position.z;
		}
		sorted.push_back(rp);
	}

	std::sort(sorted.begin(), sorted.end(), [](const RenderParticle &a, const RenderParticle &b) {
		if (a.depth == b.depth) {
			return a.layer_idx < b.layer_idx;
		}
		return a.depth > b.depth;
	});

	std::array<std::vector<RenderParticle>, MAX_VISUAL_LAYERS> per_layer;
	for (const RenderParticle &rp : sorted) {
		per_layer[static_cast<std::size_t>(rp.layer_idx)].push_back(rp);
	}

	last_render_batch_count = 0;
	last_sorted_depth_count = static_cast<int>(sorted.size());
	debug_first_rotation = 0.0f;
	debug_first_flip_frame = 0;
	debug_first_blend_mode = 0;
	// Engine: CParticleEmitter_RenderStaticBillboards @ 0x5f4e10 is selected
	// when `def.flags & 0x100` is set (= particle_flag::YawAndPitch); that path
	// uses D3DXMatrixScaling only (no per-particle rotation). We mirror by
	// suppressing the 2D rotate when the bit is set. The debug bool is
	// captured per render so GUT tests can verify the branch was taken.
	debug_static_billboard =
			(native_def->flags & opennova::particle::particle_flag::YawAndPitch) != 0;
	debug_first_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
	debug_first_lit_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
	debug_first_quad_vertices.clear();
	bool debug_quad_set = false;

	for (int layer_idx = 0; layer_idx < MAX_VISUAL_LAYERS; ++layer_idx) {
		const std::vector<RenderParticle> &layer_particles = per_layer[static_cast<std::size_t>(layer_idx)];
		const int quad_count = static_cast<int>(layer_particles.size());
		layer_quad_counts[layer_idx] = quad_count;
		layer_last_flip_frames[layer_idx] = 1;
		layer_last_flip_frame[layer_idx] = 0;

		if (mesh_layers[layer_idx] == nullptr) {
			continue;
		}
		if (quad_count == 0) {
			layer_meshes[layer_idx].unref();
			mesh_layers[layer_idx]->set_mesh(Ref<Mesh>());
			mesh_layers[layer_idx]->set_visible(false);
			continue;
		}

		PackedVector3Array verts;
		PackedVector2Array uvs;
		PackedColorArray colors;
		PackedInt32Array indices;
		// Engine FVF 450 = D3DFVF_XYZ | DIFFUSE | SPECULAR | TEX1. We mirror
		// the SPECULAR slot via Godot's ARRAY_CUSTOM0 (RGBA8 unorm = 4 bytes
		// per vertex). Bump and Bumpadd shaders read it via the CUSTOM0
		// fragment input.
		PackedByteArray custom0;
		verts.resize(quad_count * 4);
		uvs.resize(quad_count * 4);
		colors.resize(quad_count * 4);
		custom0.resize(quad_count * 4 * 4);
		uint8_t *custom0_ptr = custom0.ptrw();
		indices.resize(quad_count * 6);

		for (int q = 0; q < quad_count; ++q) {
			const RenderParticle &rp = layer_particles[static_cast<std::size_t>(q)];
			const float half = 0.5f * rp.scale;
			// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 vs.
			// CParticleEmitter_RenderStaticBillboards @ 0x5f4e10 dispatch:
			// rotated path applies a `D3DXMatrixRotationX(angle)` per particle,
			// static path uses `D3DXMatrixScaling` only. The engine selects
			// based on `(def.flags & 0x100) == 0` (YawAndPitch suppresses
			// rotation). We branch the same way using the per-render
			// `debug_static_billboard` snapshot so all particles in a frame
			// take a consistent path.
			const float effective_rotation = debug_static_billboard ? 0.0f : rp.rotation;
			const float c = std::cos(effective_rotation);
			const float s = std::sin(effective_rotation);
			const Vector2 local_corners[4] = {
				Vector2(-half, half),
				Vector2(half, half),
				Vector2(-half, -half),
				Vector2(half, -half),
			};
			Vector3 quad_verts[4];
			for (int corner = 0; corner < 4; ++corner) {
				const Vector2 local = local_corners[corner];
				const float rx = local.x * c - local.y * s;
				const float ry = local.x * s + local.y * c;
				quad_verts[corner] = rp.position + right * rx + up * ry;
			}

			// Engine: CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 reads
			// per-frame UV rects from `graphic+724` (a runtime-baked array
			// populated by CParticleManager_BuildTextureAtlases @ 0x5e8db0).
			// Our portable form embeds the rects in
			// `GraphicLayer::baked_uv_rects` via `bake_graphic_uv_rects`,
			// defaulting to horizontal-strip layout but allowing future
			// atlas-bake replacements. The renderer applies `inset` as
			// symmetric texel padding to suppress neighbour-tile bleeding.
			const auto &uv_rects = native_def->graphics[
					static_cast<std::size_t>(layer_idx)].baked_uv_rects;
			const std::size_t rect_count = std::max<std::size_t>(uv_rects.size(), 1);
			const std::size_t frame_idx = static_cast<std::size_t>(
					std::max(0, rp.frame)) % rect_count;
			float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
			if (!uv_rects.empty()) {
				const opennova::particle::UvRect &rect = uv_rects[frame_idx];
				u0 = rect.u_min + rect.inset;
				u1 = rect.u_max - rect.inset;
				v0 = rect.v_min + rect.inset;
				v1 = rect.v_max - rect.inset;
			} else {
				// Fallback for legacy paths where bake hasn't run yet.
				const float inv_frames = 1.0f / static_cast<float>(std::max(1, rp.flip_frames));
				u0 = static_cast<float>(rp.frame) * inv_frames;
				u1 = static_cast<float>(rp.frame + 1) * inv_frames;
			}
			const int vertex_offset = q * 4;
			const int index_offset = q * 6;
			verts[vertex_offset + 0] = quad_verts[0];
			verts[vertex_offset + 1] = quad_verts[1];
			verts[vertex_offset + 2] = quad_verts[2];
			verts[vertex_offset + 3] = quad_verts[3];
			uvs[vertex_offset + 0] = Vector2(u0, v0);
			uvs[vertex_offset + 1] = Vector2(u1, v0);
			uvs[vertex_offset + 2] = Vector2(u0, v1);
			uvs[vertex_offset + 3] = Vector2(u1, v1);
			for (int corner = 0; corner < 4; ++corner) {
				colors[vertex_offset + corner] = rp.color;
			}
			// Engine `D3DFVF_DIFFUSE` slot — the lit color when LitColor flag
			// is set, white otherwise. Encoded as RGBA8 unorm (4 bytes) into
			// ARRAY_CUSTOM0. Engine writes this at vertex offset +12 in the
			// 28-byte vertex layout per `BuildBillboardQuads @ 0x5e6d60`.
			const uint8_t lit_r = static_cast<uint8_t>(std::clamp(rp.lit_color.r, 0.0f, 1.0f) * 255.0f);
			const uint8_t lit_g = static_cast<uint8_t>(std::clamp(rp.lit_color.g, 0.0f, 1.0f) * 255.0f);
			const uint8_t lit_b = static_cast<uint8_t>(std::clamp(rp.lit_color.b, 0.0f, 1.0f) * 255.0f);
			const uint8_t lit_a = static_cast<uint8_t>(std::clamp(rp.lit_color.a, 0.0f, 1.0f) * 255.0f);
			for (int corner = 0; corner < 4; ++corner) {
				const int byte_offset = (vertex_offset + corner) * 4;
				custom0_ptr[byte_offset + 0] = lit_r;
				custom0_ptr[byte_offset + 1] = lit_g;
				custom0_ptr[byte_offset + 2] = lit_b;
				custom0_ptr[byte_offset + 3] = lit_a;
			}
			indices[index_offset + 0] = vertex_offset + 0;
			indices[index_offset + 1] = vertex_offset + 1;
			indices[index_offset + 2] = vertex_offset + 2;
			indices[index_offset + 3] = vertex_offset + 1;
			indices[index_offset + 4] = vertex_offset + 3;
			indices[index_offset + 5] = vertex_offset + 2;

			if (!debug_quad_set) {
				// debug_first_rotation reflects the *effective* rotation used
				// to construct the quad (= 0 when static-billboard, particle
				// rotation otherwise). Tests use this to verify YawAndPitch
				// suppresses rotation regardless of the per-particle accumulator.
				debug_first_rotation = effective_rotation;
				debug_first_flip_frame = rp.frame;
				debug_first_blend_mode = rp.blend_mode;
				debug_first_color = rp.color;
				debug_first_lit_color = rp.lit_color;
				debug_first_quad_vertices.resize(4);
				for (int corner = 0; corner < 4; ++corner) {
					debug_first_quad_vertices[corner] = quad_verts[corner];
				}
				debug_quad_set = true;
			}
			layer_last_flip_frames[layer_idx] = rp.flip_frames;
			layer_last_flip_frame[layer_idx] = rp.frame;
		}

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_COLOR] = colors;
		arrays[Mesh::ARRAY_CUSTOM0] = custom0;
		arrays[Mesh::ARRAY_INDEX] = indices;

		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		// ARRAY_FORMAT_CUSTOM0 enables the CUSTOM0 attribute; the format
		// bits at ARRAY_FORMAT_CUSTOM0_SHIFT default to 0 = RGBA8_UNORM,
		// matching our 4-byte-per-vertex encoding above.
		const uint64_t surface_flags = static_cast<uint64_t>(Mesh::ARRAY_FORMAT_CUSTOM0);
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays,
				TypedArray<Array>(), Dictionary(),
				static_cast<Mesh::ArrayFormat>(surface_flags));
		mesh->surface_set_material(0, layer_materials[layer_idx]);
		layer_meshes[layer_idx] = mesh;
		mesh_layers[layer_idx]->set_material_override(layer_materials[layer_idx]);
		mesh_layers[layer_idx]->set_mesh(mesh);
		mesh_layers[layer_idx]->set_visible(true);
		++last_render_batch_count;
	}
}

void NovaParticleEmitter::set_def(const Ref<NovaParticleDef> &p_def) {
	def = p_def;
	if (is_inside_tree()) {
		_refresh_emitter();
	}
}

Ref<NovaParticleDef> NovaParticleEmitter::get_def() const { return def; }

void NovaParticleEmitter::set_tables(const TypedArray<NovaParticleTable> &p_tables) {
	tables = p_tables;
	if (is_inside_tree()) {
		_update_meshes();
	}
}

TypedArray<NovaParticleTable> NovaParticleEmitter::get_tables() const { return tables; }

void NovaParticleEmitter::set_seed(int p_seed) {
	seed = p_seed;
	if (is_inside_tree()) {
		_refresh_emitter();
	}
}

int NovaParticleEmitter::get_seed() const { return seed; }

void NovaParticleEmitter::set_auto_advance(bool p_value) { auto_advance = p_value; }
bool NovaParticleEmitter::get_auto_advance() const { return auto_advance; }

void NovaParticleEmitter::set_time_scale(float p_value) {
	time_scale = std::max(0.0f, p_value);
}

float NovaParticleEmitter::get_time_scale() const { return time_scale; }

void NovaParticleEmitter::set_texture_dir(const String &p_dir) {
	if (texture_dir == p_dir) {
		return;
	}
	texture_dir = p_dir;
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		layer_texture_names[i] = String();
		layer_texture_paths[i] = String();
		layer_textures[i].unref();
	}
	if (is_inside_tree()) {
		_update_meshes();
	}
}

String NovaParticleEmitter::get_texture_dir() const { return texture_dir; }

String NovaParticleEmitter::get_resolved_texture_path(int p_layer_index) const {
	if (p_layer_index < 0 || p_layer_index >= MAX_VISUAL_LAYERS) {
		return String();
	}
	return layer_texture_paths[p_layer_index];
}

void NovaParticleEmitter::set_color_tint(const Color &p_tint) {
	emitter.color_tint = {p_tint.r, p_tint.g, p_tint.b};
}

Color NovaParticleEmitter::get_color_tint() const {
	return Color(emitter.color_tint.x, emitter.color_tint.y, emitter.color_tint.z, 1.0f);
}

void NovaParticleEmitter::set_spring_const(float p_value) {
	emitter.spring_const = p_value;
}

float NovaParticleEmitter::get_spring_const() const {
	return emitter.spring_const;
}

void NovaParticleEmitter::set_lod_divisor(int p_value) {
	emitter.lod_divisor = static_cast<std::uint32_t>(std::max(1, p_value));
}

int NovaParticleEmitter::get_lod_divisor() const {
	return static_cast<int>(emitter.lod_divisor);
}

void NovaParticleEmitter::set_kill_plane_mode(int p_value) {
	// Engine: bits 27/28 in def.flags select kill-above vs kill-at/below.
	// Our portable form clamps to {0=Disabled, 1=KillAbove, 2=KillAtOrBelow}
	// — out-of-range values fall back to Disabled.
	const std::uint32_t clamped = (p_value == 1 || p_value == 2)
			? static_cast<std::uint32_t>(p_value)
			: 0u;
	emitter.kill_plane_mode = clamped;
}

int NovaParticleEmitter::get_kill_plane_mode() const {
	return static_cast<int>(emitter.kill_plane_mode);
}

void NovaParticleEmitter::set_kill_plane_y(float p_value) {
	emitter.kill_plane_y = p_value;
}

float NovaParticleEmitter::get_kill_plane_y() const {
	return emitter.kill_plane_y;
}

void NovaParticleEmitter::play() {
	if (def.is_null()) {
		return;
	}
	if (is_inside_tree()) {
		_ensure_visual_setup();
	}
	_refresh_emitter();
	playing = true;
}

void NovaParticleEmitter::stop() {
	playing = false;
	emitter.particles.clear();
	_clear_meshes();
}

void NovaParticleEmitter::restart() {
	if (def.is_null()) {
		return;
	}
	_refresh_emitter();
	playing = true;
}

void NovaParticleEmitter::advance(float dt) {
	if (!playing || emitter.def == nullptr) {
		return;
	}
	opennova::particle::emitter_advance(emitter, dt);
	_update_meshes();
	if (is_finished()) {
		playing = false;
	}
}

bool NovaParticleEmitter::is_finite() const {
	if (emitter.def == nullptr) {
		return true;
	}
	return emitter.finite;
}

bool NovaParticleEmitter::is_finished() const {
	if (emitter.def == nullptr) {
		return true;
	}
	if (!emitter.finite) {
		return false;
	}
	return emitter.emit_dur_remaining <= 0.0f && emitter.particles.empty();
}

int NovaParticleEmitter::get_alive_count() const {
	return static_cast<int>(emitter.particles.size());
}

int NovaParticleEmitter::get_visual_layer_count() const {
	if (def.is_null()) {
		return 0;
	}
	int count = 0;
	TypedArray<NovaParticleGraphicLayer> graphics = def->get_graphics();
	const int graphic_count = std::min<int>(graphics.size(), MAX_VISUAL_LAYERS);
	for (int i = 0; i < graphic_count; ++i) {
		Ref<NovaParticleGraphicLayer> layer = graphics[i];
		if (layer.is_valid() && layer->get_present()) {
			++count;
		}
	}
	return count > 0 ? count : 1;
}

int NovaParticleEmitter::get_rendered_instance_count() const {
	int total = 0;
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		total += layer_quad_counts[i];
	}
	return total;
}

int NovaParticleEmitter::get_textured_layer_count() const {
	int total = 0;
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (layer_textures[i].is_valid()) {
			++total;
		}
	}
	return total;
}

int NovaParticleEmitter::get_render_batch_count() const {
	return last_render_batch_count;
}

int NovaParticleEmitter::get_sorted_depth_count() const {
	return last_sorted_depth_count;
}

float NovaParticleEmitter::get_debug_first_rotation() const {
	return debug_first_rotation;
}

int NovaParticleEmitter::get_debug_first_flip_frame() const {
	return debug_first_flip_frame;
}

int NovaParticleEmitter::get_debug_first_blend_mode() const {
	return debug_first_blend_mode;
}

bool NovaParticleEmitter::get_debug_static_billboard() const {
	return debug_static_billboard;
}

Color NovaParticleEmitter::get_debug_first_color() const {
	return debug_first_color;
}

Color NovaParticleEmitter::get_debug_first_lit_color() const {
	return debug_first_lit_color;
}

Ref<ImageTexture> NovaParticleEmitter::get_debug_atlas_texture() const {
	return atlas_texture;
}

Ref<ShaderMaterial> NovaParticleEmitter::get_debug_layer_material(int p_layer_index) const {
	if (p_layer_index < 0 || p_layer_index >= MAX_VISUAL_LAYERS) {
		return Ref<ShaderMaterial>();
	}
	return layer_materials[static_cast<std::size_t>(p_layer_index)];
}

Vector3 NovaParticleEmitter::get_debug_last_translation_delta() const {
	return Vector3(emitter.last_translation_delta.x,
			emitter.last_translation_delta.y,
			emitter.last_translation_delta.z);
}

Vector3 NovaParticleEmitter::get_debug_first_layer_aabb_center() const {
	// Return the world-space AABB center of the first present layer's
	// MeshInstance3D. Engine equivalent: per-emitter bbox center projected
	// to view space in CParticleManager_TransformToViewSpace @ 0x5ecc50.
	// Godot's transparent renderer auto-sorts meshes back-to-front by AABB
	// center depth — with our top_level=true layer meshes + world-space
	// vertex data, this serves as the cross-emitter sort key without
	// needing a manager-level coordinator.
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (mesh_layers[i] != nullptr && mesh_layers[i]->get_mesh().is_valid()) {
			const AABB box = mesh_layers[i]->get_aabb();
			return box.get_center();
		}
	}
	return Vector3();
}

String NovaParticleEmitter::get_debug_first_shader_path() const {
	// Return the resource path of the shader bound to the first present
	// layer's material — used by GUT tests to assert the BlendMode → shader
	// dispatch picks the matching .gdshader file.
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (layer_materials[i].is_valid()) {
			Ref<Shader> shader = layer_materials[i]->get_shader();
			if (shader.is_valid()) {
				return shader->get_path();
			}
		}
	}
	return String();
}

PackedVector3Array NovaParticleEmitter::get_debug_first_quad_vertices() const {
	return debug_first_quad_vertices;
}
