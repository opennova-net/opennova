#include "particle/particle_renderer.h"

#include "env/mission_environment.h"

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

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/particle/emitter.h>
#include <runtime/renderer/particle_atlas.h>
#include <runtime/renderer/particle_color.h>
#include <runtime/renderer/particle_frame.h>

#include "particle/particle_compositor.h"
#include "render/frame_fx.h"
#include "util/texture_path_resolver.h"

using namespace godot;

namespace {

constexpr int kGraphicLayerCount = 4;
constexpr std::uint32_t kFirstPersonVisibilityMask = 1u << 11;
constexpr std::size_t kMissingEntry = std::numeric_limits<std::size_t>::max();

enum ParticleDrawSlot : std::size_t {
	kWorldFarSide = 0,
	kWorldCameraSide = 1,
	kReflectionFarSide = 2,
	kReflectionCameraSide = 3,
	kFirstPerson = 4,
	kParticleDrawSlotCount = 5,
};

using ParticleEffectPair = std::array<Ref<ParticleCompositorEffect>, 2>;

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

struct AtlasPage {
	std::uint8_t type = 0;
	int side = 0;
	Ref<ImageTexture> texture;
};

struct LayerVisual {
	bool present = false;
	std::uint8_t type = 0;
	int flip_frames = 1;
	int flip_rate = 0;
	std::vector<std::size_t> frame_entries;
};

struct DefinitionVisual {
	std::array<LayerVisual, kGraphicLayerCount> layers;
};

renderer::ParticleRgbaImage particle_rgba_image(const Ref<Image> &image,
		bool type_one_procedural_mask) {
	renderer::ParticleRgbaImage result;
	if (image.is_null() || image->get_width() <= 0 || image->get_height() <= 0)
		return result;
	result.width = image->get_width();
	result.height = image->get_height();
	const PackedByteArray pixels = image->get_data();
	const std::size_t width = static_cast<std::size_t>(result.width);
	const std::size_t height = static_cast<std::size_t>(result.height);
	if (width > std::numeric_limits<std::size_t>::max() / height)
		return renderer::ParticleRgbaImage{};
	const std::size_t pixel_count = width * height;
	if (pixel_count > std::numeric_limits<std::size_t>::max() / 4u)
		return renderer::ParticleRgbaImage{};
	const std::size_t mip0_bytes = pixel_count * 4u;
	if (mip0_bytes > static_cast<std::size_t>(
			std::numeric_limits<int64_t>::max()) ||
			pixels.size() < static_cast<int64_t>(mip0_bytes))
		return renderer::ParticleRgbaImage{};
	// Image::get_data() appends lower mip levels after mip 0. The portable
	// atlas accepts one flat RGBA frame, so copy only the base-level prefix.
	result.rgba.resize(mip0_bytes);
	if (!result.rgba.empty())
		std::memcpy(result.rgba.data(), pixels.ptr(), result.rgba.size());
	if (type_one_procedural_mask) {
		for (std::size_t offset = 0; offset + 3 < result.rgba.size();
				offset += 4) {
			const std::uint8_t radial = result.rgba[offset + 3];
			result.rgba[offset + 0] = radial;
			result.rgba[offset + 1] = radial;
			result.rgba[offset + 2] = radial;
		}
	}
	return result;
}

PackedByteArray packed_rgba(const std::vector<std::uint8_t> &pixels) {
	PackedByteArray result;
	result.resize(static_cast<int64_t>(pixels.size()));
	if (!pixels.empty())
		std::memcpy(result.ptrw(), pixels.data(), pixels.size());
	return result;
}

String shader_path_for_pipeline(renderer::ParticlePipeline pipeline) {
	switch (pipeline) {
		case renderer::ParticlePipeline::Blend:
			return "res://shaders/particle/particle_blend_blend.gdshader";
		case renderer::ParticlePipeline::Additive:
			return "res://shaders/particle/particle_blend_additive.gdshader";
		case renderer::ParticlePipeline::Premult:
			return "res://shaders/particle/particle_blend_premult.gdshader";
		case renderer::ParticlePipeline::Bump:
			return "res://shaders/particle/particle_blend_bump.gdshader";
		case renderer::ParticlePipeline::Mod:
			return "res://shaders/particle/particle_blend_mod.gdshader";
		case renderer::ParticlePipeline::Mod2x:
			return "res://shaders/particle/particle_blend_mod2x.gdshader";
		case renderer::ParticlePipeline::Bumpadd:
			return "res://shaders/particle/particle_blend_bumpadd.gdshader";
		case renderer::ParticlePipeline::Distort:
			return "res://shaders/particle/particle_blend_distort.gdshader";
	}
	return "res://shaders/particle/particle_blend_blend.gdshader";
}

std::uint8_t unit_byte(float value) {
	return renderer::particle_unit_byte(value);
}

std::uint32_t pack_argb(float red, float green, float blue, float alpha) {
	return (static_cast<std::uint32_t>(unit_byte(alpha)) << 24) |
			(static_cast<std::uint32_t>(unit_byte(red)) << 16) |
			(static_cast<std::uint32_t>(unit_byte(green)) << 8) |
			static_cast<std::uint32_t>(unit_byte(blue));
}

std::uint32_t pack_argb_bytes(std::uint8_t red, std::uint8_t green,
		std::uint8_t blue, std::uint8_t alpha) {
	return (static_cast<std::uint32_t>(alpha) << 24) |
			(static_cast<std::uint32_t>(red) << 16) |
			(static_cast<std::uint32_t>(green) << 8) |
			static_cast<std::uint32_t>(blue);
}

std::uint8_t retail_low_byte(float value) {
	return renderer::particle_retail_low_byte(value);
}

Color unpack_argb(std::uint32_t value) {
	constexpr float inv = 1.0f / 255.0f;
	return Color(
			static_cast<float>((value >> 16) & 0xffu) * inv,
			static_cast<float>((value >> 8) & 0xffu) * inv,
			static_cast<float>(value & 0xffu) * inv,
			static_cast<float>((value >> 24) & 0xffu) * inv);
}

renderer::ParticleVec3 particle_vec(const opennova::particle::Vec3 &value) {
	return {value.x, value.y, value.z};
}

renderer::ParticleVec3 particle_vec(const Vector3 &value) {
	return {value.x, value.y, value.z};
}

void include_point(renderer::ParticleAabb &bounds,
		const renderer::ParticleVec3 &point, float radius) {
	const renderer::ParticleVec3 minimum{
		point.x - radius, point.y - radius, point.z - radius};
	const renderer::ParticleVec3 maximum{
		point.x + radius, point.y + radius, point.z + radius};
	if (!bounds.valid) {
		bounds.min = minimum;
		bounds.max = maximum;
		bounds.valid = true;
		return;
	}
	bounds.min.x = std::min(bounds.min.x, minimum.x);
	bounds.min.y = std::min(bounds.min.y, minimum.y);
	bounds.min.z = std::min(bounds.min.z, minimum.z);
	bounds.max.x = std::max(bounds.max.x, maximum.x);
	bounds.max.y = std::max(bounds.max.y, maximum.y);
	bounds.max.z = std::max(bounds.max.z, maximum.z);
}

int64_t godot_token(std::uint64_t value) {
	int64_t result = 0;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

AABB godot_aabb(const renderer::ParticleAabb &bounds) {
	const Vector3 minimum(bounds.min.x, bounds.min.y, bounds.min.z);
	const Vector3 maximum(bounds.max.x, bounds.max.y, bounds.max.z);
	return AABB(minimum, maximum - minimum);
}

struct ParticleCameraFrame {
	Vector3 position{};
	Vector3 right{1.0f, 0.0f, 0.0f};
	Vector3 up{0.0f, 1.0f, 0.0f};
	Vector3 forward{0.0f, 0.0f, 1.0f};
	Basis view_basis{};
	std::array<float, 16> projection{};
	bool projection_valid = false;
};

// The manager camera matrix retail folds into view space per frame
// (CParticleManager_TransformToViewSpace @0x5ecc50 - docs/particles/ptl-format-re.md).
ParticleCameraFrame particle_camera_frame(Camera3D *camera) {
	ParticleCameraFrame result;
	if (camera == nullptr)
		return result;
	// get_camera_transform includes h_offset/v_offset, so classification and
	// billboards use the eye that the viewport actually renders.
	const Transform3D transform = camera->get_camera_transform();
	result.position = transform.origin;
	result.view_basis = transform.affine_inverse().basis;
	result.right = transform.basis.get_column(0);
	result.up = transform.basis.get_column(1);
	result.forward = -transform.basis.get_column(2);
	auto normalize_or = [](Vector3 value, const Vector3 &fallback) {
		if (value.length_squared() > 0.0f)
			return value.normalized();
		return fallback;
	};
	result.right = normalize_or(result.right, Vector3(1.0f, 0.0f, 0.0f));
	result.up = normalize_or(result.up, Vector3(0.0f, 1.0f, 0.0f));
	result.forward = normalize_or(result.forward, Vector3(0.0f, 0.0f, 1.0f));
	const Projection projection = camera->get_camera_projection();
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			result.projection[static_cast<std::size_t>(column * 4 + row)] =
					projection.columns[column][row];
		}
	}
	result.projection_valid = true;
	return result;
}

