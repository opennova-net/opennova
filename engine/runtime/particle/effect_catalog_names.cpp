#include <runtime/particle/effect_catalog_names.h>

#include <cctype>
#include <limits>

namespace opennova::particle {

namespace {

// The same ASCII fold every by-name walk in the effect system uses
// [orig: CEffectWorld_FindEffectDefByName @0x5E34F0 stricmp].
std::string fold_name(std::string_view value) {
	std::string folded;
	folded.reserve(value.size());
	for (const char ch : value)
		folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
	return folded;
}

} // namespace

void EffectCatalogNames::clear() {
	display_by_key_.clear();
	interned_by_key_.clear();
	interned_.clear();
	has_stock_effect_ = false;
}

void EffectCatalogNames::add_document(const ParticleFile &file) {
	for (const EffectDef &effect : file.effects) {
		const std::string key = fold_name(effect.id);
		if (display_by_key_.find(key) != display_by_key_.end()) continue;
		display_by_key_.emplace(key, effect.id);
		if (key == "stockeffect") has_stock_effect_ = true;
	}
}

EffectHandle EffectCatalogNames::intern(std::string_view effect_name) {
	const std::string key = fold_name(effect_name);
	const auto interned = interned_by_key_.find(key);
	if (interned != interned_by_key_.end()) return interned->second;
	std::string display_name;
	const auto found = display_by_key_.find(key);
	if (found != display_by_key_.end()) {
		display_name = found->second;
	} else if (has_stock_effect_) {
		display_name.assign(effect_name.begin(), effect_name.end());
	} else {
		return {};
	}
	if (interned_.size() >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
		return {};
	interned_.push_back(std::move(display_name));
	const EffectHandle handle{static_cast<std::uint32_t>(interned_.size())};
	interned_by_key_.emplace(key, handle);
	return handle;
}

std::vector<std::string> EffectCatalogNames::interned_names() const {
	return interned_;
}

} // namespace opennova::particle
