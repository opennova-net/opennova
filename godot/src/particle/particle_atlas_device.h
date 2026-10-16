#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>

#include <formats/particle/particle.h>
#include <runtime/renderer/particle_atlas.h>

#include "particle/particle_compositor.h"

namespace godot {

// The particle atlas's device side for one renderer: the catalog's graphics
// registered as atlas frames (renderer::ParticleAtlasBuilder), the page
// textures built under the session's page flags, and the immutable page
// snapshot the World compositor effects upload from. A definition keeps its
// per-layer frame entries, so the frame a quad draws resolves to an atlas
// entry here.
class ParticleAtlasDevice {
public:
	static constexpr int kGraphicLayerCount = 4;
	static constexpr std::size_t kMissingEntry = std::numeric_limits<std::size_t>::max();

	struct Page {
		std::uint8_t type = 0;
		int side = 0;
		// The page's retail levels and the last of them, the stage's point-mip
		// bound (renderer::particle_atlas_page_texture / _last_level).
		int levels = 0;
		float max_lod = 0.0f;
		// The uploaded image carries a mip chain.
		bool mipmapped = false;
		// The device texture's level-0 side (the page's after its halvings) and
		// whether it is DXT5 (D-RMAT-24).
		int device_side = 0;
		bool dxt5 = false;
		Ref<ImageTexture> texture;
	};

	struct LayerVisual {
		bool present = false;
		std::uint8_t type = 0;
		int flip_frames = 1;
		std::vector<std::size_t> frame_entries;
	};

	struct DefinitionVisual {
		std::array<LayerVisual, kGraphicLayerCount> layers;
		// The ambient tint pick (engine: runtime/renderer/particle_quad_inputs.h).
		bool ambient_lit = false;
	};

	using Definitions = std::shared_ptr<const std::vector<opennova::particle::ParticleDef>>;

	// A catalog built for other definitions or other page flags, or one marked
	// stale, rebuilds on the next render.
	bool needs_rebuild(const Definitions &p_definitions, std::uint32_t p_page_flags) const;
	void invalidate() { dirty_ = true; }
	// Registers every definition's graphics (their flipbook frames by the
	// retail frame names) and builds the pages. A frame no provider or texture
	// directory resolves is unresolved; with the procedural fallback on it
	// draws a soft white disc per type, else nothing.
	void rebuild(const Definitions &p_definitions, const Callable &p_provider,
			const String &p_texture_dir, bool p_procedural_fallback, std::uint32_t p_page_flags);

	const std::vector<DefinitionVisual> &definition_visuals() const { return definition_visuals_; }
	const std::vector<opennova::renderer::ParticleAtlasEntry> &entries() const { return entries_; }
	const std::vector<Page> &pages() const { return pages_; }
	const std::shared_ptr<const ParticleAtlasSnapshot> &snapshot() const { return snapshot_; }
	std::uint32_t page_flags() const { return page_flags_; }
	const std::vector<std::string> &unresolved_names() const { return unresolved_names_; }
	std::size_t rejected_entries() const { return rejected_entries_; }

private:
	Definitions definitions_;
	std::vector<DefinitionVisual> definition_visuals_;
	std::vector<opennova::renderer::ParticleAtlasEntry> entries_;
	std::vector<Page> pages_;
	std::shared_ptr<const ParticleAtlasSnapshot> snapshot_;
	std::uint64_t generation_ = 0;
	std::vector<std::string> unresolved_names_;
	std::size_t rejected_entries_ = 0;
	bool dirty_ = true;
	// The creation flags the current pages were built with
	// (renderer::particle_atlas_page_creation_flags).
	std::uint32_t page_flags_ = opennova::renderer::kParticlePageCreationFlags;
};

} // namespace godot