// Diagnostics read each compiler's retained draw list in place: it stays
// valid until that slot compiles again and nothing on the render thread
// references it (World submissions are immutable copies), so no per-frame
// capture copy and no opt-in flag are needed for the first report to be live.
Dictionary draw_list_report(const renderer::ParticleDrawList &draw_list) {
	Dictionary result;
	const renderer::ParticleFrameDebugCounters &debug = draw_list.debug;
	result["frame_id"] = godot_token(draw_list.frame_id);
	result["compile_index"] = godot_token(debug.compile_index);
	result["input_emitters"] = static_cast<int64_t>(debug.input_emitters);
	result["selected_emitters"] = static_cast<int64_t>(debug.selected_emitters);
	result["input_particles"] = static_cast<int64_t>(debug.input_particles);
	result["domain_filtered_particles"] =
			static_cast<int64_t>(debug.domain_filtered_particles);
	result["water_filtered_emitters"] =
			static_cast<int64_t>(debug.water_filtered_emitters);
	result["water_filtered_particles"] =
			static_cast<int64_t>(debug.water_filtered_particles);
	result["invisible_particles"] = static_cast<int64_t>(debug.invisible_particles);
	result["truncated_particles"] = static_cast<int64_t>(debug.truncated_particles);
	result["rendered_quad_count"] = static_cast<int64_t>(debug.emitted_quads);
	result["draw_command_count"] = static_cast<int64_t>(debug.draw_commands);
	result["adjacent_state_merges"] =
			static_cast<int64_t>(debug.adjacent_state_merges);
	result["recursive_partition_calls"] =
			static_cast<int64_t>(debug.recursive_partition_calls);
	result["render_batch_leaves"] =
			static_cast<int64_t>(debug.render_batch_leaves);
	result["capacity_growths_this_compile"] =
			static_cast<int64_t>(debug.capacity_growths_this_compile);
	result["lifetime_capacity_growths"] =
			godot_token(debug.lifetime_capacity_growths);
	result["vertex_capacity"] = static_cast<int64_t>(debug.vertex_capacity);
	result["command_capacity"] = static_cast<int64_t>(debug.command_capacity);
	result["emitter_bounds_capacity"] =
			static_cast<int64_t>(debug.emitter_bounds_capacity);
	Array commands;
	commands.resize(static_cast<int64_t>(draw_list.commands.size()));
	for (std::size_t i = 0; i < draw_list.commands.size(); ++i) {
		const renderer::ParticleDrawCommand &command = draw_list.commands[i];
		Dictionary value;
		value["render_domain"] = static_cast<int>(command.domain);
		value["render_pass"] = static_cast<int>(command.pass);
		value["pipeline"] = static_cast<int>(command.pipeline);
		value["atlas_page"] = static_cast<int64_t>(command.atlas_page);
		value["atlas_type"] = static_cast<int>(command.atlas_type);
		value["variant"] = static_cast<int>(command.variant);
		value["first_quad"] = static_cast<int64_t>(command.first_quad);
		value["quad_count"] = static_cast<int64_t>(command.quad_count);
		commands[static_cast<int64_t>(i)] = value;
	}
	result["commands"] = commands;
	return result;
}

WorldEnvironment *find_world_environment(Node *root) {
	if (root == nullptr)
		return nullptr;
	if (WorldEnvironment *environment =
				Object::cast_to<WorldEnvironment>(root))
		return environment;
	for (int child_index = 0; child_index < root->get_child_count();
			++child_index) {
		if (WorldEnvironment *environment =
					find_world_environment(root->get_child(child_index)))
			return environment;
	}
	return nullptr;
}

// A camera owns one composed resource regardless of how many independent
// EffectWorld/preview renderers target it. Keeping the coordinator outside any
// one renderer prevents A/B from cloning each other's installed compositor
// forever, and lets non-LIFO teardown remove only its own effect.
struct ParticleCameraCompositorState {
	Ref<Compositor> explicit_base;
	Ref<Compositor> effective_base;
	Ref<Compositor> base_source;
	Ref<Compositor> installed;
	std::vector<std::uint64_t> base_effect_ids;
	std::vector<std::pair<std::uint64_t,
			Ref<ParticleCompositorEffect>>> effects;
	bool camera_inherits_world = false;
	bool inherited_world_compositor = false;
};

std::map<std::uint64_t, ParticleCameraCompositorState> &
particle_camera_compositors() {
	static std::map<std::uint64_t, ParticleCameraCompositorState> states;
	return states;
}

Ref<Compositor> world_compositor_for(Viewport *viewport) {
	if (viewport == nullptr)
		return Ref<Compositor>();
	WorldEnvironment *environment = find_world_environment(viewport);
	return environment != nullptr ? environment->get_compositor() :
			Ref<Compositor>();
}

std::vector<std::uint64_t> non_particle_effect_ids(
		const Ref<Compositor> &compositor) {
	std::vector<std::uint64_t> ids;
	if (compositor.is_null())
		return ids;
	const TypedArray<Ref<CompositorEffect>> effects =
			compositor->get_compositor_effects();
	ids.reserve(static_cast<std::size_t>(effects.size()));
	for (int64_t index = 0; index < effects.size(); ++index) {
		Ref<CompositorEffect> effect = effects[index];
		if (effect.is_valid() &&
				Object::cast_to<ParticleCompositorEffect>(
						effect.ptr()) == nullptr) {
			ids.push_back(effect->get_instance_id());
		}
	}
	return ids;
}

Ref<Compositor> without_particle_effects(
		const Ref<Compositor> &source) {
	if (source.is_null())
		return Ref<Compositor>();
	const TypedArray<Ref<CompositorEffect>> source_effects =
			source->get_compositor_effects();
	TypedArray<Ref<CompositorEffect>> filtered;
	bool removed = false;
	for (int64_t index = 0; index < source_effects.size(); ++index) {
		Ref<CompositorEffect> effect = source_effects[index];
		if (effect.is_valid() &&
				Object::cast_to<ParticleCompositorEffect>(
						effect.ptr()) != nullptr) {
			removed = true;
			continue;
		}
		filtered.push_back(effect);
	}
	if (!removed)
		return source;
	Ref<Compositor> result;
	result.instantiate();
	result->set_compositor_effects(filtered);
	return result;
}

void set_camera_base_source(ParticleCameraCompositorState &state,
		const Ref<Compositor> &source) {
	state.base_source = source;
	state.base_effect_ids = non_particle_effect_ids(source);
	state.effective_base = without_particle_effects(source);
	state.explicit_base = state.camera_inherits_world ?
			Ref<Compositor>() : state.effective_base;
	state.inherited_world_compositor =
			state.camera_inherits_world && source.is_valid();
}

void capture_camera_base(ParticleCameraCompositorState &state,
		Camera3D *camera, Viewport *viewport) {
	Ref<Compositor> explicit_base = camera->get_compositor();
	state.camera_inherits_world = explicit_base.is_null();
	set_camera_base_source(state, state.camera_inherits_world ?
			world_compositor_for(viewport) : explicit_base);
}

