#pragma once

// The effect catalog's NAME surface without the scene: which effect names the
// mounted PTL/PTU documents define (first registration wins, case-insensitive,
// exactly EffectScene::open's rule) plus the stable 1-based interned handles the
// WAC compiler binds FX literals to at compile time
// [orig: CEffectWorld_InternEffectHandle @0x5F7310 -> CEffectWorld_FindEffectDefByName
//  @0x5E34F0; an unknown name clones stockeffect under the requested name].
// MissionKernel keeps one of these so the compiler never opens a second
// EffectScene (group/emitter pools) just to bind names; the shell's EffectWorld
// re-interns every name by string, so the two intern orders need not match.

#include <runtime/particle/effect_scene.h> // EffectHandle, ParticleFile

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opennova::particle {

class EffectCatalogNames {
public:
	// Forget every catalog name and interned handle.
	void clear();
	// Register one parsed document's effect ids; a name already registered by an
	// earlier document keeps its first definition.
	void add_document(const ParticleFile &file);

	// Case-insensitive, stable 1-based handles. An unknown name aliases the
	// catalog's stockeffect definition; returns 0 when stockeffect is absent.
	EffectHandle intern(std::string_view effect_name);
	// Interned display names in handle order (handle value - 1 indexes it).
	std::vector<std::string> interned_names() const;

	std::size_t effect_count() const { return display_by_key_.size(); }
	bool has_stock_effect() const { return has_stock_effect_; }

private:
	std::unordered_map<std::string, std::string> display_by_key_; // folded -> catalog id
	std::unordered_map<std::string, EffectHandle> interned_by_key_;
	std::vector<std::string> interned_;
	bool has_stock_effect_ = false;
};

} // namespace opennova::particle
