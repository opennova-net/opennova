#pragma once

// The game's texture registry: every texture a keyed loader makes is kept under a key the
// loader prints from the name it was asked for, and the loader looks that key up before it
// opens any file. So the first load of a key decides the texture every later request of the
// key draws: its file, its reader and its creation flags; the later request's own loader
// rule, reader, transform and flags never run. The lookup ignores case. A load that fails
// keeps nothing, so the next request of its key loads by its own loader. Witness record:
// docs/render/render-material-re.md ("The texture registry").

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace opennova::renderer {

// The key a model texture row of runtime `type` (material_texture_runtime_type) keeps its
// texture under, folded to lower case as the lookup compares it: the row's name as written
// (the whole name, not the stage loader's cut query) and its loader's suffix, the channel the
// dispatcher passes being 0 for every loader that prints one:
// - 0, 1, 2 and 8: "name:1". The stage and plain loaders print the same key, so a type-1 row
//   takes the texture a type-0 row of the name made (its .dds sibling, say), and the other way.
// - 4 and 5, the normal maps: "name:BA:1".
// - 6, the horizon volume: "name:HZ:1"; 7, the occlusion map: "name:AO:1".
// - 16 to 18, the chunk producers: "name:NML", "name:HZ" and "name:AO".
// - any other type (the dispatcher's default, which loads nothing) and an empty name (a row
//   the resolver skips): "", no key.
// The volume textures (types 6 and 17) are kept in a list of their own, so their keys never
// meet a 2D texture's: they start with a NUL byte, which no name holds.
// [orig: Material_LoadStageTexture @ 0x5B16F0, the loader by type @ 0x5B1737, channel 0 at
//  @ 0x5B173F, @ 0x5B1755, @ 0x5B176B, @ 0x5B1788, @ 0x5B17AA, @ 0x5B17C7;
//  Texture_LoadByNameWithChannel @ 0x58B4AD ("%s:%c", channel + '1');
//  Texture_LoadAndRegister @ 0x58B7CD (the same);
//  Texture_LoadAsNormalMap @ 0x58C4CE ("%s:BA:%c");
//  sub_58A580 @ 0x58A5BA ("%s:HZ:%c", looked up in the volume list GStaticVB_FindByName
//  @ 0x686B60); sub_58CE10 @ 0x58CE4A ("%s:AO:%c");
//  sub_58F350 @ 0x58F399 ("%s:NML"); sub_58F470 @ 0x58F4B9 ("%s:HZ", the volume list);
//  Texture_LoadTGAAlphaOverlay @ 0x58F5D9 ("%s:AO");
//  the row's name copied byte for byte, Material_ConvertDefinition @ 0x5B0410..0x5B041A;
//  rows with an empty name skipped, Material_ResolveEffectSubobjectsAndShader @ 0x5B1890]
std::string texture_registry_key(std::string_view name, uint8_t runtime_type);

// The stage loader's other callers print their keys the same way: channel 0, so "name:1"
// shared with the model rows, for the sight cards, the face textures, the tip icons, the
// view effects, the weather and the scorch set; channel 1, so "name:2", for the terrain's
// detail maps. Their loads by name go through the embedder's own caches, not this registry
// (render-material-re.md "The texture registry").
// [orig: WeaponDef_CreateBlendNamedMaterial @ 0x54019E; Shadow_DecalLoadTextures @ 0x5880F5;
//  CTipSystem_Init @ 0x5B6996; WeatherParticle_LoadTextures @ 0x5DE845;
//  Terrain_LoadScorchTextures @ 0x604CE2 (channel 0 pushed); PolyTrn_InitTextures
//  @ 0x60ABE8, @ 0x60ABF5, @ 0x60AC07, @ 0x60AD99, @ 0x60AE59, @ 0x60AF81 (channel 1)]

// The registry over the embedder's texture handle: one texture per key, kept by the first
// load that made one. `Texture` is copied out (a reference-counted handle).
// [orig: GTexture_FindByName @ 0x687C60, a stricmp walk of the list @ 0x687C7F; every keyed
//  loader returns a found texture before it opens a file (Texture_LoadByNameWithChannel
//  @ 0x58B4BA..0x58B4C4, Texture_LoadAndRegister @ 0x58B7DA..0x58B7E4 -> @ 0x58B968,
//  Texture_LoadAsNormalMap @ 0x58C4DB..0x58C4E5); a load that fails is freed, never listed
//  (GTexture_FindOrCreateFromData @ 0x676CC7..0x676CD5)]
template <typename Texture>
class TextureRegistry {
public:
	// The texture registered under `key`, else what `load()` makes, registered when
	// `loaded(texture)` says it loaded. An empty key (no keyed loader) registers nothing:
	// `load()` answers each time. A `load()` that registers the key itself (a loader
	// that delegates to another of the same key) leaves that registration standing.
	template <typename Load, typename Loaded>
	Texture find_or_load(const std::string &key, Load &&load, Loaded &&loaded) {
		if (key.empty()) return load();
		const auto found = textures_.find(key);
		if (found != textures_.end()) return found->second;
		Texture texture = load();
		if (!loaded(texture)) return texture;
		return textures_.emplace(key, std::move(texture)).first->second;
	}
	// The texture registered under `key`, null when none.
	const Texture *find(const std::string &key) const {
		const auto found = textures_.find(key);
		return found == textures_.end() ? nullptr : &found->second;
	}
	size_t size() const { return textures_.size(); }
	void clear() { textures_.clear(); }

private:
	std::unordered_map<std::string, Texture> textures_;
};

} // namespace opennova::renderer