bool refresh_camera_base(ParticleCameraCompositorState &state,
		Viewport *viewport) {
	Ref<Compositor> source = state.camera_inherits_world ?
			world_compositor_for(viewport) : state.base_source;
	const std::vector<std::uint64_t> effect_ids =
			non_particle_effect_ids(source);
	if (source == state.base_source &&
			effect_ids == state.base_effect_ids)
		return false;
	set_camera_base_source(state, source);
	return true;
}

void rebuild_camera_compositor(ParticleCameraCompositorState &state,
		Camera3D *camera) {
	TypedArray<Ref<CompositorEffect>> effects;
	TypedArray<Ref<CompositorEffect>> terminal_effects;
	if (state.effective_base.is_valid()) {
		const TypedArray<Ref<CompositorEffect>> base_effects =
				state.effective_base->get_compositor_effects();
		for (int64_t index = 0; index < base_effects.size(); ++index) {
			Ref<CompositorEffect> effect = base_effects[index];
			if (effect.is_valid() &&
					Object::cast_to<FrameFxCompositorEffect>(
							effect.ptr()) != nullptr) {
				terminal_effects.push_back(effect);
			} else {
				effects.push_back(effect);
			}
		}
	}
	for (const auto &entry : state.effects) {
		bool already_present = false;
		for (int64_t index = 0; index < effects.size(); ++index) {
			Ref<CompositorEffect> existing = effects[index];
			if (existing.is_valid() &&
					existing->get_instance_id() == entry.first) {
				already_present = true;
				break;
			}
		}
		if (!already_present) {
			Ref<CompositorEffect> generic_effect = entry.second;
			effects.push_back(generic_effect);
		}
	}
	// Retail particles are part of the D3D9 gamma framebuffer. FrameFX and
	// the one terminal gamma-to-linear bridge must therefore run after every
	// particle compositor targeting this camera.
	for (int64_t index = 0; index < terminal_effects.size(); ++index) {
		effects.push_back(terminal_effects[index]);
	}
	state.installed.instantiate();
	state.installed->set_compositor_effects(effects);
	camera->set_compositor(state.installed);
}

// A Bump/Bumpadd quad's DIFFUSE channel depends on the view basis, so the
// mirror camera relights these quads in place instead of rebuilding the
// whole snapshot.
struct LitQuadInput {
	std::size_t particle_index = 0;
	float bump_scale = 0.0f;
	float roll = 0.0f;
	std::uint8_t alpha = 255;
};

// Retained FirstPerson upload scratch: one ArrayMesh whose surfaces are
// rebuilt per frame, plus packed arrays whose storage survives across frames.
struct FirstPersonUploadBuffers {
	PackedVector3Array vertices;
	PackedVector2Array uvs;
	PackedColorArray colors;
	PackedByteArray custom0;
	PackedInt32Array indices;
	Array arrays;
};

} // namespace

class ParticleRenderer::Impl {
public:
	MeshInstance3D *first_person_instance = nullptr;
	Ref<ArrayMesh> first_person_mesh;
	FirstPersonUploadBuffers first_person_upload;
	std::array<renderer::ParticleFrameCompiler, kParticleDrawSlotCount> compilers;
	// A slot is present once it compiled for the current render and until
	// clear_draws or a render without its camera retires it.
	std::array<bool, kParticleDrawSlotCount> slot_present{};
	renderer::ParticleFrameSnapshot render_snapshot;
	std::vector<LitQuadInput> lit_quads;
	std::shared_ptr<const std::vector<opennova::particle::ParticleDef>> catalog_definitions;
	std::vector<DefinitionVisual> definition_visuals;
	std::vector<renderer::ParticleAtlasEntry> entries;
	std::vector<AtlasPage> pages;
	std::shared_ptr<const ParticleAtlasSnapshot> atlas_snapshot;
	std::uint64_t atlas_generation = 0;
	std::array<Ref<Shader>, 8> shader_cache;
	std::map<std::uint64_t, Ref<ShaderMaterial>> materials;
	std::vector<std::string> unresolved_names;
	std::size_t rejected_atlas_entries = 0;
	ParticleEffectPair world_effects;
	ParticleEffectPair reflection_effects;
	ObjectID attached_world_camera;
	ObjectID attached_reflection_camera;
	bool inherited_world_compositor = false;
	bool inherited_reflection_compositor = false;
	bool catalog_dirty = true;
	ObjectID cached_environment_source;
	std::int64_t cached_environment_generation =
			std::numeric_limits<std::int64_t>::min();
	std::array<float, 3> fog_color{0.5f, 0.6f, 0.8f};
	float fog_start = 30000.0f;
	float fog_end = 100000.0f;
	std::int32_t fog_type = 1;

	Impl() {
		for (Ref<ParticleCompositorEffect> &effect : world_effects)
			effect.instantiate();
		for (Ref<ParticleCompositorEffect> &effect : reflection_effects)
			effect.instantiate();
		world_effects[0]->set_effect_callback_type(
				CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);
	}

	~Impl() {
		detach_compositors();
	}

	void invalidate_catalog() {
		catalog_dirty = true;
	}

	void invalidate_environment() {
		cached_environment_source = ObjectID();
		cached_environment_generation =
				std::numeric_limits<std::int64_t>::min();
	}

	void refresh_environment(Node *source) {
		MissionEnvironment *env = Object::cast_to<MissionEnvironment>(source);
		const ObjectID source_id = env != nullptr ?
				ObjectID(env->get_instance_id()) : ObjectID();
		std::int64_t generation =
				std::numeric_limits<std::int64_t>::min();
		bool has_generation = false;
		if (env != nullptr) {
			generation = env->get_scene_generation();
			has_generation = true;
		}
		if (has_generation && source_id == cached_environment_source &&
				generation == cached_environment_generation) {
			return;
		}

		// Project defaults keep standalone previews useful. Runtime GameWorld
		// supplies MissionEnvironment explicitly, avoiding RenderingServer global
		// readback (which Godot rejects outside the editor).
		fog_color = {0.5f, 0.6f, 0.8f};
		fog_start = 30000.0f;
		fog_end = 100000.0f;
		fog_type = 1;
		if (env != nullptr) {
			const Vector3 color = env->get_scene_fog_color();
			if (std::isfinite(color.x) && std::isfinite(color.y) &&
					std::isfinite(color.z)) {
				fog_color = {color.x, color.y, color.z};
			}
			auto finite_or = [](float value, float fallback) {
				return std::isfinite(value) ? value : fallback;
			};
			fog_start = finite_or(env->get_scene_fog_start(), fog_start);
			fog_end = finite_or(env->get_scene_fog_end(), fog_end);
			fog_type = std::clamp<std::int32_t>(
					env->get_scene_fog_type(), 0, 3);
		}
		cached_environment_source = source_id;
		cached_environment_generation = has_generation ? generation :
				std::numeric_limits<std::int64_t>::min();
	}

	void detach_compositor_group(ObjectID &attached_camera,
			const ParticleEffectPair &effects, bool &inherited_compositor) {
		if (!attached_camera.is_valid())
			return;
		const std::uint64_t camera_id =
				static_cast<std::uint64_t>(attached_camera);
		auto &states = particle_camera_compositors();
		auto state_it = states.find(camera_id);
		Camera3D *camera = Object::cast_to<Camera3D>(
				ObjectDB::get_instance(camera_id));
		if (state_it != states.end()) {
			ParticleCameraCompositorState &state = state_it->second;
			state.effects.erase(std::remove_if(
					state.effects.begin(), state.effects.end(),
					[&effects](const auto &entry) {
						return std::any_of(effects.begin(), effects.end(),
								[&entry](const Ref<ParticleCompositorEffect> &effect) {
									return effect.is_valid() && entry.first ==
											effect->get_instance_id();
								});
					}), state.effects.end());
			if (state.effects.empty()) {
				if (camera != nullptr &&
						camera->get_compositor() == state.installed) {
					camera->set_compositor(state.explicit_base);
				}
				states.erase(state_it);
			} else if (camera != nullptr &&
					camera->get_compositor() == state.installed) {
				rebuild_camera_compositor(state, camera);
			}
		}
		attached_camera = ObjectID();
		inherited_compositor = false;
	}

