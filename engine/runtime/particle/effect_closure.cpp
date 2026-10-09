#include <runtime/particle/effect_closure.h>

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace opennova::particle {

namespace {

// The catalog's name key: every by-name walk of the effect system compares with _stricmp
// (EffectScene::open's fold).
std::string fold(std::string_view value) {
	std::string folded;
	folded.reserve(value.size());
	for (const char ch : value) folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
	return folded;
}

struct Registered {
	const ParticleDef *definition = nullptr;
	const std::string *source = nullptr;
};

} // namespace

EffectClosure effect_closure(const std::vector<EffectCatalogDocument> &documents, std::string_view name,
		const EffectSceneConfig &limits) {
	EffectClosure out;
	// The registrations EffectScene::open makes: each particle definition and each effect by its first
	// document, in the documents' order.
	std::unordered_map<std::string, Registered> definitions;
	std::unordered_map<std::string, std::pair<const EffectDef *, const std::string *>> effects;
	std::unordered_map<std::string, std::size_t> again;
	for (const EffectCatalogDocument &document : documents) {
		for (const ParticleDef &definition : document.file.particles)
			definitions.emplace(fold(definition.id), Registered{&definition, &document.source});
		for (const EffectDef &effect : document.file.effects) {
			const std::string key = fold(effect.id);
			if (!effects.emplace(key, std::make_pair(&effect, &document.source)).second) ++again[key];
		}
	}
	auto found = effects.find(fold(name));
	if (found != effects.end()) {
		out.found = true;
	} else {
		// An unknown name interns as a clone of stockeffect [orig: CEffectWorld_InternEffectHandle
		// @ 0x5F7310].
		found = effects.find("stockeffect");
		if (found == effects.end()) return out;
		out.stock = true;
	}
	const EffectDef &effect = *found->second.first;
	out.effect = effect.id;
	out.source = *found->second.second;
	if (const auto shadows = again.find(found->first); shadows != again.end()) out.shadowed = shadows->second;
	out.members = effect.pdefs;
	out.unresolved_member = out.members.size();
	// The members, all or nothing [orig: CEffectBank_ResolveAllEntries @ 0x5E4920: the first miss
	// clears the effect's resolved list].
	for (std::size_t member = 0; member < out.members.size(); ++member)
		if (!definitions.count(fold(out.members[member]))) {
			out.unresolved_member = member;
			break;
		}
	EffectCatalogDocument closure;
	closure.source = out.source;
	closure.file.effects.push_back(effect);
	if (out.unresolved_member == out.members.size()) {
		// Each member and its child chain, as EffectScene::spawn instantiates them (a chain of 4 at
		// most), each definition held once.
		std::unordered_set<const ParticleDef *> held;
		for (std::size_t member = 0; member < out.members.size(); ++member) {
			auto at = definitions.find(fold(out.members[member]));
			for (int depth = 0; depth < 4 && at != definitions.end(); ++depth) {
				const ParticleDef &definition = *at->second.definition;
				if (held.insert(&definition).second) {
					closure.file.particles.push_back(definition);
					out.definitions.push_back({definition.id, *at->second.source, member, depth > 0});
				}
				at = definition.child_id.empty() ? definitions.end() : definitions.find(fold(definition.child_id));
			}
		}
	}
	// Every table in the documents' order: a curve finds the first base of its name, a modified one
	// the last (CParticleManager_FindTableDefByName @ 0x5E9540), as over the whole catalog.
	for (const EffectCatalogDocument &document : documents)
		closure.file.tables.insert(closure.file.tables.end(), document.file.tables.begin(), document.file.tables.end());
	out.config.simulation_tick_seconds = limits.simulation_tick_seconds;
	out.config.max_live_groups = limits.max_live_groups;
	out.config.max_live_emitters = limits.max_live_emitters;
	out.config.random_seed = limits.random_seed;
	out.config.documents.push_back(std::move(closure));
	return out;
}

EffectSceneConfig effect_closures(const std::vector<EffectCatalogDocument> &documents,
		const std::vector<std::string> &names, const EffectSceneConfig &limits,
		std::vector<EffectClosure> &each) {
	EffectSceneConfig out;
	out.simulation_tick_seconds = limits.simulation_tick_seconds;
	out.max_live_groups = limits.max_live_groups;
	out.max_live_emitters = limits.max_live_emitters;
	out.random_seed = limits.random_seed;
	each.clear();
	EffectCatalogDocument merged;
	std::unordered_set<std::string> effects, definitions;
	bool tables = false;
	for (const std::string &name : names) {
		each.push_back(effect_closure(documents, name, limits));
		const EffectClosure &one = each.back();
		if (one.config.documents.empty()) continue;
		const EffectCatalogDocument &document = one.config.documents.front();
		if (merged.source.empty()) merged.source = document.source;
		for (const EffectDef &effect : document.file.effects)
			if (effects.insert(fold(effect.id)).second) merged.file.effects.push_back(effect);
		for (const ParticleDef &definition : document.file.particles)
			if (definitions.insert(fold(definition.id)).second) merged.file.particles.push_back(definition);
		if (!tables) {
			merged.file.tables = document.file.tables;
			tables = true;
		}
	}
	if (!merged.file.effects.empty()) out.documents.push_back(std::move(merged));
	return out;
}

} // namespace opennova::particle
