#include "nova_particle_renderer.h"

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
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <particle/emitter.h>
#include <renderer/particle_atlas.h>
#include <renderer/particle_color.h>
#include <renderer/particle_frame.h>

#include "nova_particle_compositor.h"
#include "util/texture_path_resolver.h"

using namespace godot;

namespace {

constexpr int kGraphicLayerCount = 4;
constexpr std::uint32_t kFirstPersonVisibilityMask = 1u << 11;
constexpr std::size_t kMissingEntry = std::numeric_limits<std::size_t>::max();

using opennova::particle::BlendMode;
using opennova::particle::CurveRef;
using opennova::particle::GraphicLayer;
using opennova::particle::Particle;
using opennova::particle::ParticleDef;

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

struct PacketDiagnostics {
	bool present = false;
	std::uint64_t frame_id = 0;
	renderer::ParticleFrameDebugCounters debug{};
	std::vector<renderer::ParticleDrawCommand> commands;
	std::vector<renderer::ParticleEmitterDrawBounds> emitter_bounds;
};

void capture_packet_diagnostics(PacketDiagnostics &destination,
		const renderer::ParticleDrawPacket &packet) {
	destination.present = true;
	destination.frame_id = packet.frame_id;
	destination.debug = packet.debug;
	destination.commands = packet.commands;
	destination.emitter_bounds = packet.emitter_bounds;
}

Dictionary packet_report(const PacketDiagnostics &packet) {
	Dictionary result;
	if (!packet.present)
		return result;
	const renderer::ParticleFrameDebugCounters &debug = packet.debug;
	result["frame_id"] = godot_token(packet.frame_id);
	result["compile_index"] = godot_token(debug.compile_index);
	result["input_emitters"] = static_cast<int64_t>(debug.input_emitters);
	result["selected_emitters"] = static_cast<int64_t>(debug.selected_emitters);
	result["input_particles"] = static_cast<int64_t>(debug.input_particles);
	result["domain_filtered_particles"] =
			static_cast<int64_t>(debug.domain_filtered_particles);
	result["invisible_particles"] = static_cast<int64_t>(debug.invisible_particles);
	result["truncated_particles"] = static_cast<int64_t>(debug.truncated_particles);
	result["rendered_quad_count"] = static_cast<int64_t>(debug.emitted_quads);
	result["draw_command_count"] = static_cast<int64_t>(debug.draw_commands);
	result["adjacent_state_merges"] =
			static_cast<int64_t>(debug.adjacent_state_merges);
	result["capacity_growths_this_compile"] =
			static_cast<int64_t>(debug.capacity_growths_this_compile);
	result["lifetime_capacity_growths"] =
			godot_token(debug.lifetime_capacity_growths);
	result["vertex_capacity"] = static_cast<int64_t>(debug.vertex_capacity);
	result["command_capacity"] = static_cast<int64_t>(debug.command_capacity);
	result["emitter_bounds_capacity"] =
			static_cast<int64_t>(debug.emitter_bounds_capacity);
	Array commands;
	commands.resize(static_cast<int64_t>(packet.commands.size()));
	for (std::size_t i = 0; i < packet.commands.size(); ++i) {
		const renderer::ParticleDrawCommand &command = packet.commands[i];
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
			Ref<NovaParticleCompositorEffect>>> effects;
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
				Object::cast_to<NovaParticleCompositorEffect>(
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
				Object::cast_to<NovaParticleCompositorEffect>(
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
	if (state.effective_base.is_valid())
		effects = state.effective_base->get_compositor_effects();
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
	state.installed.instantiate();
	state.installed->set_compositor_effects(effects);
	camera->set_compositor(state.installed);
}

} // namespace

class NovaParticleRenderer::Impl {
public:
	MeshInstance3D *first_person_instance = nullptr;
	Ref<ArrayMesh> first_person_mesh;
	std::array<renderer::ParticleFrameCompiler, 2> compilers;
	std::array<PacketDiagnostics, 2> packets;
	renderer::ParticleFrameSnapshot render_snapshot;
	std::shared_ptr<const std::vector<ParticleDef>> catalog_definitions;
	std::vector<DefinitionVisual> definition_visuals;
	std::vector<renderer::ParticleAtlasEntry> entries;
	std::vector<AtlasPage> pages;
	std::shared_ptr<const NovaParticleAtlasSnapshot> atlas_snapshot;
	std::uint64_t atlas_generation = 0;
	std::array<Ref<Shader>, 8> shader_cache;
	std::map<std::uint64_t, Ref<ShaderMaterial>> materials;
	std::vector<std::string> unresolved_names;
	std::size_t rejected_atlas_entries = 0;
	Ref<NovaParticleCompositorEffect> world_effect;
	ObjectID attached_camera;
	bool inherited_world_compositor = false;
	bool catalog_dirty = true;
	ObjectID cached_environment_source;
	std::int64_t cached_environment_generation =
			std::numeric_limits<std::int64_t>::min();
	std::array<float, 3> fog_color{0.5f, 0.6f, 0.8f};
	float fog_start = 30000.0f;
	float fog_end = 100000.0f;
	std::int32_t fog_type = 1;

	Impl() {
		world_effect.instantiate();
	}

	~Impl() {
		detach_compositor();
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
		const ObjectID source_id = source != nullptr ?
				ObjectID(source->get_instance_id()) : ObjectID();
		std::int64_t generation =
				std::numeric_limits<std::int64_t>::min();
		bool has_generation = false;
		if (source != nullptr &&
				source->has_method(StringName("get_env_generation"))) {
			const Variant value = source->call("get_env_generation");
			if (value.get_type() == Variant::INT) {
				generation = static_cast<std::int64_t>(value);
				has_generation = true;
			}
		}
		if (has_generation && source_id == cached_environment_source &&
				generation == cached_environment_generation) {
			return;
		}

		// Project defaults keep standalone previews useful. Runtime GameWorld
		// supplies NovaEnvironment explicitly, avoiding RenderingServer global
		// readback (which Godot rejects outside the editor).
		fog_color = {0.5f, 0.6f, 0.8f};
		fog_start = 30000.0f;
		fog_end = 100000.0f;
		fog_type = 1;
		if (source != nullptr) {
			if (source->has_method(StringName("get_fog_color"))) {
				const Variant value = source->call("get_fog_color");
				if (value.get_type() == Variant::VECTOR3) {
					const Vector3 color = static_cast<Vector3>(value);
					if (std::isfinite(color.x) && std::isfinite(color.y) &&
							std::isfinite(color.z)) {
						fog_color = {color.x, color.y, color.z};
					}
				}
			}
			auto read_finite_float = [source](const char *method,
					float fallback) {
				if (!source->has_method(StringName(method)))
					return fallback;
				const Variant value = source->call(method);
				if (value.get_type() != Variant::FLOAT &&
						value.get_type() != Variant::INT) {
					return fallback;
				}
				const float converted = static_cast<float>(
						static_cast<double>(value));
				return std::isfinite(converted) ? converted : fallback;
			};
			fog_start = read_finite_float("get_fog_start", fog_start);
			fog_end = read_finite_float("get_fog_level", fog_end);
			if (source->has_method(StringName("get_fog_type"))) {
				const Variant value = source->call("get_fog_type");
				if (value.get_type() == Variant::INT) {
					fog_type = std::clamp<std::int32_t>(
							static_cast<std::int32_t>(
									static_cast<std::int64_t>(value)),
							0, 3);
				}
			}
		}
		cached_environment_source = source_id;
		cached_environment_generation = has_generation ? generation :
				std::numeric_limits<std::int64_t>::min();
	}

	void detach_compositor() {
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
			const std::uint64_t effect_id =
					world_effect->get_instance_id();
			state.effects.erase(std::remove_if(
					state.effects.begin(), state.effects.end(),
					[effect_id](const auto &entry) {
						return entry.first == effect_id;
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
		inherited_world_compositor = false;
	}

	void attach_compositor(Camera3D *camera, Viewport *viewport) {
		if (camera == nullptr) {
			detach_compositor();
			return;
		}
		const std::uint64_t camera_id = camera->get_instance_id();
		const ObjectID camera_object_id(camera_id);
		if (attached_camera.is_valid() &&
				attached_camera != camera_object_id) {
			detach_compositor();
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

		const std::uint64_t effect_id = world_effect->get_instance_id();
		auto effect_it = std::find_if(state.effects.begin(),
				state.effects.end(), [effect_id](const auto &entry) {
					return entry.first == effect_id;
				});
		if (effect_it == state.effects.end()) {
			state.effects.emplace_back(effect_id, world_effect);
			rebuild = true;
		} else if (effect_it->second != world_effect) {
			effect_it->second = world_effect;
			rebuild = true;
		}
		attached_camera = camera_object_id;
		inherited_world_compositor = state.inherited_world_compositor;
		if (rebuild)
			rebuild_camera_compositor(state, camera);
	}

	void ensure_visuals(NovaParticleRenderer *owner) {
		if (first_person_instance != nullptr)
			return;
		first_person_instance = memnew(MeshInstance3D);
		first_person_instance->set_name("ParticleFirstPersonPacket");
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
		world_effect->clear_submission();
		first_person_mesh.unref();
		packets = {};
		if (first_person_instance != nullptr) {
			first_person_instance->set_mesh(Ref<Mesh>());
			first_person_instance->set_visible(false);
		}
	}

	void rebuild_catalog(
			const std::shared_ptr<const std::vector<ParticleDef>> &definitions,
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
			const ParticleDef &definition = (*definitions)[definition_index];
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
		auto snapshot = std::make_shared<NovaParticleAtlasSnapshot>();
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

			NovaParticleAtlasPageSnapshot upload;
			upload.type = source.type;
			upload.side = static_cast<std::uint32_t>(source.image.width);
			upload.rgba8 = pixels;
			snapshot->pages.push_back(std::move(upload));
		}
		atlas_snapshot = std::shared_ptr<const NovaParticleAtlasSnapshot>(
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

	void build_render_snapshot(
			const opennova::particle::ParticleFrameSnapshot &frame,
			const Basis &view_basis) {
		render_snapshot.frame_id = frame.frame_index;
		render_snapshot.emitters.clear();
		if (!frame.definitions ||
				definition_visuals.size() != frame.definitions->size())
			return;
		render_snapshot.emitters.reserve(frame.emitters.size());

		for (const opennova::particle::EffectEmitterFrameSnapshot &source_emitter :
				frame.emitters) {
			if (source_emitter.definition_index >= frame.definitions->size() ||
					source_emitter.definition_index >= definition_visuals.size())
				continue;
			const ParticleDef &definition =
					(*frame.definitions)[source_emitter.definition_index];
			const DefinitionVisual &visual =
					definition_visuals[source_emitter.definition_index];

			renderer::ParticleEmitterSnapshot emitter;
			emitter.emitter_id = source_emitter.id;
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
			emitter.particles.reserve(count);
			const std::uint32_t lod_divisor =
					std::max<std::uint32_t>(1u, source_emitter.lod_divisor);

			for (std::size_t particle_index = 0; particle_index < count;
					++particle_index) {
				const Particle &particle = frame.particles[first + particle_index];
				// EffectScene::inspect uses the simulator's base half-size for
				// manager/emitter bounds. Keep that authoritative sort box even
				// when render LOD omits this particle; packet output bounds below
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
					constexpr float light_component = 0.5773503f;
					const Vector3 seed = definition.bump_scale *
							Vector3(light_component, light_component,
									light_component);
					const Basis rotate_x(Vector3(1.0f, 0.0f, 0.0f),
							particle.rotation);
					// Literal retail operation: transpose(Rx(roll) * view).
					const Vector3 light_local =
							(rotate_x * view_basis).transposed().xform(seed);
					// Exact FVF ordering for Bump/Bumpadd: DIFFUSE (Godot COLOR)
					// carries encoded light + particle alpha; SPECULAR (CUSTOM0)
					// carries original modulated RGB with opaque alpha.
					quad.primary_color = pack_argb_bytes(
							retail_low_byte(light_local.x),
							retail_low_byte(light_local.y),
							retail_low_byte(light_local.z), unit_byte(alpha));
					quad.secondary_color = pack_argb(red, green, blue, 1.0f);
				} else {
					quad.secondary_color = 0xffffffffu;
				}

				emitter.particles.push_back(quad);
			}
			render_snapshot.emitters.push_back(std::move(emitter));
		}
	}

	void publish_world_packet(const renderer::ParticleDrawPacket &packet,
			const Vector3 &camera_position, const Vector3 &camera_forward) {
		auto submission = std::make_shared<NovaParticleWorldSubmission>();
		submission->frame_id = packet.frame_id;
		submission->commands = packet.commands;
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
		if (packet.domain != renderer::ParticleRenderDomain::World) {
			submission->valid = false;
			submission->validation_error =
					"World backend received a non-World compiler packet";
		} else if (packet.vertices.size() % 4u != 0) {
			submission->valid = false;
			submission->validation_error =
					"Compiler packet does not contain complete particle quads";
		} else {
			const std::size_t quad_count = packet.vertices.size() / 4u;
			constexpr std::size_t expanded_bytes_per_quad =
					6u * sizeof(renderer::ParticleVertex);
			if (quad_count > static_cast<std::size_t>(
					std::numeric_limits<int64_t>::max()) /
					expanded_bytes_per_quad) {
				submission->valid = false;
				submission->validation_error =
						"Expanded World packet exceeds PackedByteArray limits";
			} else {
				submission->triangle_vertices.resize(static_cast<int64_t>(
						quad_count * expanded_bytes_per_quad));
				std::uint8_t *destination =
						submission->triangle_vertices.ptrw();
				constexpr std::array<std::size_t, 6> triangle_order{
						0, 1, 2, 1, 3, 2};
				for (std::size_t quad = 0; quad < quad_count; ++quad) {
					for (std::size_t triangle_vertex = 0;
							triangle_vertex < triangle_order.size();
							++triangle_vertex) {
						const renderer::ParticleVertex &source =
								packet.vertices[quad * 4u +
										triangle_order[triangle_vertex]];
						const std::size_t offset =
								(quad * 6u + triangle_vertex) *
								sizeof(renderer::ParticleVertex);
						std::memcpy(destination + offset, &source,
								sizeof(source));
					}
				}
			}
		}
		world_effect->publish(
				std::shared_ptr<const NovaParticleWorldSubmission>(
						std::move(submission)));
	}

	void upload_first_person_packet(
			const renderer::ParticleDrawPacket &packet, bool hidden) {
		if (first_person_instance == nullptr)
			return;
		if (packet.commands.empty() || packet.vertices.empty()) {
			first_person_mesh.unref();
			first_person_instance->set_mesh(Ref<Mesh>());
			first_person_instance->set_visible(false);
			return;
		}

		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		for (const renderer::ParticleDrawCommand &command : packet.commands) {
			const std::size_t first_vertex =
					static_cast<std::size_t>(command.first_quad) * 4u;
			const std::size_t vertex_count =
					static_cast<std::size_t>(command.quad_count) * 4u;
			if (first_vertex > packet.vertices.size() ||
					vertex_count > packet.vertices.size() - first_vertex)
				continue;

			PackedVector3Array vertices;
			PackedVector2Array uvs;
			PackedColorArray colors;
			PackedByteArray custom0;
			PackedInt32Array indices;
			vertices.resize(static_cast<int64_t>(vertex_count));
			uvs.resize(static_cast<int64_t>(vertex_count));
			colors.resize(static_cast<int64_t>(vertex_count));
			custom0.resize(static_cast<int64_t>(vertex_count * 4u));
			indices.resize(static_cast<int64_t>(command.quad_count) * 6);
			std::uint8_t *custom_bytes = custom0.ptrw();

			for (std::size_t vertex_index = 0; vertex_index < vertex_count;
					++vertex_index) {
				const renderer::ParticleVertex &source =
						packet.vertices[first_vertex + vertex_index];
				vertices[static_cast<int64_t>(vertex_index)] =
						Vector3(source.x, source.y, source.z);
				uvs[static_cast<int64_t>(vertex_index)] = Vector2(source.u, source.v);
				colors[static_cast<int64_t>(vertex_index)] =
						unpack_argb(source.primary_color);
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
				const int vertex = static_cast<int>(quad * 4u);
				const int index = static_cast<int>(quad * 6u);
				indices[index + 0] = vertex + 0;
				indices[index + 1] = vertex + 1;
				indices[index + 2] = vertex + 2;
				indices[index + 3] = vertex + 1;
				indices[index + 4] = vertex + 3;
				indices[index + 5] = vertex + 2;
			}

			Array arrays;
			arrays.resize(Mesh::ARRAY_MAX);
			arrays[Mesh::ARRAY_VERTEX] = vertices;
			arrays[Mesh::ARRAY_TEX_UV] = uvs;
			arrays[Mesh::ARRAY_COLOR] = colors;
			arrays[Mesh::ARRAY_CUSTOM0] = custom0;
			arrays[Mesh::ARRAY_INDEX] = indices;
			const std::uint64_t flags =
					static_cast<std::uint64_t>(Mesh::ARRAY_FORMAT_CUSTOM0);
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays,
					TypedArray<Array>(), Dictionary(),
					static_cast<Mesh::ArrayFormat>(flags));
			const int surface = mesh->get_surface_count() - 1;
			mesh->surface_set_material(surface, material_for(command));
		}

		first_person_mesh = mesh;
		first_person_instance->set_mesh(mesh);
		first_person_instance->set_visible(
				!hidden && mesh->get_surface_count() > 0);
	}
};

NovaParticleRenderer::NovaParticleRenderer() : impl_(std::make_unique<Impl>()) {}

NovaParticleRenderer::~NovaParticleRenderer() = default;

void NovaParticleRenderer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("warm_pipelines", "position"),
			&NovaParticleRenderer::warm_pipelines);
	ClassDB::bind_method(D_METHOD("clear_warm_pipelines"),
			&NovaParticleRenderer::clear_warm_pipelines);
	ClassDB::bind_method(D_METHOD("set_scene", "scene"),
			&NovaParticleRenderer::set_scene);
	ClassDB::bind_method(D_METHOD("get_scene"),
			&NovaParticleRenderer::get_scene);
	ClassDB::bind_method(D_METHOD("set_texture_provider", "provider"),
			&NovaParticleRenderer::set_texture_provider);
	ClassDB::bind_method(D_METHOD("get_texture_provider"),
			&NovaParticleRenderer::get_texture_provider);
	ClassDB::bind_method(D_METHOD("set_texture_dir", "texture_dir"),
			&NovaParticleRenderer::set_texture_dir);
	ClassDB::bind_method(D_METHOD("get_texture_dir"),
			&NovaParticleRenderer::get_texture_dir);
	ClassDB::bind_method(D_METHOD("set_environment_source", "source"),
			&NovaParticleRenderer::set_environment_source);
	ClassDB::bind_method(D_METHOD("get_environment_source"),
			&NovaParticleRenderer::get_environment_source);
	ClassDB::bind_method(D_METHOD("set_hidden", "hidden"),
			&NovaParticleRenderer::set_hidden);
	ClassDB::bind_method(D_METHOD("get_hidden"),
			&NovaParticleRenderer::get_hidden);
	ClassDB::bind_method(D_METHOD("set_procedural_fallback_enabled", "enabled"),
			&NovaParticleRenderer::set_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("get_procedural_fallback_enabled"),
			&NovaParticleRenderer::get_procedural_fallback_enabled);
	ClassDB::bind_method(D_METHOD("render_now"),
			&NovaParticleRenderer::render_now);
	ClassDB::bind_method(D_METHOD("get_rendered_quad_count"),
			&NovaParticleRenderer::get_rendered_quad_count);
	ClassDB::bind_method(D_METHOD("get_draw_command_count"),
			&NovaParticleRenderer::get_draw_command_count);
	ClassDB::bind_method(D_METHOD("get_debug_packet_report"),
			&NovaParticleRenderer::get_debug_packet_report);
	ClassDB::bind_method(D_METHOD("get_debug_emitter_bounds"),
			&NovaParticleRenderer::get_debug_emitter_bounds);
	ClassDB::bind_method(D_METHOD("get_unresolved_texture_names"),
			&NovaParticleRenderer::get_unresolved_texture_names);

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

void NovaParticleRenderer::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		impl_->ensure_visuals(this);
		set_process(true);
		render_now();
	} else if (p_what == NOTIFICATION_PROCESS) {
		render_now();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		if (impl_)
			impl_->detach_compositor();
	}
}

void NovaParticleRenderer::_invalidate_catalog() {
	if (impl_)
		impl_->invalidate_catalog();
}

void NovaParticleRenderer::set_scene(const Ref<NovaEffectScene> &p_scene) {
	if (scene_ == p_scene)
		return;
	scene_ = p_scene;
	_invalidate_catalog();
	if (scene_.is_null() && impl_)
		impl_->clear_draws();
}

Ref<NovaEffectScene> NovaParticleRenderer::get_scene() const {
	return scene_;
}

void NovaParticleRenderer::set_texture_provider(const Callable &p_provider) {
	texture_provider_ = p_provider;
	_invalidate_catalog();
}

Callable NovaParticleRenderer::get_texture_provider() const {
	return texture_provider_;
}

void NovaParticleRenderer::set_texture_dir(const String &p_texture_dir) {
	if (texture_dir_ == p_texture_dir)
		return;
	texture_dir_ = p_texture_dir;
	_invalidate_catalog();
}

String NovaParticleRenderer::get_texture_dir() const {
	return texture_dir_;
}

void NovaParticleRenderer::set_environment_source(Node *p_source) {
	const ObjectID next = p_source != nullptr ?
			ObjectID(p_source->get_instance_id()) : ObjectID();
	if (environment_source_ == next)
		return;
	environment_source_ = next;
	if (impl_)
		impl_->invalidate_environment();
}

Node *NovaParticleRenderer::get_environment_source() const {
	if (!environment_source_.is_valid())
		return nullptr;
	return Object::cast_to<Node>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(environment_source_)));
}

void NovaParticleRenderer::warm_pipelines(const Vector3 &p_position) {
	clear_warm_pipelines();
	if (!impl_)
		return;
	impl_->world_effect->request_pipeline_warm();
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

void NovaParticleRenderer::clear_warm_pipelines() {
	if (impl_)
		impl_->world_effect->cancel_pipeline_warm();
	for (Node *node : warm_nodes_) {
		if (node != nullptr)
			node->queue_free();
	}
	warm_nodes_.clear();
}

void NovaParticleRenderer::set_hidden(bool p_hidden) {
	if (hidden_ == p_hidden)
		return;
	hidden_ = p_hidden;
	if (!impl_)
		return;
	impl_->world_effect->set_particles_hidden(hidden_);
	if (hidden_) {
		// The master switch is also a CPU switch: drop retained submissions once
		// and let the next visible process rebuild from the latest scene frame.
		impl_->clear_draws();
		return;
	}
	if (impl_->first_person_instance != nullptr) {
		impl_->first_person_instance->set_visible(
				!hidden_ && impl_->first_person_mesh.is_valid() &&
				impl_->first_person_mesh->get_surface_count() > 0);
	}
}

bool NovaParticleRenderer::get_hidden() const {
	return hidden_;
}

void NovaParticleRenderer::set_procedural_fallback_enabled(bool p_enabled) {
	if (procedural_fallback_enabled_ == p_enabled)
		return;
	procedural_fallback_enabled_ = p_enabled;
	_invalidate_catalog();
}

bool NovaParticleRenderer::get_procedural_fallback_enabled() const {
	return procedural_fallback_enabled_;
}

void NovaParticleRenderer::render_now() {
	if (!impl_)
		return;
	impl_->ensure_visuals(this);
	if (hidden_)
		return;
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	impl_->attach_compositor(camera, viewport);
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

	Vector3 camera_position;
	Vector3 camera_right(1.0f, 0.0f, 0.0f);
	Vector3 camera_up(0.0f, 1.0f, 0.0f);
	Vector3 camera_forward(0.0f, 0.0f, 1.0f);
	Basis camera_view_basis;
	if (camera != nullptr) {
		const Transform3D camera_transform = camera->get_global_transform();
		camera_position = camera_transform.origin;
		camera_view_basis = camera_transform.affine_inverse().basis;
		camera_right = camera_transform.basis.get_column(0);
		camera_up = camera_transform.basis.get_column(1);
		camera_forward = -camera_transform.basis.get_column(2);
		if (camera_right.length_squared() > 0.0f)
			camera_right.normalize();
		else
			camera_right = Vector3(1.0f, 0.0f, 0.0f);
		if (camera_up.length_squared() > 0.0f)
			camera_up.normalize();
		else
			camera_up = Vector3(0.0f, 1.0f, 0.0f);
		if (camera_forward.length_squared() > 0.0f)
			camera_forward.normalize();
		else
			camera_forward = Vector3(0.0f, 0.0f, 1.0f);
	}

	impl_->refresh_environment(get_environment_source());
	impl_->build_render_snapshot(frame, camera_view_basis);
	for (std::size_t domain = 0; domain < impl_->compilers.size(); ++domain) {
		renderer::ParticleViewInput view;
		view.domain = domain == 0 ? renderer::ParticleRenderDomain::World :
				renderer::ParticleRenderDomain::FirstPerson;
		view.position = particle_vec(camera_position);
		view.right = particle_vec(camera_right);
		view.up = particle_vec(camera_up);
		view.forward = particle_vec(camera_forward);
		const renderer::ParticleDrawPacket &packet =
				impl_->compilers[domain].compile(impl_->render_snapshot, view);
		capture_packet_diagnostics(impl_->packets[domain], packet);
		if (domain == 0)
			impl_->publish_world_packet(packet, camera_position, camera_forward);
		else
			impl_->upload_first_person_packet(packet, hidden_);
	}
}

int64_t NovaParticleRenderer::get_rendered_quad_count() const {
	if (!impl_)
		return 0;
	std::size_t total = 0;
	for (const PacketDiagnostics &packet : impl_->packets) {
		if (packet.present)
			total += packet.debug.emitted_quads;
	}
	return static_cast<int64_t>(total);
}

int64_t NovaParticleRenderer::get_draw_command_count() const {
	if (!impl_)
		return 0;
	std::size_t total = 0;
	for (const PacketDiagnostics &packet : impl_->packets) {
		if (packet.present)
			total += packet.debug.draw_commands;
	}
	return static_cast<int64_t>(total);
}

Dictionary NovaParticleRenderer::get_debug_packet_report() const {
	Dictionary result;
	if (!impl_)
		return result;
	result["world"] = packet_report(impl_->packets[0]);
	result["first_person"] = packet_report(impl_->packets[1]);
	result["world_backend"] = impl_->world_effect->get_backend_report();
	result["first_person_backend"] = "array_mesh_fallback_tool_only";
	result["world_compositor_attached"] = impl_->attached_camera.is_valid();
	result["world_compositor_inherited_effects"] =
			impl_->inherited_world_compositor;
	result["world_mesh_instance"] = false;
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

Array NovaParticleRenderer::get_debug_emitter_bounds() const {
	Array result;
	if (!impl_)
		return result;
	for (std::size_t domain = 0; domain < impl_->packets.size(); ++domain) {
		const PacketDiagnostics &packet = impl_->packets[domain];
		if (!packet.present)
			continue;
		for (const renderer::ParticleEmitterDrawBounds &bounds :
				packet.emitter_bounds) {
			Dictionary value;
			value["emitter_id"] = godot_token(bounds.emitter_id);
			value["render_domain"] = static_cast<int>(domain);
			value["first_quad"] = static_cast<int64_t>(bounds.first_quad);
			value["quad_count"] = static_cast<int64_t>(bounds.quad_count);
			value["bounds_valid"] = bounds.bounds.valid;
			value["bounds"] = bounds.bounds.valid ? godot_aabb(bounds.bounds) : AABB();
			result.push_back(value);
		}
	}
	return result;
}

PackedStringArray NovaParticleRenderer::get_unresolved_texture_names() const {
	PackedStringArray result;
	if (!impl_)
		return result;
	result.resize(static_cast<int64_t>(impl_->unresolved_names.size()));
	for (std::size_t i = 0; i < impl_->unresolved_names.size(); ++i)
		result[static_cast<int64_t>(i)] =
				String::utf8(impl_->unresolved_names[i].c_str());
	return result;
}