	void detach_compositors() {
		detach_compositor_group(attached_world_camera, world_effects,
				inherited_world_compositor);
		detach_compositor_group(attached_reflection_camera, reflection_effects,
				inherited_reflection_compositor);
	}

	void attach_compositor_group(Camera3D *camera, Viewport *viewport,
			ObjectID &attached_camera, const ParticleEffectPair &effects,
			bool &inherited_compositor) {
		if (camera == nullptr) {
			detach_compositor_group(attached_camera, effects,
					inherited_compositor);
			return;
		}
		const std::uint64_t camera_id = camera->get_instance_id();
		const ObjectID camera_object_id(camera_id);
		if (attached_camera.is_valid() &&
				attached_camera != camera_object_id) {
			detach_compositor_group(attached_camera, effects,
					inherited_compositor);
		}

		auto &states = particle_camera_compositors();
		auto [state_it, inserted] = states.try_emplace(camera_id);
		ParticleCameraCompositorState &state = state_it->second;
		bool rebuild = inserted;
		if (inserted) {
			capture_camera_base(state, camera, viewport);
		} else if (camera->get_compositor() != state.installed) {
			// Another subsystem deliberately replaced our composition. Adopt
			// that as the new base and recompose every still-live renderer.
			capture_camera_base(state, camera, viewport);
			rebuild = true;
		} else if (refresh_camera_base(state, viewport)) {
			// Track both resource replacement and same-resource effect-list
			// replacement while our camera override remains installed.
			rebuild = true;
		}

		for (const Ref<ParticleCompositorEffect> &effect : effects) {
			const std::uint64_t effect_id = effect->get_instance_id();
			auto effect_it = std::find_if(state.effects.begin(),
					state.effects.end(), [effect_id](const auto &entry) {
						return entry.first == effect_id;
					});
			if (effect_it == state.effects.end()) {
				state.effects.emplace_back(effect_id, effect);
				rebuild = true;
			} else if (effect_it->second != effect) {
				effect_it->second = effect;
				rebuild = true;
			}
		}
		attached_camera = camera_object_id;
		inherited_compositor = state.inherited_world_compositor;
		if (rebuild)
			rebuild_camera_compositor(state, camera);
	}

	void ensure_visuals(ParticleRenderer *owner) {
		if (first_person_instance != nullptr)
			return;
		first_person_instance = memnew(MeshInstance3D);
		first_person_instance->set_name("ParticleFirstPersonBatch");
		first_person_instance->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		first_person_instance->set_extra_cull_margin(200.0f);
		first_person_instance->set_as_top_level(true);
		first_person_instance->set_transform(Transform3D());
		first_person_instance->set_layer_mask(kFirstPersonVisibilityMask);
		owner->add_child(first_person_instance);
		first_person_instance->set_owner(owner->get_owner());
	}

	void clear_draws() {
		for (Ref<ParticleCompositorEffect> &effect : world_effects)
			effect->clear_submission();
		for (Ref<ParticleCompositorEffect> &effect : reflection_effects)
			effect->clear_submission();
		if (first_person_mesh.is_valid())
			first_person_mesh->clear_surfaces();
		slot_present.fill(false);
		if (first_person_instance != nullptr)
			first_person_instance->set_visible(false);
	}

	bool first_person_has_surfaces() const {
		return first_person_mesh.is_valid() &&
				first_person_mesh->get_surface_count() > 0;
	}

	void rebuild_catalog(
			const std::shared_ptr<const std::vector<opennova::particle::ParticleDef>> &definitions,
			const Callable &provider, const String &texture_dir,
			bool procedural_fallback) {
		catalog_definitions = definitions;
		definition_visuals.clear();
		entries.clear();
		pages.clear();
		materials.clear();
		unresolved_names.clear();
		rejected_atlas_entries = 0;
		catalog_dirty = false;
		renderer::ParticleAtlasBuilder builder;
		if (definitions)
			definition_visuals.resize(definitions->size());
		std::unordered_map<std::string, std::size_t> entry_lookup;
		std::unordered_set<std::string> unresolved_lookup;
		std::unordered_map<int, std::size_t> fallback_entries;
		Ref<Image> fallback_image;

		auto mark_unresolved = [&](const std::string &name) {
			if (name.empty())
				return;
			const std::string key = lower_ascii(name);
			if (unresolved_lookup.insert(key).second)
				unresolved_names.push_back(name);
		};

		auto register_frame = [&](const std::string &name,
				std::uint8_t type) -> std::size_t {
			const std::string key = std::to_string(static_cast<int>(type)) +
					"|" + lower_ascii(name);
			const auto found = entry_lookup.find(key);
			if (found != entry_lookup.end())
				return found->second;

			Ref<Image> image;
			bool used_fallback = false;
			if (!name.empty())
				image = load_particle_image(provider, texture_dir, name);
			if (image.is_null()) {
				mark_unresolved(name);
				if (!procedural_fallback) {
					entry_lookup.emplace(key, kMissingEntry);
					return kMissingEntry;
				}
				const auto fallback = fallback_entries.find(static_cast<int>(type));
				if (fallback != fallback_entries.end()) {
					entry_lookup.emplace(key, fallback->second);
					return fallback->second;
				}
				if (fallback_image.is_null())
					fallback_image = make_fallback_image();
				image = fallback_image;
				used_fallback = true;
			}
			if (image.is_null() || image->get_width() <= 0 ||
					image->get_height() <= 0) {
				entry_lookup.emplace(key, kMissingEntry);
				return kMissingEntry;
			}

			renderer::ParticleRgbaImage rgba = particle_rgba_image(image,
					used_fallback && type == 1);
			if (!rgba.valid()) {
				entry_lookup.emplace(key, kMissingEntry);
				return kMissingEntry;
			}
			const std::size_t index = builder.register_frame(name, type,
					std::move(rgba));
			entry_lookup.emplace(key, index);
			if (name.empty() || used_fallback)
				fallback_entries.emplace(static_cast<int>(type), index);
			return index;
		};

		for (std::size_t definition_index = 0; definitions &&
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
				LayerVisual &layer = visual.layers[static_cast<std::size_t>(layer_index)];
				layer.present = true;
				layer.type = static_cast<std::uint8_t>(graphic.blend_mode);
				layer.flip_frames = std::clamp(graphic.flip_frames, 1,
						opennova::particle::kMaxParticleFlipFrames);
				layer.flip_rate = std::max(0, graphic.flip_rate);
				layer.frame_entries.reserve(static_cast<std::size_t>(layer.flip_frames));
				for (int frame = 1; frame <= layer.flip_frames; ++frame) {
					const std::string name = renderer::retail_particle_frame_name(
							graphic.texture, layer.flip_frames, frame);
					layer.frame_entries.push_back(register_frame(name, layer.type));
				}
			}
			if (!any_present) {
				LayerVisual &layer = visual.layers[0];
				layer.present = true;
				layer.frame_entries.push_back(register_frame(std::string(), 0));
			}
		}

