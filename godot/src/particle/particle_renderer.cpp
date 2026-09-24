#include "particle/particle_renderer.h"
#include "world/game_world.h"

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
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
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
#include "particle/particle_convert.h"
#include "render/frame_fx.h"
#include "render/visual_layers.h"
#include "render/world_environment_lookup.h"
#include "util/texture_path_resolver.h"
#include "util/string_convert.h"

using namespace godot;

namespace {

constexpr int kGraphicLayerCount = 4;
// The FirstPerson batch rides the viewmodel layer, so every view that masks the
// viewmodel out (the water mirror, the second scene view) drops it as well.
constexpr std::uint32_t kFirstPersonVisibilityMask = visual_layers::VIEWMODEL;
static_assert((visual_layers::SECOND_SCENE_VIEW_EXCLUDED & kFirstPersonVisibilityMask) != 0,
		"a second scene view must never draw the FirstPerson particle batch");
constexpr std::size_t kMissingEntry = std::numeric_limits<std::size_t>::max();

enum ParticleDrawSlot : std::size_t {
	kWorldFarSide = 0,
	kWorldCameraSide = 1,
	kReflectionFarSide = 2,
	kReflectionCameraSide = 3,
	kSecondSceneFarSide = 4,
	kSecondSceneCameraSide = 5,
	kFirstPerson = 6,
	kParticleDrawSlotCount = 7,
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
	const String candidate = opennova::to_gd(name);
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
	// AMBIENTCOLOR authored, or forced by a blend-mode-0 graphic at Initialize
	// (retail CEffectEmitter_Initialize @ 0x5e6038..0x5e605e).
	bool ambient_lit = false;
};

opennova::renderer::ParticleRgbaImage particle_rgba_image(const Ref<Image> &image,
		bool type_one_procedural_mask) {
	opennova::renderer::ParticleRgbaImage result;
	if (image.is_null() || image->get_width() <= 0 || image->get_height() <= 0)
		return result;
	result.width = image->get_width();
	result.height = image->get_height();
	const PackedByteArray pixels = image->get_data();
	const std::size_t width = static_cast<std::size_t>(result.width);
	const std::size_t height = static_cast<std::size_t>(result.height);
	if (width > std::numeric_limits<std::size_t>::max() / height)
		return opennova::renderer::ParticleRgbaImage{};
	const std::size_t pixel_count = width * height;
	if (pixel_count > std::numeric_limits<std::size_t>::max() / 4u)
		return opennova::renderer::ParticleRgbaImage{};
	const std::size_t mip0_bytes = pixel_count * 4u;
	if (mip0_bytes > static_cast<std::size_t>(
			std::numeric_limits<int64_t>::max()) ||
			pixels.size() < static_cast<int64_t>(mip0_bytes))
		return opennova::renderer::ParticleRgbaImage{};
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

String shader_path_for_pipeline(opennova::renderer::ParticlePipeline pipeline) {
	switch (pipeline) {
		case opennova::renderer::ParticlePipeline::Blend:
			return "res://shaders/particle/particle_blend_blend.gdshader";
		case opennova::renderer::ParticlePipeline::Additive:
			return "res://shaders/particle/particle_blend_additive.gdshader";
		case opennova::renderer::ParticlePipeline::Premult:
			return "res://shaders/particle/particle_blend_premult.gdshader";
		case opennova::renderer::ParticlePipeline::Bump:
			return "res://shaders/particle/particle_blend_bump.gdshader";
		case opennova::renderer::ParticlePipeline::Mod:
			return "res://shaders/particle/particle_blend_mod.gdshader";
		case opennova::renderer::ParticlePipeline::Mod2x:
			return "res://shaders/particle/particle_blend_mod2x.gdshader";
		case opennova::renderer::ParticlePipeline::Bumpadd:
			return "res://shaders/particle/particle_blend_bumpadd.gdshader";
		case opennova::renderer::ParticlePipeline::Distort:
			return "res://shaders/particle/particle_blend_distort.gdshader";
	}
	return "res://shaders/particle/particle_blend_blend.gdshader";
}

std::uint8_t unit_byte(float value) {
	return opennova::renderer::particle_unit_byte(value);
}

std::uint32_t pack_argb(float red, float green, float blue, float alpha) {
	return (static_cast<std::uint32_t>(unit_byte(alpha)) << 24) |
			(static_cast<std::uint32_t>(unit_byte(red)) << 16) |
			(static_cast<std::uint32_t>(unit_byte(green)) << 8) |
			static_cast<std::uint32_t>(unit_byte(blue));
}

Color unpack_argb(std::uint32_t value) {
	constexpr float inv = 1.0f / 255.0f;
	return Color(
			static_cast<float>((value >> 16) & 0xffu) * inv,
			static_cast<float>((value >> 8) & 0xffu) * inv,
			static_cast<float>(value & 0xffu) * inv,
			static_cast<float>((value >> 24) & 0xffu) * inv);
}

opennova::renderer::ParticleVec3 particle_vec(const opennova::particle::Vec3 &value) {
	return {value.x, value.y, value.z};
}

opennova::renderer::ParticleVec3 particle_vec(const Vector3 &value) {
	return {value.x, value.y, value.z};
}

void include_point(opennova::renderer::ParticleAabb &bounds,
		const opennova::renderer::ParticleVec3 &point, float radius) {
	const opennova::renderer::ParticleVec3 minimum{
		point.x - radius, point.y - radius, point.z - radius};
	const opennova::renderer::ParticleVec3 maximum{
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

// One comparable value for "what this slot draws": FNV-1a over the compiled
// vertex bytes (position, both colour words, UV; the 28-byte vertex has no
// padding). Two slots, or two renders of one slot, draw the same quads exactly
// when it matches. Computed only when a report is asked for.
std::uint64_t vertex_checksum(
		const std::vector<opennova::renderer::ParticleVertex> &vertices) {
	std::uint64_t hash = 14695981039346656037ull;
	const unsigned char *bytes =
			reinterpret_cast<const unsigned char *>(vertices.data());
	const std::size_t size =
			vertices.size() * sizeof(opennova::renderer::ParticleVertex);
	for (std::size_t index = 0; index < size; ++index) {
		hash ^= bytes[index];
		hash *= 1099511628211ull;
	}
	return hash;
}

// Diagnostics read each compiler's retained draw list in place: it stays
// valid until that slot compiles again and nothing on the render thread
// references it (World submissions are immutable copies), so no per-frame
// capture copy and no opt-in flag are needed for the first report to be live.
Dictionary draw_list_report(const opennova::renderer::ParticleDrawList &draw_list) {
	Dictionary result;
	const opennova::renderer::ParticleFrameDebugCounters &debug = draw_list.debug;
	result["frame_id"] = token_to_godot(draw_list.frame_id);
	result["vertex_checksum"] = token_to_godot(vertex_checksum(draw_list.vertices));
	result["compile_index"] = token_to_godot(debug.compile_index);
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
			token_to_godot(debug.lifetime_capacity_growths);
	result["vertex_capacity"] = static_cast<int64_t>(debug.vertex_capacity);
	result["command_capacity"] = static_cast<int64_t>(debug.command_capacity);
	result["emitter_bounds_capacity"] =
			static_cast<int64_t>(debug.emitter_bounds_capacity);
	Array commands;
	commands.resize(static_cast<int64_t>(draw_list.commands.size()));
	for (std::size_t i = 0; i < draw_list.commands.size(); ++i) {
		const opennova::renderer::ParticleDrawCommand &command = draw_list.commands[i];
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

// The viewport whose WorldEnvironment a second scene camera inherits. That
// camera renders the main view's World3D from a sibling SubViewport holding no
// WorldEnvironment of its own, and a camera without a compositor resolves the
// scenario's, so the chain to recompose around its particle pair (FrameFX and
// the display transfer last) is the main view's. A camera in a world of its
// own keeps its own viewport.
Viewport *second_scene_base_viewport(Camera3D *camera, Viewport *main_viewport) {
	Viewport *own_viewport = camera->get_viewport();
	if (own_viewport == nullptr || main_viewport == nullptr ||
			own_viewport == main_viewport)
		return own_viewport;
	return own_viewport->find_world_3d() == main_viewport->find_world_3d() ?
			main_viewport : own_viewport;
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

// A Bump/Bumpadd quad's DIFFUSE channel depends on the view basis, so every
// secondary view (the mirror, the second scene view) relights these quads in
// place instead of rebuilding the whole snapshot.
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
	std::array<opennova::renderer::ParticleFrameCompiler, kParticleDrawSlotCount> compilers;
	// A slot is present once it compiled for the current render and until
	// clear_draws or a render without its camera retires it.
	std::array<bool, kParticleDrawSlotCount> slot_present{};
	opennova::renderer::ParticleFrameSnapshot render_snapshot;
	std::vector<LitQuadInput> lit_quads;
	std::shared_ptr<const std::vector<opennova::particle::ParticleDef>> catalog_definitions;
	std::vector<DefinitionVisual> definition_visuals;
	std::vector<opennova::renderer::ParticleAtlasEntry> entries;
	std::vector<AtlasPage> pages;
	std::shared_ptr<const ParticleAtlasSnapshot> atlas_snapshot;
	std::uint64_t atlas_generation = 0;
	std::array<Ref<Shader>, 8> shader_cache;
	std::map<std::uint64_t, Ref<ShaderMaterial>> materials;
	std::vector<std::string> unresolved_names;
	std::size_t rejected_atlas_entries = 0;
	ParticleEffectPair world_effects;
	ParticleEffectPair reflection_effects;
	ParticleEffectPair second_scene_effects;
	ObjectID attached_world_camera;
	ObjectID attached_reflection_camera;
	ObjectID attached_second_scene_camera;
	bool inherited_world_compositor = false;
	bool inherited_reflection_compositor = false;
	bool inherited_second_scene_compositor = false;
	bool catalog_dirty = true;
	ObjectID cached_environment_source;
	std::int64_t cached_environment_generation =
			std::numeric_limits<std::int64_t>::min();
	std::array<float, 3> fog_color{0.5f, 0.6f, 0.8f};
	float fog_start = 30000.0f;
	float fog_end = 100000.0f;
	std::int32_t fog_type = 1;
	// The water mirror pair's fog: the reflected scene's particle passes run
	// under the dry weather block whatever the main pass selects
	// (EnvironmentState::build_water_mirror_fog).
	std::array<float, 3> mirror_fog_color{0.5f, 0.6f, 0.8f};
	float mirror_fog_start = 30000.0f;
	float mirror_fog_end = 100000.0f;
	std::int32_t mirror_fog_type = 1;
	// The manager's two per-frame particle tints (retail byte 128 = 1.0):
	// +0x3E8 = Env_TerrainLightCombined for AMBIENTCOLOR emitters, +0x3F0 =
	// the modulator block doubled+saturated for the rest
	// (retail render_emitter_effect @ 0x5f70c0; CParticleEmitter_AdvanceFrame
	//  @ 0x5e6600..0x5e661c selects into emitter+200). Neutral until an
	// environment source is attached.
	std::array<float, 3> ambient_tint{1.0f, 1.0f, 1.0f};
	std::array<float, 3> modulator_tint{1.0f, 1.0f, 1.0f};

	Impl() {
		create_effects();
	}

	// Every compositor effect this renderer owns, whichever view it serves, so
	// no lifecycle path (hide, warm, shutdown, re-entry) can skip a view.
	template <typename Visitor>
	void for_each_effect(Visitor &&visit) {
		for (ParticleEffectPair *pair :
				{&world_effects, &reflection_effects, &second_scene_effects}) {
			for (Ref<ParticleCompositorEffect> &effect : *pair)
				visit(effect);
		}
	}

	// A fresh compositor set: the constructor's, and the replacement a
	// re-entering renderer needs. release_device_resources() retires an
	// effect for good (its render callback never runs again), so a renderer
	// that left the tree can only render again through new effects.
	void create_effects() {
		for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
			effect.instantiate();
		});
		// The far side of the water plane precedes the transparent list in the
		// two views that draw the water surface: the main view and the second
		// scene view. The mirror admits no water surface, so its pair stays two
		// consecutive POST_TRANSPARENT passes.
		world_effects[0]->set_effect_callback_type(
				CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);
		second_scene_effects[0]->set_effect_callback_type(
				CompositorEffect::EFFECT_CALLBACK_TYPE_PRE_TRANSPARENT);
	}

	// ParticleRenderer::shutdown() detaches while camera and server ownership
	// are known-live. Late destruction must only discard retained references.
	~Impl() = default;

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
		mirror_fog_color = fog_color;
		mirror_fog_start = fog_start;
		mirror_fog_end = fog_end;
		mirror_fog_type = fog_type;
		ambient_tint = {1.0f, 1.0f, 1.0f};
		modulator_tint = {1.0f, 1.0f, 1.0f};
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
			const opennova::env::SceneFogValues mirror =
					env->state().build_water_mirror_fog();
			if (std::isfinite(mirror.color.r) && std::isfinite(mirror.color.g) &&
					std::isfinite(mirror.color.b)) {
				mirror_fog_color = {mirror.color.r, mirror.color.g, mirror.color.b};
			}
			mirror_fog_start = finite_or(mirror.start, mirror_fog_start);
			mirror_fog_end = finite_or(mirror.end, mirror_fog_end);
			mirror_fog_type = std::clamp<std::int32_t>(mirror.type, 0, 3);
			auto finite_tint = [](const Vector3 &value, std::array<float, 3> fallback) {
				if (std::isfinite(value.x) && std::isfinite(value.y) &&
						std::isfinite(value.z)) {
					return std::array<float, 3>{value.x, value.y, value.z};
				}
				return fallback;
			};
			ambient_tint = finite_tint(env->get_particle_ambient_tint(), ambient_tint);
			modulator_tint = finite_tint(env->get_particle_modulator_tint(), modulator_tint);
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
		detach_compositor_group(attached_second_scene_camera, second_scene_effects,
				inherited_second_scene_compositor);
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
		for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
			effect->clear_submission();
		});
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
		opennova::renderer::ParticleAtlasBuilder builder;
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

			opennova::renderer::ParticleRgbaImage rgba = particle_rgba_image(image,
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
			visual.ambient_lit = (definition.flags &
					opennova::particle::particle_flag::AmbientColor) != 0;
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
				if (graphic.blend_mode == opennova::particle::BlendMode::Blend)
					visual.ambient_lit = true;
				layer.flip_frames = std::clamp(graphic.flip_frames, 1,
						opennova::particle::kMaxParticleFlipFrames);
				layer.flip_rate = std::max(0, graphic.flip_rate);
				layer.frame_entries.reserve(static_cast<std::size_t>(layer.flip_frames));
				for (int frame = 1; frame <= layer.flip_frames; ++frame) {
					const std::string name = opennova::renderer::retail_particle_frame_name(
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

		opennova::renderer::ParticleAtlasBuild build = builder.build();
		entries = std::move(build.entries);
		rejected_atlas_entries = build.rejected_entries;
		pages.reserve(build.pages.size());
		auto snapshot = std::make_shared<ParticleAtlasSnapshot>();
		snapshot->generation = ++atlas_generation;
		snapshot->pages.reserve(build.pages.size());
		for (opennova::renderer::ParticleAtlasPage &source : build.pages) {
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

	Ref<Shader> shader_for(opennova::renderer::ParticlePipeline pipeline) {
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

	Ref<ShaderMaterial> material_for(const opennova::renderer::ParticleDrawCommand &command) {
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
		std::array<float, 9> rows;
		for (int row = 0; row < 3; ++row)
			for (int col = 0; col < 3; ++col)
				rows[row * 3 + col] = static_cast<float>(view_basis[row][col]);
		return opennova::renderer::particle_lit_primary_color(lit.bump_scale, lit.roll, lit.alpha, rows);
	}

	// Re-derives the view-dependent Bump/Bumpadd DIFFUSE channel for a second
	// camera (the one other basis-dependent input, a camera-facing TOPALIGN
	// quad's roll, keeps the main view's value). Each colour is recomputed from
	// the retained LitQuadInput row and never from the value it replaces, so a
	// relight leaves the snapshot lit for exactly this basis no matter which
	// view compiled (and relit) before it.
	void relight_render_snapshot(const Basis &view_basis) {
		for (const LitQuadInput &lit : lit_quads) {
			render_snapshot.particles[lit.particle_index].primary_color =
					lit_primary_color(lit, view_basis);
		}
	}

	// A view without its camera this render: nothing published, nothing
	// reported, and (its group already detached) nothing drawn.
	void retire_view_pair(ParticleEffectPair &effects, ParticleDrawSlot far_slot,
			ParticleDrawSlot camera_slot) {
		for (Ref<ParticleCompositorEffect> &effect : effects)
			effect->clear_submission();
		slot_present[far_slot] = false;
		slot_present[camera_slot] = false;
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

			opennova::renderer::ParticleEmitterSnapshot emitter;
			emitter.emitter_id = source_emitter.id;
			emitter.position = particle_vec(source_emitter.position);
			emitter.first_particle = render_snapshot.particles.size();
			if (source_emitter.group_index < frame.groups.size() &&
					frame.groups[source_emitter.group_index].render_domain ==
						opennova::particle::EffectRenderDomain::FirstPerson) {
				emitter.domain = opennova::renderer::ParticleRenderDomain::FirstPerson;
			} else {
				emitter.domain = opennova::renderer::ParticleRenderDomain::World;
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

				// The manager tint `(byte * channel) >> 7`: AMBIENTCOLOR defs — and
				// every def with a blend-mode-0 graphic, which Initialize forces
				// into AMBIENTCOLOR (retail CEffectEmitter_Initialize @ 0x5e6038..
				// 0x5e605e) — take the terrain light; the rest the doubled modulator
				// (retail CParticleEmitter_AdvanceFrame @ 0x5e6600..0x5e661c;
				//  BuildBillboardQuads @ 0x5e6d60 / RenderStaticBillboards @ 0x5f4e10).
				const std::array<float, 3> &tint = visual.ambient_lit ? ambient_tint : modulator_tint;
				constexpr float byte_to_unit = 1.0f / 255.0f;
				float red = static_cast<float>(particle.color.r) * byte_to_unit;
				float green = static_cast<float>(particle.color.g) * byte_to_unit;
				float blue = static_cast<float>(particle.color.b) * byte_to_unit;
				red = std::clamp(red * red_multiplier * tint[0], 0.0f, 1.0f);
				green = std::clamp(green * green_multiplier * tint[1], 0.0f, 1.0f);
				blue = std::clamp(blue * blue_multiplier * tint[2], 0.0f, 1.0f);
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

				opennova::renderer::ParticleQuadSnapshot quad;
				quad.center = particle_vec(particle.position);
				quad.half_width = size * 0.5f;
				quad.half_height = size * 0.5f;
				quad.roll = particle.rotation;
				quad.yaw = particle.yaw;
				quad.pitch = particle.pitch;
				quad.camera_pull = source_emitter.camera_pull;
				quad.primary_color = pack_argb(red, green, blue, alpha);
				quad.alignment = (definition.flags &
						opennova::particle::particle_flag::YawAndPitch) != 0 ?
						opennova::renderer::ParticleAlignment::WorldOriented :
						opennova::renderer::ParticleAlignment::CameraFacing;
				if ((definition.flags & opennova::particle::particle_flag::TopAlign) != 0) {
					// TOPALIGN defs run through the rot-head system, whose renderer
					// overwrites the roll every frame so the quad's top follows the
					// particle's heading: the velocity is taken into the view frame,
					// normalized, and the roll is `±acos(dot(dir, up))`, negative
					// when the direction points right (retail: the CParticleRotHeadSystem
					// vtable+40 renderer @ 0x5f5640 — head = pos + vel @ 0x5f587f,
					// normalize @ 0x5f597b, acos and sign @ 0x5f59a0..0x5f59c7). A
					// world-oriented quad measures against its own yaw/pitch frame.
					Vector3 heading(particle.velocity.x, particle.velocity.y, particle.velocity.z);
					if (quad.alignment == opennova::renderer::ParticleAlignment::WorldOriented) {
						const Basis frame = Basis::from_euler(
								Vector3(particle.pitch, particle.yaw, 0.0f), EULER_ORDER_YXZ);
						heading = frame.xform_inv(heading);
					} else {
						heading = view_basis.xform(heading);
					}
					const float planar = heading.x * heading.x + heading.y * heading.y;
					if (planar > 1.0e-12f) {
						quad.roll = std::atan2(-heading.x, heading.y);
					}
				}
				quad.state.pipeline = static_cast<opennova::renderer::ParticlePipeline>(layer.type);
				quad.state.pass = layer.type == 7 ?
						opennova::renderer::ParticleRenderPass::Distortion :
						opennova::renderer::ParticleRenderPass::Color;

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
					const opennova::renderer::ParticleAtlasEntry &entry = entries[entry_index];
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
			const opennova::renderer::ParticleDrawList &draw_list,
			const Vector3 &camera_position, const Vector3 &camera_forward, int64_t time_ms) {
		auto submission = std::make_shared<ParticleWorldSubmission>();
		submission->frame_id = draw_list.frame_id;
		submission->time_ms = static_cast<uint32_t>(time_ms);
		submission->commands = draw_list.commands;
		submission->atlas = atlas_snapshot;
		for (std::size_t component = 0; component < 3; ++component) {
			submission->camera_position[component] =
					camera_position[static_cast<int>(component)];
			submission->camera_forward[component] =
					camera_forward[static_cast<int>(component)];
		}
		const bool water_mirror = effect == reflection_effects[0] ||
				effect == reflection_effects[1];
		submission->fog_color = water_mirror ? mirror_fog_color : fog_color;
		submission->fog_start = water_mirror ? mirror_fog_start : fog_start;
		submission->fog_end = water_mirror ? mirror_fog_end : fog_end;
		submission->fog_type = water_mirror ? mirror_fog_type : fog_type;
		if (draw_list.domain != opennova::renderer::ParticleRenderDomain::World) {
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
					6u * sizeof(opennova::renderer::ParticleVertex);
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
				constexpr std::size_t stride = sizeof(opennova::renderer::ParticleVertex);
				for (std::size_t quad = 0; quad < quad_count; ++quad) {
					const opennova::renderer::ParticleVertex *source =
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
			const opennova::renderer::ParticleDrawList &draw_list, bool hidden) {
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
		for (const opennova::renderer::ParticleDrawCommand &command : draw_list.commands) {
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
				const opennova::renderer::ParticleVertex &source =
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

void ParticleRenderer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("warm_pipelines", "position"),
			&ParticleRenderer::warm_pipelines);
	ClassDB::bind_method(D_METHOD("set_scene", "scene"),
			&ParticleRenderer::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"),
			&ParticleRenderer::get_scene);
	ClassDB::bind_method(D_METHOD("set_texture_provider", "provider"),
			&ParticleRenderer::set_texture_provider);
	ClassDB::bind_method(D_METHOD("get_texture_provider"),
			&ParticleRenderer::get_texture_provider);
	ClassDB::bind_method(D_METHOD("set_environment_source", "source"),
			&ParticleRenderer::set_environment_source);
	ClassDB::bind_method(D_METHOD("get_environment_source"),
			&ParticleRenderer::get_environment_source);
	ClassDB::bind_method(D_METHOD("set_water_plane", "height", "reflection_camera"),
			&ParticleRenderer::set_water_plane);
	ClassDB::bind_method(D_METHOD("set_second_scene_camera", "camera"),
			&ParticleRenderer::set_second_scene_camera);
	ClassDB::bind_method(D_METHOD("get_second_scene_camera"),
			&ParticleRenderer::get_second_scene_camera);
	ClassDB::bind_method(D_METHOD("set_hidden", "hidden"),
			&ParticleRenderer::set_hidden);
	ClassDB::bind_method(D_METHOD("get_hidden"),
			&ParticleRenderer::get_hidden);
	ClassDB::bind_method(D_METHOD("set_procedural_fallback_enabled", "enabled"),
			&ParticleRenderer::set_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("get_procedural_fallback_enabled"),
			&ParticleRenderer::get_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("render_now", "time_ms"),
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
	if (p_what == NOTIFICATION_ENTER_TREE) {
		_restore_device_state();
	} else if (p_what == NOTIFICATION_READY) {
		impl_->ensure_visuals(this);
		set_process(false);
		render_now(GameWorld::current_frame_clock_ms());
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		shutdown();
	}
}

// The re-entry half of the EXIT_TREE/shutdown contract (the same shape
// DisplayDecode follows: release on exit, recreate on entry). A renderer that
// left the tree, or was released explicitly, holds retired compositor effects
// and a latched shutdown_; entering the tree again replaces the effects and
// clears the latch so the next render_now attaches and publishes as on the
// first entry. The retained CPU state (catalog, atlas snapshot, first-person
// batch child) is untouched: the new effects re-upload from it on publish.
void ParticleRenderer::_restore_device_state() {
	if (!shutdown_)
		return;
	shutdown_ = false;
	if (!impl_)
		return;
	impl_->create_effects();
	const bool hidden = hidden_;
	impl_->for_each_effect([hidden](Ref<ParticleCompositorEffect> &effect) {
		effect->set_particles_hidden(hidden);
	});
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

void ParticleRenderer::set_second_scene_camera(Camera3D *p_camera) {
	second_scene_camera_ = p_camera != nullptr ?
			ObjectID(p_camera->get_instance_id()) : ObjectID();
}

Camera3D *ParticleRenderer::get_second_scene_camera() const {
	if (!second_scene_camera_.is_valid())
		return nullptr;
	return Object::cast_to<Camera3D>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(second_scene_camera_)));
}

void ParticleRenderer::warm_pipelines(const Vector3 &p_position) {
	clear_warm_pipelines();
	if (!impl_)
		return;
	impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
		effect->request_pipeline_warm();
	});
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(0.01f, 0.01f));
	for (std::size_t i = 0; i < impl_->shader_cache.size(); ++i) {
		Ref<Shader> shader =
				impl_->shader_for(static_cast<opennova::renderer::ParticlePipeline>(i));
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
		impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
			effect->cancel_pipeline_warm();
		});
	}
	for (Node *node : warm_nodes_) {
		if (node != nullptr)
			node->queue_free();
	}
	warm_nodes_.clear();
}

void ParticleRenderer::shutdown() {
	if (shutdown_)
		return;
	shutdown_ = true;
	clear_warm_pipelines();
	if (!impl_)
		return;

	impl_->clear_draws();
	impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
		effect->set_enabled(false);
	});
	impl_->detach_compositors();

	// Detaching affects the next render setup. Drain a callback already queued
	// on the render thread before releasing the RIDs it can still consume.
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr && server->get_rendering_device() != nullptr)
		server->force_sync();
	impl_->for_each_effect([](Ref<ParticleCompositorEffect> &effect) {
		effect->release_device_resources();
	});
}

void ParticleRenderer::set_hidden(bool p_hidden) {
	if (hidden_ == p_hidden)
		return;
	hidden_ = p_hidden;
	if (!impl_)
		return;
	impl_->for_each_effect([p_hidden](Ref<ParticleCompositorEffect> &effect) {
		effect->set_particles_hidden(p_hidden);
	});
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

void ParticleRenderer::render_now(int64_t p_time_ms) {
	if (shutdown_ || !impl_)
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
	// A camera that already owns the World or the mirror pair keeps that one,
	// and a camera outside the tree renders nothing (and has no transform).
	Camera3D *second_scene_camera = get_second_scene_camera();
	if (second_scene_camera != nullptr && (second_scene_camera == camera ||
			second_scene_camera == reflection_camera ||
			!second_scene_camera->is_inside_tree()))
		second_scene_camera = nullptr;
	impl_->attach_compositor_group(camera, viewport,
			impl_->attached_world_camera, impl_->world_effects,
			impl_->inherited_world_compositor);
	impl_->attach_compositor_group(reflection_camera,
			reflection_camera != nullptr ? reflection_camera->get_viewport() : nullptr,
			impl_->attached_reflection_camera, impl_->reflection_effects,
			impl_->inherited_reflection_compositor);
	impl_->attach_compositor_group(second_scene_camera,
			second_scene_camera != nullptr ?
					second_scene_base_viewport(second_scene_camera, viewport) : nullptr,
			impl_->attached_second_scene_camera, impl_->second_scene_effects,
			impl_->inherited_second_scene_compositor);
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
			opennova::renderer::ParticleWaterSubset subset,
			const ParticleCameraFrame &view_camera,
			const Ref<ParticleCompositorEffect> &effect) {
		opennova::renderer::ParticleViewInput view;
		view.domain = opennova::renderer::ParticleRenderDomain::World;
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
		const opennova::renderer::ParticleDrawList &draw_list =
				impl_->compilers[slot].compile(impl_->render_snapshot, view);
		impl_->slot_present[slot] = true;
		impl_->publish_world_draw_list(effect, draw_list, view_camera.position,
				view_camera.forward, p_time_ms);
	};

	compile_world(kWorldFarSide,
			opennova::renderer::particle_water_subset_for_side(camera_above_water, false),
			world_camera, impl_->world_effects[0]);
	compile_world(kWorldCameraSide,
			opennova::renderer::particle_water_subset_for_side(camera_above_water, true),
			world_camera, impl_->world_effects[1]);

	opennova::renderer::ParticleViewInput first_person_view;
	first_person_view.domain = opennova::renderer::ParticleRenderDomain::FirstPerson;
	first_person_view.water_subset = opennova::renderer::ParticleWaterSubset::All;
	first_person_view.position = particle_vec(world_camera.position);
	first_person_view.right = particle_vec(world_camera.right);
	first_person_view.up = particle_vec(world_camera.up);
	first_person_view.forward = particle_vec(world_camera.forward);
	std::copy(world_camera.projection.begin(), world_camera.projection.end(),
			first_person_view.projection);
	first_person_view.projection_valid = world_camera.projection_valid;
	first_person_view.projection_near_is_one = true;
	const opennova::renderer::ParticleDrawList &first_person_draw =
			impl_->compilers[kFirstPerson].compile(impl_->render_snapshot,
					first_person_view);
	impl_->slot_present[kFirstPerson] = true;
	impl_->upload_first_person_draw_list(first_person_draw, hidden_);

	// A secondary view compiles the same snapshot for its own eye. LitColor/Bump
	// channels transform through the active view basis, so only those quads are
	// relit for this view before its two consecutive compiles. The World and
	// FirstPerson draw lists above already hold their own vertex copies, and a
	// relight never reads the colour it replaces, so no view can change what
	// another draws, whichever of them compiles first.
	auto compile_secondary_pair = [&](const ParticleCameraFrame &view_camera,
			bool above_water, ParticleDrawSlot far_slot,
			ParticleDrawSlot camera_slot, const ParticleEffectPair &effects) {
		impl_->relight_render_snapshot(view_camera.view_basis);
		compile_world(far_slot,
				opennova::renderer::particle_water_subset_for_side(above_water, false),
				view_camera, effects[0]);
		compile_world(camera_slot,
				opennova::renderer::particle_water_subset_for_side(above_water, true),
				view_camera, effects[1]);
	};

	if (reflection_camera != nullptr) {
		// The reflected pass selects its water subsets by the main camera's
		// side (Water_RenderReflectedWorldScene passes the prerender's
		// below-water flag), whether its eye is mirrored or not.
		compile_secondary_pair(particle_camera_frame(reflection_camera),
				camera_above_water, kReflectionFarSide, kReflectionCameraSide,
				impl_->reflection_effects);
	} else {
		impl_->retire_view_pair(impl_->reflection_effects, kReflectionFarSide,
				kReflectionCameraSide);
	}

	if (second_scene_camera != nullptr) {
		// The original runs its one scene routine again for this camera, so the
		// far/camera-side bracket belongs to this view's own eye. The Inset eye
		// is the main eye, so the two agree today; keying on the view's own
		// frame keeps the bracket right for a second view placed anywhere else.
		const ParticleCameraFrame second_camera =
				particle_camera_frame(second_scene_camera);
		compile_secondary_pair(second_camera,
				second_camera.position.y >= water_height_, kSecondSceneFarSide,
				kSecondSceneCameraSide, impl_->second_scene_effects);
	} else {
		impl_->retire_view_pair(impl_->second_scene_effects, kSecondSceneFarSide,
				kSecondSceneCameraSide);
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
	result["second_scene_far_side"] = slot_report(kSecondSceneFarSide);
	result["second_scene_camera_side"] = slot_report(kSecondSceneCameraSide);
	result["first_person"] = slot_report(kFirstPerson);
	result["world_far_backend"] =
			impl_->world_effects[0]->get_backend_report();
	result["world_camera_backend"] =
			impl_->world_effects[1]->get_backend_report();
	result["reflection_far_backend"] =
			impl_->reflection_effects[0]->get_backend_report();
	result["reflection_camera_backend"] =
			impl_->reflection_effects[1]->get_backend_report();
	result["second_scene_far_backend"] =
			impl_->second_scene_effects[0]->get_backend_report();
	result["second_scene_camera_backend"] =
			impl_->second_scene_effects[1]->get_backend_report();
	result["first_person_backend"] = "array_mesh_fallback_tool_only";
	result["world_compositor_attached"] =
			impl_->attached_world_camera.is_valid();
	result["reflection_compositor_attached"] =
			impl_->attached_reflection_camera.is_valid();
	result["second_scene_compositor_attached"] =
			impl_->attached_second_scene_camera.is_valid();
	result["world_compositor_inherited_effects"] =
			impl_->inherited_world_compositor;
	result["reflection_compositor_inherited_effects"] =
			impl_->inherited_reflection_compositor;
	result["second_scene_compositor_inherited_effects"] =
			impl_->inherited_second_scene_compositor;
	result["world_mesh_instance"] = false;
	result["shutdown"] = shutdown_;
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
	for (const opennova::renderer::ParticleAtlasEntry &entry : impl_->entries) {
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
		const opennova::renderer::ParticleAtlasEntry &entry = impl_->entries[i];
		Dictionary value;
		value["name"] = opennova::to_gd(entry.name);
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
		for (const opennova::renderer::ParticleEmitterDrawBounds &bounds :
				impl_->compilers[slot].draw_list().emitter_bounds) {
			Dictionary value;
			value["emitter_id"] = token_to_godot(bounds.emitter_id);
			value["render_domain"] = slot == kFirstPerson ?
					static_cast<int>(opennova::renderer::ParticleRenderDomain::FirstPerson) :
					static_cast<int>(opennova::renderer::ParticleRenderDomain::World);
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

void ParticleRenderer::collect_debug_emitter_bounds(
		std::vector<opennova::renderer::ParticleEmitterDrawBounds> &r_out) const {
	r_out.clear();
	if (!impl_)
		return;
	for (const ParticleDrawSlot slot : {
			kWorldFarSide, kWorldCameraSide, kFirstPerson}) {
		if (!impl_->slot_present[slot])
			continue;
		const auto &bounds = impl_->compilers[slot].draw_list().emitter_bounds;
		r_out.insert(r_out.end(), bounds.begin(), bounds.end());
	}
}

PackedStringArray ParticleRenderer::get_unresolved_texture_names() const {
	PackedStringArray result;
	if (!impl_)
		return result;
	result.resize(static_cast<int64_t>(impl_->unresolved_names.size()));
	for (std::size_t i = 0; i < impl_->unresolved_names.size(); ++i)
		result[static_cast<int64_t>(i)] =
				opennova::to_gd(impl_->unresolved_names[i]);
	return result;
}
