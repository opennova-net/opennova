#pragma once

// The simulation's own parse-once .3di source (ADR 0028). Resolves a graphic
// name through the mounted resource index with the same name rule the render
// path applies (<basename>.3di), parses it once, and owns the parsed model
// until reset. Sim correctness stops depending on render-resource lifetimes;
// a headless embedder parses only this side.

#include <formats/threedi/threedi_3di3.h>

#include <string>
#include <unordered_map>

namespace opennova {
class ResourceIndex;
}

namespace opennova::simassets {

class SimModelCache {
public:
	SimModelCache() = default;
	~SimModelCache();
	SimModelCache(const SimModelCache &) = delete;
	SimModelCache &operator=(const SimModelCache &) = delete;

	// Non-owning; the embedder keeps the index alive for the cache's lifetime
	// (the adapter pins its ResourceRoot). Switching the index resets.
	void set_index(const opennova::ResourceIndex *index);
	bool has_index() const { return index_ != nullptr; }

	// Frees every parsed model and forgets negatives.
	void reset();

	// Parse-once lookup by graphic name (any authored path/extension spelling;
	// the flat <basename>.3di rule). nullptr — cached — when the file is
	// missing or unparsable.
	const Threedi3di3 *model_for(const std::string &graphic);

	// Load-budget stats (mission-load delta accounting).
	size_t parsed_count() const { return parsed_count_; }
	size_t negative_count() const { return negative_count_; }

private:
	const opennova::ResourceIndex *index_ = nullptr;
	// Keyed by the lowercased basename; value nullptr = cached negative.
	std::unordered_map<std::string, Threedi3di3 *> by_name_;
	size_t parsed_count_ = 0;
	size_t negative_count_ = 0;
};

} // namespace opennova::simassets