		renderer::ParticleAtlasBuild build = builder.build();
		entries = std::move(build.entries);
		rejected_atlas_entries = build.rejected_entries;
		pages.reserve(build.pages.size());
		auto snapshot = std::make_shared<ParticleAtlasSnapshot>();
		snapshot->generation = ++atlas_generation;
		snapshot->pages.reserve(build.pages.size());
		for (renderer::ParticleAtlasPage &source : build.pages) {
			AtlasPage page;
			page.type = source.type;
			page.side = source.image.width;
			const PackedByteArray pixels = packed_rgba(source.image.rgba);
			Ref<Image> page_image = Image::create_from_data(
					source.image.width, source.image.height, false,
					Image::FORMAT_RGBA8, pixels);
			if (page_image.is_valid())
				page.texture = ImageTexture::create_from_image(page_image);
			pages.push_back(page);

			ParticleAtlasPageSnapshot upload;
			upload.type = source.type;
			upload.side = static_cast<std::uint32_t>(source.image.width);
			upload.rgba8 = pixels;
			snapshot->pages.push_back(std::move(upload));
		}
		atlas_snapshot = std::shared_ptr<const ParticleAtlasSnapshot>(
				std::move(snapshot));
	}

	Ref<Shader> shader_for(renderer::ParticlePipeline pipeline) {
		const std::size_t index = static_cast<std::size_t>(pipeline);
		if (index >= shader_cache.size())
			return Ref<Shader>();
		if (shader_cache[index].is_null()) {
			ResourceLoader *loader = ResourceLoader::get_singleton();
			if (loader != nullptr) {
				Ref<Resource> resource = loader->load(shader_path_for_pipeline(pipeline));
				shader_cache[index] = resource;
			}
		}
		return shader_cache[index];
	}

	Ref<ShaderMaterial> material_for(const renderer::ParticleDrawCommand &command) {
		const std::uint64_t key =
				(static_cast<std::uint64_t>(command.atlas_page) << 24) |
				(static_cast<std::uint64_t>(command.pipeline) << 16) |
				static_cast<std::uint64_t>(command.variant);
		const auto found = materials.find(key);
		if (found != materials.end())
			return found->second;
		Ref<ShaderMaterial> material;
		material.instantiate();
		material->set_shader(shader_for(command.pipeline));
		if (command.atlas_page < pages.size() &&
				pages[command.atlas_page].texture.is_valid()) {
			material->set_shader_parameter("albedo_tex",
					pages[command.atlas_page].texture);
			material->set_shader_parameter("has_texture", true);
		} else {
			material->set_shader_parameter("has_texture", false);
		}
		materials.emplace(key, material);
		return material;
	}

	static std::uint32_t lit_primary_color(const LitQuadInput &lit,
			const Basis &view_basis) {
		// k = 0.5773503 and the transpose(Rx(roll) * view) light rotation
		// (CParticleEmitter_BuildBillboardQuads @0x5e6d60 - docs/particles/ptl-format-re.md).
		constexpr float light_component = 0.5773503f;
		const Vector3 seed = lit.bump_scale *
				Vector3(light_component, light_component, light_component);
		const Basis rotate_x(Vector3(1.0f, 0.0f, 0.0f), lit.roll);
		// Literal retail operation: transpose(Rx(roll) * view).
		const Vector3 light_local =
				(rotate_x * view_basis).transposed().xform(seed);
		// Exact FVF ordering for Bump/Bumpadd: DIFFUSE (Godot COLOR) carries
		// encoded light + particle alpha; SPECULAR (CUSTOM0) keeps the original
		// modulated RGB with opaque alpha.
		return pack_argb_bytes(retail_low_byte(light_local.x),
				retail_low_byte(light_local.y), retail_low_byte(light_local.z),
				lit.alpha);
	}

	// Re-derives only the view-dependent Bump/Bumpadd DIFFUSE channel for a
	// second camera; every other quad input is view-independent.
	void relight_render_snapshot(const Basis &view_basis) {
		for (const LitQuadInput &lit : lit_quads) {
			render_snapshot.particles[lit.particle_index].primary_color =
					lit_primary_color(lit, view_basis);
		}
	}

	// Refills the retained flat snapshot (emitters + one particle run each)
	// in the simulator's order. clear() keeps both vectors' capacity, so a
	// steady-state frame allocates nothing here.
	void build_render_snapshot(
			const opennova::particle::ParticleFrameSnapshot &frame,
			const Basis &view_basis) {
		render_snapshot.frame_id = frame.frame_index;
		render_snapshot.emitters.clear();
		render_snapshot.particles.clear();
		lit_quads.clear();
		if (!frame.definitions ||
				definition_visuals.size() != frame.definitions->size())
			return;
		render_snapshot.emitters.reserve(frame.emitters.size());
		render_snapshot.particles.reserve(frame.particles.size());

		for (const opennova::particle::EffectEmitterFrameSnapshot &source_emitter :
				frame.emitters) {
			if (source_emitter.definition_index >= frame.definitions->size() ||
					source_emitter.definition_index >= definition_visuals.size())
				continue;
			const opennova::particle::ParticleDef &definition =
					(*frame.definitions)[source_emitter.definition_index];
			const DefinitionVisual &visual =
					definition_visuals[source_emitter.definition_index];

			renderer::ParticleEmitterSnapshot emitter;
			emitter.emitter_id = source_emitter.id;
			emitter.position = particle_vec(source_emitter.position);
			emitter.first_particle = render_snapshot.particles.size();
			if (source_emitter.group_index < frame.groups.size() &&
					frame.groups[source_emitter.group_index].render_domain ==
						opennova::particle::EffectRenderDomain::FirstPerson) {
				emitter.domain = renderer::ParticleRenderDomain::FirstPerson;
			} else {
				emitter.domain = renderer::ParticleRenderDomain::World;
			}

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
			const std::size_t count = std::min(source_emitter.particle_count, available);
			const std::uint32_t lod_divisor =
					std::max<std::uint32_t>(1u, source_emitter.lod_divisor);

			for (std::size_t particle_index = 0; particle_index < count;
					++particle_index) {
				const Particle &particle = frame.particles[first + particle_index];
				// EffectScene::inspect uses the simulator's base half-size for
				// manager/emitter bounds. Keep that authoritative sort box even
				// when render LOD omits this particle; draw list output bounds below
				// remain exact expanded quad bounds.
				include_point(emitter.bounds, particle_vec(particle.position),
						particle.size * 0.5f);
				if (lod_divisor > 1u &&
						(static_cast<std::uint32_t>(particle.serial) % lod_divisor) != 0u)
					continue;

				int layer_index = std::clamp<int>(
						static_cast<int>(particle.graphic_layer), 0,
						kGraphicLayerCount - 1);
				if (!visual.layers[static_cast<std::size_t>(layer_index)].present)
					layer_index = fallback_layer;
				const LayerVisual &layer =
						visual.layers[static_cast<std::size_t>(layer_index)];
				const GraphicLayer &graphic =
						definition.graphics[static_cast<std::size_t>(layer_index)];

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
								static_cast<std::size_t>(std::min(lut_index + 1, 255))];
						const float interpolated = static_cast<float>(first_sample) +
								(static_cast<float>(second_sample) -
										static_cast<float>(first_sample)) * lut_fraction;
						scale_multiplier = interpolated / 128.0f;
					}
				}
				const float size = particle.size * scale_multiplier;

				constexpr float byte_to_unit = 1.0f / 255.0f;
				float red = static_cast<float>(particle.color.r) * byte_to_unit;
				float green = static_cast<float>(particle.color.g) * byte_to_unit;
				float blue = static_cast<float>(particle.color.b) * byte_to_unit;
				red = std::clamp(red * red_multiplier * source_emitter.color_tint.x,
						0.0f, 1.0f);
				green = std::clamp(green * green_multiplier * source_emitter.color_tint.y,
						0.0f, 1.0f);
				blue = std::clamp(blue * blue_multiplier * source_emitter.color_tint.z,
						0.0f, 1.0f);
				const float alpha = std::clamp(
						static_cast<float>(particle.alpha) * byte_to_unit *
								alpha_multiplier,
						0.0f, 1.0f);

				// Flipbook clock: frame = int(4 * flip_rate * elapsed seconds)
				// wrapped over the frame count, with GFXFLIPRAND offsetting by
				// a serial-derived start frame [orig:
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
							4.0f * static_cast<float>(layer.flip_rate) * elapsed)) %
							layer.flip_frames;
				}

				renderer::ParticleQuadSnapshot quad;
				quad.center = particle_vec(particle.position);
				quad.half_width = size * 0.5f;
				quad.half_height = size * 0.5f;
				quad.roll = particle.rotation;
				quad.yaw = particle.yaw;
				quad.pitch = particle.pitch;
				quad.camera_pull = definition.z_offset;
				quad.primary_color = pack_argb(red, green, blue, alpha);
				quad.alignment = (definition.flags &
						opennova::particle::particle_flag::YawAndPitch) != 0 ?
						renderer::ParticleAlignment::WorldOriented :
						renderer::ParticleAlignment::CameraFacing;
				quad.state.pipeline = static_cast<renderer::ParticlePipeline>(layer.type);
				quad.state.pass = layer.type == 7 ?
						renderer::ParticleRenderPass::Distortion :
						renderer::ParticleRenderPass::Color;

				std::size_t entry_index = kMissingEntry;
				if (!layer.frame_entries.empty()) {
					entry_index = layer.frame_entries[
							static_cast<std::size_t>(std::max(frame_index, 0)) %
							layer.frame_entries.size()];
				}
				quad.visible = entry_index != kMissingEntry &&
						entry_index < entries.size() &&
						entries[entry_index].placement.valid;
				if (quad.visible) {
					const renderer::ParticleAtlasEntry &entry = entries[entry_index];
					quad.state.atlas.page = entry.placement.page;
					quad.state.atlas.type = entry.type;
					quad.state.atlas.rect = entry.placement.rect;
					quad.state.atlas.inset_u = entry.placement.inset_u;
					quad.state.atlas.inset_v = entry.placement.inset_v;
				}

				if ((particle.flags & LitColor) != 0) {
					LitQuadInput lit;
					lit.particle_index = render_snapshot.particles.size();
					lit.bump_scale = definition.bump_scale;
					lit.roll = particle.rotation;
					lit.alpha = unit_byte(alpha);
					quad.primary_color = lit_primary_color(lit, view_basis);
					quad.secondary_color = pack_argb(red, green, blue, 1.0f);
					lit_quads.push_back(lit);
				} else {
					quad.secondary_color = 0xffffffffu;
				}

				render_snapshot.particles.push_back(quad);
			}
			emitter.particle_count =
					render_snapshot.particles.size() - emitter.first_particle;
			render_snapshot.emitters.push_back(emitter);
		}
	}

	void publish_world_draw_list(const Ref<ParticleCompositorEffect> &effect,
			const renderer::ParticleDrawList &draw_list,
			const Vector3 &camera_position, const Vector3 &camera_forward) {
		auto submission = std::make_shared<ParticleWorldSubmission>();
		submission->frame_id = draw_list.frame_id;
		submission->commands = draw_list.commands;
		submission->atlas = atlas_snapshot;
		for (std::size_t component = 0; component < 3; ++component) {
			submission->camera_position[component] =
					camera_position[static_cast<int>(component)];
			submission->camera_forward[component] =
					camera_forward[static_cast<int>(component)];
		}
		submission->fog_color = fog_color;
		submission->fog_start = fog_start;
		submission->fog_end = fog_end;
		submission->fog_type = fog_type;
		if (draw_list.domain != renderer::ParticleRenderDomain::World) {
			submission->valid = false;
			submission->validation_error =
					"World backend received a non-World compiler draw_list";
		} else if (draw_list.vertices.size() % 4u != 0) {
			submission->valid = false;
			submission->validation_error =
					"Compiler draw_list does not contain complete particle quads";
		} else {
			const std::size_t quad_count = draw_list.vertices.size() / 4u;
			constexpr std::size_t expanded_bytes_per_quad =
					6u * sizeof(renderer::ParticleVertex);
			if (quad_count > static_cast<std::size_t>(
					std::numeric_limits<int64_t>::max()) /
					expanded_bytes_per_quad) {
				submission->valid = false;
				submission->validation_error =
						"Expanded World draw_list exceeds PackedByteArray limits";
			} else {
				submission->triangle_vertices.resize(static_cast<int64_t>(
						quad_count * expanded_bytes_per_quad));
				std::uint8_t *destination =
						submission->triangle_vertices.ptrw();
				// Retail's quad index pattern 0/1/2/1/3/2 (CParticleEmitter_BuildBillboardQuads
				// @0x5e6d60 - docs/particles/ptl-format-re.md): the first triangle
				// is the quad's contiguous first three vertices, so it copies
				// as one block.
				constexpr std::size_t stride = sizeof(renderer::ParticleVertex);
				for (std::size_t quad = 0; quad < quad_count; ++quad) {
					const renderer::ParticleVertex *source =
							draw_list.vertices.data() + quad * 4u;
					std::uint8_t *target = destination +
							quad * expanded_bytes_per_quad;
					std::memcpy(target, source, 3u * stride);
					std::memcpy(target + 3u * stride, source + 1, stride);
					std::memcpy(target + 4u * stride, source + 3, stride);
					std::memcpy(target + 5u * stride, source + 2, stride);
				}
			}
		}
		effect->publish(
				std::shared_ptr<const ParticleWorldSubmission>(
						std::move(submission)));
	}

	void upload_first_person_draw_list(
			const renderer::ParticleDrawList &draw_list, bool hidden) {
		if (first_person_instance == nullptr)
			return;
		// One retained ArrayMesh: surfaces are rebuilt per frame while the mesh
		// RID, the instance binding, and the packed scratch arrays survive.
		if (first_person_mesh.is_null())
			first_person_mesh.instantiate();
		if (first_person_instance->get_mesh().ptr() != first_person_mesh.ptr())
			first_person_instance->set_mesh(first_person_mesh);
		first_person_mesh->clear_surfaces();
		if (draw_list.commands.empty() || draw_list.vertices.empty()) {
			first_person_instance->set_visible(false);
			return;
		}

		FirstPersonUploadBuffers &upload = first_person_upload;
		if (upload.arrays.size() != Mesh::ARRAY_MAX)
			upload.arrays.resize(Mesh::ARRAY_MAX);
		for (const renderer::ParticleDrawCommand &command : draw_list.commands) {
			const std::size_t first_vertex =
					static_cast<std::size_t>(command.first_quad) * 4u;
			const std::size_t vertex_count =
					static_cast<std::size_t>(command.quad_count) * 4u;
			if (first_vertex > draw_list.vertices.size() ||
					vertex_count > draw_list.vertices.size() - first_vertex)
				continue;

			upload.vertices.resize(static_cast<int64_t>(vertex_count));
			upload.uvs.resize(static_cast<int64_t>(vertex_count));
			upload.colors.resize(static_cast<int64_t>(vertex_count));
			upload.custom0.resize(static_cast<int64_t>(vertex_count * 4u));
			upload.indices.resize(static_cast<int64_t>(command.quad_count) * 6);
			Vector3 *vertices = upload.vertices.ptrw();
			Vector2 *uvs = upload.uvs.ptrw();
			Color *colors = upload.colors.ptrw();
			std::uint8_t *custom_bytes = upload.custom0.ptrw();
			std::int32_t *indices = upload.indices.ptrw();

			for (std::size_t vertex_index = 0; vertex_index < vertex_count;
					++vertex_index) {
				const renderer::ParticleVertex &source =
						draw_list.vertices[first_vertex + vertex_index];
				vertices[vertex_index] = Vector3(source.x, source.y, source.z);
				uvs[vertex_index] = Vector2(source.u, source.v);
				colors[vertex_index] = unpack_argb(source.primary_color);
				const std::size_t byte_offset = vertex_index * 4u;
				custom_bytes[byte_offset + 0] = static_cast<std::uint8_t>(
						(source.secondary_color >> 16) & 0xffu);
				custom_bytes[byte_offset + 1] = static_cast<std::uint8_t>(
						(source.secondary_color >> 8) & 0xffu);
				custom_bytes[byte_offset + 2] = static_cast<std::uint8_t>(
						source.secondary_color & 0xffu);
				custom_bytes[byte_offset + 3] = static_cast<std::uint8_t>(
						(source.secondary_color >> 24) & 0xffu);
			}

			for (std::uint32_t quad = 0; quad < command.quad_count; ++quad) {
				const std::int32_t vertex = static_cast<std::int32_t>(quad * 4u);
				const std::size_t index = static_cast<std::size_t>(quad) * 6u;
				indices[index + 0] = vertex + 0;
				indices[index + 1] = vertex + 1;
				indices[index + 2] = vertex + 2;
				indices[index + 3] = vertex + 1;
				indices[index + 4] = vertex + 3;
				indices[index + 5] = vertex + 2;
			}

			upload.arrays[Mesh::ARRAY_VERTEX] = upload.vertices;
			upload.arrays[Mesh::ARRAY_TEX_UV] = upload.uvs;
			upload.arrays[Mesh::ARRAY_COLOR] = upload.colors;
			upload.arrays[Mesh::ARRAY_CUSTOM0] = upload.custom0;
			upload.arrays[Mesh::ARRAY_INDEX] = upload.indices;
			const std::uint64_t flags =
					static_cast<std::uint64_t>(Mesh::ARRAY_FORMAT_CUSTOM0);
			first_person_mesh->add_surface_from_arrays(
					Mesh::PRIMITIVE_TRIANGLES, upload.arrays,
					TypedArray<Array>(), Dictionary(),
					static_cast<Mesh::ArrayFormat>(flags));
			// The surface owns a converted copy. Drop the Array's shares so the
			// next frame's ptrw() writes in place instead of copying on write.
			upload.arrays.fill(Variant());
			const int surface = first_person_mesh->get_surface_count() - 1;
			first_person_mesh->surface_set_material(surface, material_for(command));
		}

		first_person_instance->set_visible(
				!hidden && first_person_mesh->get_surface_count() > 0);
	}
};

