#pragma once

// What one effect's spawn reads of the effect catalog, without the scene: the effect the catalog
// registers for a name (first registration wins, case-insensitive), every particle definition its
// pdefs members and their child_id chains resolve to, and every table, which a catalog of those
// alone compiles to the same effect as the whole (EffectScene::open's rules: the same first
// registrations, the same all-or-nothing member resolve, the same curves). An embedder that draws
// one effect (the OpenNova Editor's effect preview, ADR 0046 DI-14) opens a scene over the closure
// rather than over every document, so a particle renderer's catalog holds that effect's graphics
// alone [orig: CEffectWorld_FindEffectDefByName @ 0x5E34F0; CEffectWorld_FindParticleDefByName
// @ 0x5E41D0; CEffectBank_ResolveAllEntries @ 0x5E4920; CEffectWorld_InternEffectHandle @ 0x5F7310
// for an unknown name's stockeffect].

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <runtime/particle/effect_scene.h>

namespace opennova::particle {

// One particle definition the effect's spawn instantiates: its id as the registered definition
// writes it, the document that registered it, and the member (a pdefs entry's index) or the child
// chain it is reached through.
struct EffectClosureDefinition {
	std::string id;
	std::string source;
	std::size_t member = 0; // the pdefs entry it is (or whose child chain it is in)
	bool child = false;     // reached through a child_id, not a pdefs entry itself
};

struct EffectClosure {
	// The catalog registers an effect of the name; else, `stock`, the catalog's stockeffect stands
	// in for it (the game interns an unknown name as a stockeffect clone), else neither (an unknown
	// name with no stockeffect interns as no effect at all).
	bool found = false;
	bool stock = false;
	// The effect the spawn reads: its id as registered, the document that registered it first, and
	// how many documents define it again after that one (they are passed over).
	std::string effect;
	std::string source;
	std::size_t shadowed = 0;
	// Its pdefs as written, and the first that no definition registers (members.size() when every one
	// resolves): an effect with one unresolved member spawns nothing at all.
	std::vector<std::string> members;
	std::size_t unresolved_member = 0;
	// The definitions it instantiates, in the order the spawn makes their emitters (each member,
	// then its child chain, up to EffectScene's depth of 4), each once.
	std::vector<EffectClosureDefinition> definitions;
	// The catalog that compiles the same effect: one document holding the effect, the definitions it
	// instantiates and every table of `documents` in their order. Empty when no effect is found.
	EffectSceneConfig config;

	bool spawns() const { return (found || stock) && unresolved_member == members.size() && !members.empty(); }
};

// The closure of `name` over `documents` in their load order (EffectSceneConfig::documents), its
// config's limits and seed taken from `limits`.
EffectClosure effect_closure(const std::vector<EffectCatalogDocument> &documents, std::string_view name,
		const EffectSceneConfig &limits = EffectSceneConfig());

} // namespace opennova::particle
