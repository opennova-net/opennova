#include "particle/particle_atlas_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <base/io/strutil.h>
#include <runtime/renderer/particle_quad_inputs.h>
#include <runtime/renderer/texture_compression.h>

#include "particle/particle_atlas_page_upload.h"
#include "util/string_convert.h"
#include "util/texture_path_resolver.h"

namespace godot {

namespace {

using opennova::particle::GraphicLayer;

Ref<Image> load_particle_image(const Callable &provider,
		const String &texture_dir, const std::string &name) {
	const String candidate = opennova::to_gd(name);
	Ref<Texture2D> texture;
	if (provider.is_valid())
		texture = provider.call(candidate);
	if (texture.is_null() && !texture_dir.is_empty())
		texture = opennova::load_texture_from_dir(texture_dir, candidate,
				opennova::renderer::TextureLoader::Particle);
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

} // namespace

bool ParticleAtlasDevice::needs_rebuild(const Definitions &p_definitions,
		std::uint32_t p_page_flags) const {
	return dirty_ || definitions_.get() != p_definitions.get() ||
			page_flags_ != p_page_flags;
}

void ParticleAtlasDevice::rebuild(const Definitions &p_definitions,
		const Callable &p_provider, const String &p_texture_dir,
		bool p_procedural_fallback, std::uint32_t p_page_flags) {
	definitions_ = p_definitions;
	page_flags_ = p_page_flags;
	definition_visuals_.clear();
	entries_.clear();
	pages_.clear();
	unresolved_names_.clear();
	rejected_entries_ = 0;
	dirty_ = false;
	opennova::renderer::ParticleAtlasBuilder builder;
	if (p_definitions)
		definition_visuals_.resize(p_definitions->size());
	std::unordered_map<std::string, std::size_t> entry_lookup;
	std::unordered_set<std::string> unresolved_lookup;
	std::unordered_map<int, std::size_t> fallback_entries;
	Ref<Image> fallback_image;

	auto mark_unresolved = [&](const std::string &name) {
		if (name.empty())
			return;
		const std::string key = opennova::strutil::to_lower(name);
		if (unresolved_lookup.insert(key).second)
			unresolved_names_.push_back(name);
	};

	auto register_frame = [&](const std::string &name,
			std::uint8_t type) -> std::size_t {
		const std::string key = std::to_string(static_cast<int>(type)) +
				"|" + opennova::strutil::to_lower(name);
		const auto found = entry_lookup.find(key);
		if (found != entry_lookup.end())
			return found->second;

		Ref<Image> image;
		bool used_fallback = false;
		if (!name.empty())
			image = load_particle_image(p_provider, p_texture_dir, name);
		if (image.is_null()) {
			mark_unresolved(name);
			if (!p_procedural_fallback) {
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

	for (std::size_t definition_index = 0; p_definitions &&
			definition_index < p_definitions->size(); ++definition_index) {
		const opennova::particle::ParticleDef &definition =
				(*p_definitions)[definition_index];
		DefinitionVisual &visual = definition_visuals_[definition_index];
		visual.ambient_lit = opennova::renderer::particle_def_ambient_tinted(definition);
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
	entries_ = std::move(build.entries);
	rejected_entries_ = build.rejected_entries;
	pages_.reserve(build.pages.size());
	auto snapshot = std::make_shared<ParticleAtlasSnapshot>();
	snapshot->generation = ++generation_;
	snapshot->pages.reserve(build.pages.size());
	for (opennova::renderer::ParticleAtlasPage &source : build.pages) {
		Page page;
		page.type = source.type;
		page.side = source.image.width;
		// The page's device texture under the session's two words: at most
		// three levels sampled with point mips (D-RMAT-23), DXT5 and halved
		// once on the low branches (D-RMAT-24).
		const ParticleAtlasPageUpload device = particle_atlas_page_upload(
				opennova::renderer::particle_atlas_page_texture(source.image, p_page_flags));
		page.levels = static_cast<int>(device.level_count);
		page.device_side = static_cast<int>(device.side);
		page.dxt5 = device.dxt5;
		page.max_lod = static_cast<float>(
				opennova::renderer::particle_atlas_page_last_level(page.side, p_page_flags));
		if (device.image.is_valid()) {
			page.mipmapped = device.image->has_mipmaps();
			page.texture = ImageTexture::create_from_image(device.image);
		}
		pages_.push_back(page);

		ParticleAtlasPageSnapshot upload;
		upload.type = source.type;
		upload.side = device.side;
		upload.levels = device.level_count;
		upload.dxt5 = device.dxt5;
		upload.data = device.levels;
		snapshot->pages.push_back(std::move(upload));
	}
	snapshot_ = std::shared_ptr<const ParticleAtlasSnapshot>(std::move(snapshot));
}

} // namespace godot