ParticleRenderer::ParticleRenderer() : impl_(std::make_unique<Impl>()) {}

ParticleRenderer::~ParticleRenderer() = default;

String ParticleRenderer::retail_frame_name(const String &p_authored,
		int p_frame_count, int p_frame) {
	return String(renderer::retail_particle_frame_name(
			std::string(p_authored.utf8().get_data()), p_frame_count, p_frame)
					.c_str());
}

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
	ClassDB::bind_method(D_METHOD("set_environment_source", "source"),
			&ParticleRenderer::set_environment_source);
	ClassDB::bind_method(D_METHOD("get_environment_source"),
			&ParticleRenderer::get_environment_source);
	ClassDB::bind_method(D_METHOD("set_water_plane", "height", "reflection_camera"),
			&ParticleRenderer::set_water_plane);
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
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment_source",
			PROPERTY_HINT_NODE_TYPE, "Node"),
			"set_environment_source", "get_environment_source");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hidden"),
			"set_hidden", "get_hidden");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "procedural_fallback_enabled"),
			"set_procedural_fallback_enabled",
			"get_procedural_fallback_enabled");
}

void ParticleRenderer::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		impl_->ensure_visuals(this);
		set_process(false);
		render_now();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (impl_)
			impl_->detach_compositors();
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

void ParticleRenderer::set_environment_source(Node *p_source) {
	const ObjectID next = p_source != nullptr ?
			ObjectID(p_source->get_instance_id()) : ObjectID();
	if (environment_source_ == next)
		return;
	environment_source_ = next;
	if (impl_)
		impl_->invalidate_environment();
}

Node *ParticleRenderer::get_environment_source() const {
	if (!environment_source_.is_valid())
		return nullptr;
	return Object::cast_to<Node>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(environment_source_)));
}

void ParticleRenderer::set_water_plane(float p_height,
		Camera3D *p_reflection_camera) {
	water_height_ = p_height;
	reflection_camera_ = p_reflection_camera != nullptr ?
			ObjectID(p_reflection_camera->get_instance_id()) : ObjectID();
}

void ParticleRenderer::warm_pipelines(const Vector3 &p_position) {
	clear_warm_pipelines();
	if (!impl_)
		return;
	for (Ref<ParticleCompositorEffect> &effect : impl_->world_effects)
		effect->request_pipeline_warm();
	for (Ref<ParticleCompositorEffect> &effect : impl_->reflection_effects)
		effect->request_pipeline_warm();
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(0.01f, 0.01f));
	for (std::size_t i = 0; i < impl_->shader_cache.size(); ++i) {
		Ref<Shader> shader =
				impl_->shader_for(static_cast<renderer::ParticlePipeline>(i));
		if (shader.is_null())
			continue;
		Ref<ShaderMaterial> material;
		material.instantiate();
		material->set_shader(shader);
		MeshInstance3D *quad_instance = memnew(MeshInstance3D);
		quad_instance->set_mesh(quad);
		quad_instance->set_material_override(material);
		quad_instance->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(quad_instance);
		quad_instance->set_global_position(p_position);
		warm_nodes_.push_back(quad_instance);
	}
}

void ParticleRenderer::clear_warm_pipelines() {
	if (impl_) {
		for (Ref<ParticleCompositorEffect> &effect : impl_->world_effects)
			effect->cancel_pipeline_warm();
		for (Ref<ParticleCompositorEffect> &effect : impl_->reflection_effects)
			effect->cancel_pipeline_warm();
	}
	for (Node *node : warm_nodes_) {
		if (node != nullptr)
			node->queue_free();
	}
	warm_nodes_.clear();
}

void ParticleRenderer::set_hidden(bool p_hidden) {
	if (hidden_ == p_hidden)
		return;
	hidden_ = p_hidden;
	if (!impl_)
		return;
	for (Ref<ParticleCompositorEffect> &effect : impl_->world_effects)
		effect->set_particles_hidden(hidden_);
	for (Ref<ParticleCompositorEffect> &effect : impl_->reflection_effects)
		effect->set_particles_hidden(hidden_);
	if (hidden_) {
		// The master switch is also a CPU switch: drop retained submissions once
		// and let the next visible process rebuild from the latest scene frame.
		impl_->clear_draws();
		return;
	}
	if (impl_->first_person_instance != nullptr) {
		impl_->first_person_instance->set_visible(
				!hidden_ && impl_->first_person_has_surfaces());
	}
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
	impl_->ensure_visuals(this);
	if (hidden_)
		return;
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	Camera3D *reflection_camera = reflection_camera_.is_valid() ?
			Object::cast_to<Camera3D>(ObjectDB::get_instance(
					static_cast<std::uint64_t>(reflection_camera_))) : nullptr;
	if (reflection_camera == camera)
		reflection_camera = nullptr;
	impl_->attach_compositor_group(camera, viewport,
			impl_->attached_world_camera, impl_->world_effects,
			impl_->inherited_world_compositor);
	impl_->attach_compositor_group(reflection_camera,
			reflection_camera != nullptr ? reflection_camera->get_viewport() : nullptr,
			impl_->attached_reflection_camera, impl_->reflection_effects,
			impl_->inherited_reflection_compositor);
	if (scene_.is_null()) {
		impl_->clear_draws();
		return;
	}

	const opennova::particle::ParticleFrameSnapshot &frame =
			scene_->native_frame_snapshot();
	if (impl_->catalog_dirty ||
			impl_->catalog_definitions.get() != frame.definitions.get()) {
		impl_->rebuild_catalog(frame.definitions, texture_provider_, texture_dir_,
				procedural_fallback_enabled_);
	}

	const ParticleCameraFrame world_camera = particle_camera_frame(camera);
	const bool camera_above_water = world_camera.position.y >= water_height_;
	impl_->refresh_environment(get_environment_source());
	impl_->build_render_snapshot(frame, world_camera.view_basis);
	auto compile_world = [&](ParticleDrawSlot slot,
			renderer::ParticleWaterSubset subset,
			const ParticleCameraFrame &view_camera,
			const Ref<ParticleCompositorEffect> &effect) {
		renderer::ParticleViewInput view;
		view.domain = renderer::ParticleRenderDomain::World;
		view.water_subset = subset;
		view.water_height = water_height_;
		view.position = particle_vec(view_camera.position);
		view.right = particle_vec(view_camera.right);
		view.up = particle_vec(view_camera.up);
		view.forward = particle_vec(view_camera.forward);
		std::copy(view_camera.projection.begin(), view_camera.projection.end(),
				view.projection);
		view.projection_valid = view_camera.projection_valid;
		view.projection_near_is_one = true;
		const renderer::ParticleDrawList &draw_list =
				impl_->compilers[slot].compile(impl_->render_snapshot, view);
		impl_->slot_present[slot] = true;
		impl_->publish_world_draw_list(effect, draw_list, view_camera.position,
				view_camera.forward);
	};

	compile_world(kWorldFarSide,
			renderer::particle_water_subset_for_side(camera_above_water, false),
			world_camera, impl_->world_effects[0]);
	compile_world(kWorldCameraSide,
			renderer::particle_water_subset_for_side(camera_above_water, true),
			world_camera, impl_->world_effects[1]);

	renderer::ParticleViewInput first_person_view;
	first_person_view.domain = renderer::ParticleRenderDomain::FirstPerson;
	first_person_view.water_subset = renderer::ParticleWaterSubset::All;
	first_person_view.position = particle_vec(world_camera.position);
	first_person_view.right = particle_vec(world_camera.right);
	first_person_view.up = particle_vec(world_camera.up);
	first_person_view.forward = particle_vec(world_camera.forward);
	std::copy(world_camera.projection.begin(), world_camera.projection.end(),
			first_person_view.projection);
	first_person_view.projection_valid = world_camera.projection_valid;
	first_person_view.projection_near_is_one = true;
	const renderer::ParticleDrawList &first_person_draw =
			impl_->compilers[kFirstPerson].compile(impl_->render_snapshot,
					first_person_view);
	impl_->slot_present[kFirstPerson] = true;
	impl_->upload_first_person_draw_list(first_person_draw, hidden_);

	if (reflection_camera != nullptr) {
		const ParticleCameraFrame mirror_camera =
				particle_camera_frame(reflection_camera);
		// LitColor/Bump channels transform through the active view basis while
		// the water selector remains the emitter's main-camera side. Relight
		// only those quads for the mirror before its two consecutive passes;
		// the World draw lists above already hold their own vertex copies.
		impl_->relight_render_snapshot(mirror_camera.view_basis);
		compile_world(kReflectionFarSide,
				renderer::particle_water_subset_for_side(
						camera_above_water, false),
				mirror_camera, impl_->reflection_effects[0]);
		compile_world(kReflectionCameraSide,
				renderer::particle_water_subset_for_side(
						camera_above_water, true),
				mirror_camera, impl_->reflection_effects[1]);
	} else {
		impl_->reflection_effects[0]->clear_submission();
		impl_->reflection_effects[1]->clear_submission();
		impl_->slot_present[kReflectionFarSide] = false;
		impl_->slot_present[kReflectionCameraSide] = false;
	}
}

int64_t ParticleRenderer::get_rendered_quad_count() const {
	if (!impl_)
		return 0;
	std::size_t total = 0;
	for (const ParticleDrawSlot slot : {
			kWorldFarSide, kWorldCameraSide, kFirstPerson}) {
		if (impl_->slot_present[slot])
			total += impl_->compilers[slot].draw_list().debug.emitted_quads;
	}
	return static_cast<int64_t>(total);
}

int64_t ParticleRenderer::get_draw_command_count() const {
	if (!impl_)
		return 0;
	std::size_t total = 0;
	for (const ParticleDrawSlot slot : {
			kWorldFarSide, kWorldCameraSide, kFirstPerson}) {
		if (impl_->slot_present[slot])
			total += impl_->compilers[slot].draw_list().debug.draw_commands;
	}
	return static_cast<int64_t>(total);
}

Dictionary ParticleRenderer::get_debug_draw_list_report() const {
	Dictionary result;
	if (!impl_)
		return result;
	auto slot_report = [this](ParticleDrawSlot slot) {
		return impl_->slot_present[slot] ?
				draw_list_report(impl_->compilers[slot].draw_list()) :
				Dictionary();
	};
	result["world_far_side"] = slot_report(kWorldFarSide);
	result["world_camera_side"] = slot_report(kWorldCameraSide);
	result["reflection_far_side"] = slot_report(kReflectionFarSide);
	result["reflection_camera_side"] = slot_report(kReflectionCameraSide);
	result["first_person"] = slot_report(kFirstPerson);
	result["world_far_backend"] =
			impl_->world_effects[0]->get_backend_report();
	result["world_camera_backend"] =
			impl_->world_effects[1]->get_backend_report();
	result["reflection_far_backend"] =
			impl_->reflection_effects[0]->get_backend_report();
	result["reflection_camera_backend"] =
			impl_->reflection_effects[1]->get_backend_report();
	result["first_person_backend"] = "array_mesh_fallback_tool_only";
	result["world_compositor_attached"] =
			impl_->attached_world_camera.is_valid();
	result["reflection_compositor_attached"] =
			impl_->attached_reflection_camera.is_valid();
	result["world_compositor_inherited_effects"] =
			impl_->inherited_world_compositor;
	result["reflection_compositor_inherited_effects"] =
			impl_->inherited_reflection_compositor;
	result["world_mesh_instance"] = false;
	result["water_height"] = water_height_;
	Dictionary environment_fog;
	environment_fog["color"] = Vector3(impl_->fog_color[0],
			impl_->fog_color[1], impl_->fog_color[2]);
	environment_fog["start"] = impl_->fog_start;
	environment_fog["end"] = impl_->fog_end;
	environment_fog["type"] = impl_->fog_type;
	result["environment_fog"] = environment_fog;
	result["atlas_page_count"] = static_cast<int64_t>(impl_->pages.size());
	result["atlas_entry_count"] = static_cast<int64_t>(impl_->entries.size());
	std::size_t resolved_entries = 0;
	for (const renderer::ParticleAtlasEntry &entry : impl_->entries) {
		if (entry.placement.valid)
			++resolved_entries;
	}
	result["atlas_resolved_entry_count"] =
			static_cast<int64_t>(resolved_entries);
	result["atlas_rejected_entry_count"] =
			static_cast<int64_t>(impl_->rejected_atlas_entries);
	Array atlas_pages;
	atlas_pages.resize(static_cast<int64_t>(impl_->pages.size()));
	for (std::size_t i = 0; i < impl_->pages.size(); ++i) {
		Dictionary value;
		value["page"] = static_cast<int64_t>(i);
		value["type"] = static_cast<int>(impl_->pages[i].type);
		value["side"] = impl_->pages[i].side;
		atlas_pages[static_cast<int64_t>(i)] = value;
	}
	result["atlas_pages"] = atlas_pages;
	Array atlas_entries;
	atlas_entries.resize(static_cast<int64_t>(impl_->entries.size()));
	for (std::size_t i = 0; i < impl_->entries.size(); ++i) {
		const renderer::ParticleAtlasEntry &entry = impl_->entries[i];
		Dictionary value;
		value["name"] = String::utf8(entry.name.c_str());
		value["type"] = static_cast<int>(entry.type);
		value["width"] = entry.width;
		value["height"] = entry.height;
		value["resolved"] = entry.placement.valid;
		if (entry.placement.valid) {
			value["page"] = static_cast<int64_t>(entry.placement.page);
			value["pixel_rect"] = Rect2i(entry.placement.x, entry.placement.y,
					entry.width, entry.height);
			// Preserve the scalar diagnostics key as the horizontal inset while
			// exposing the per-axis values used for tiny atlas frames.
			value["inset"] = entry.placement.inset_u;
			value["inset_u"] = entry.placement.inset_u;
			value["inset_v"] = entry.placement.inset_v;
		}
		atlas_entries[static_cast<int64_t>(i)] = value;
	}
	result["atlas_entries"] = atlas_entries;
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
	for (const ParticleDrawSlot slot : {
			kWorldFarSide, kWorldCameraSide, kFirstPerson}) {
		if (!impl_->slot_present[slot])
			continue;
		for (const renderer::ParticleEmitterDrawBounds &bounds :
				impl_->compilers[slot].draw_list().emitter_bounds) {
			Dictionary value;
			value["emitter_id"] = godot_token(bounds.emitter_id);
			value["render_domain"] = slot == kFirstPerson ?
					static_cast<int>(renderer::ParticleRenderDomain::FirstPerson) :
					static_cast<int>(renderer::ParticleRenderDomain::World);
			value["draw_scope"] = slot == kWorldFarSide ?
					String("world_far_side") :
					(slot == kWorldCameraSide ? String("world_camera_side") :
							String("first_person"));
			value["first_quad"] = static_cast<int64_t>(bounds.first_quad);
			value["quad_count"] = static_cast<int64_t>(bounds.quad_count);
			value["bounds_valid"] = bounds.bounds.valid;
			value["bounds"] = bounds.bounds.valid ? godot_aabb(bounds.bounds) : AABB();
			result.push_back(value);
		}
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
