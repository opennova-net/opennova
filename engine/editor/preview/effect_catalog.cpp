#include <editor/preview/effect_catalog.h>

#include <map>
#include <set>
#include <utility>

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>

namespace opennova::editor {

namespace {

constexpr size_t kNone = size_t(-1);

} // namespace

void PreviewEffectCatalog::clear() {
	if (!files_.empty() || !documents_.empty()) ++serial_;
	files_.clear();
	documents_.clear();
	document_of_.clear();
	scan_.reset();
	source_.reset();
	generation_ = 0;
	followed_ = false;
}

bool PreviewEffectCatalog::follow(const std::shared_ptr<const AssetScan> &scan,
		const std::shared_ptr<const ProjectAssetSource> &files) {
	if (!scan || !files) {
		const bool held = !files_.empty();
		clear();
		return held;
	}
	if (followed_ && scan == scan_ && files == source_ && files->generation() == generation_) return false;
	scan_ = scan;
	source_ = files;
	generation_ = files->generation();
	followed_ = true;
	// The kind's files by name (a name the scan lists twice is its first file's, as the build packs it),
	// and the gore set the project's own fgn2.bin picks, as the game's config load does.
	std::vector<std::string> names;
	std::map<std::string, const AssetEntry *> entries;
	for (const AssetEntry &entry : scan->entries) {
		if (entry.kind != AssetKind::Particles) continue;
		const AssetEntry *first = scan->find(entry.logical_name);
		if (!first || !entries.emplace(strutil::to_lower(first->logical_name), first).second) continue;
		names.push_back(first->logical_name);
	}
	const std::string gore = scan->find("fgn2.bin") ? ".ptg" : ".ptu";
	const std::vector<std::string> order = effect_file_order(names, gore);
	// What was read stays where its file's name, path and stamp did not move.
	std::map<std::string, std::pair<File, particle::EffectCatalogDocument>> kept;
	for (size_t i = 0; i < files_.size(); ++i) {
		std::pair<File, particle::EffectCatalogDocument> held{ files_[i], particle::EffectCatalogDocument() };
		if (document_of_[i] != kNone) held.second = std::move(documents_[document_of_[i]]);
		kept.emplace(files_[i].path, std::move(held));
	}
	std::vector<File> next_files;
	std::vector<particle::EffectCatalogDocument> next_documents;
	std::vector<size_t> next_of;
	bool moved = order.size() != files_.size();
	for (size_t i = 0; i < order.size(); ++i) {
		const AssetEntry &entry = *entries.at(strutil::to_lower(order[i]));
		const uint64_t stamp = files->stamp(entry.logical_name);
		File file;
		particle::EffectCatalogDocument document;
		const auto held = kept.find(entry.relative_path);
		if (held != kept.end() && held->second.first.name == entry.logical_name && held->second.first.stamp == stamp) {
			file = held->second.first;
			document = std::move(held->second.second);
		} else {
			file.name = entry.logical_name;
			file.path = entry.relative_path;
			file.stamp = stamp;
			std::vector<uint8_t> bytes;
			++reads_;
			if (!files->read(entry.logical_name, bytes)) {
				file.error.message = "the file could not be read";
			} else {
				document.source = entry.relative_path;
				file.read = particle::load_particles_from_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(),
						document.file, file.error);
			}
			moved = true;
		}
		if (!moved && (i >= files_.size() || files_[i].path != file.path)) moved = true;
		next_of.push_back(file.read ? next_documents.size() : kNone);
		if (file.read) next_documents.push_back(std::move(document));
		next_files.push_back(std::move(file));
	}
	files_ = std::move(next_files);
	documents_ = std::move(next_documents);
	document_of_ = std::move(next_of);
	if (moved) ++serial_;
	return moved;
}

const PreviewEffectCatalog::File *PreviewEffectCatalog::file_at(const std::string &path) const {
	for (const File &file : files_)
		if (file.path == path) return &file;
	return nullptr;
}

const particle::ParticleFile *PreviewEffectCatalog::document_at(const std::string &path) const {
	for (size_t i = 0; i < files_.size(); ++i)
		if (files_[i].path == path) return document_of_[i] == kNone ? nullptr : &documents_[document_of_[i]].file;
	return nullptr;
}

particle::EffectClosure PreviewEffectCatalog::closure(const std::string &name,
		const particle::EffectSceneConfig &limits) const {
	return particle::effect_closure(documents_, name, limits);
}

particle::EffectSceneConfig PreviewEffectCatalog::closures(const std::vector<std::string> &names,
		const particle::EffectSceneConfig &limits, std::vector<particle::EffectClosure> &each) const {
	particle::EffectSceneConfig out;
	out.simulation_tick_seconds = limits.simulation_tick_seconds;
	out.max_live_groups = limits.max_live_groups;
	out.max_live_emitters = limits.max_live_emitters;
	out.random_seed = limits.random_seed;
	each.clear();
	// One document of every effect and definition the closures hold, each once by its name as the catalog
	// folds names (the closures read the same first registrations, so a name held twice is one definition),
	// and every table once (each closure carries them all).
	particle::EffectCatalogDocument merged;
	std::set<std::string> effects, definitions;
	bool tables = false;
	for (const std::string &name : names) {
		each.push_back(closure(name, limits));
		const particle::EffectClosure &one = each.back();
		if (one.config.documents.empty()) continue;
		const particle::EffectCatalogDocument &document = one.config.documents.front();
		if (merged.source.empty()) merged.source = document.source;
		for (const particle::EffectDef &effect : document.file.effects)
			if (effects.insert(strutil::to_lower(effect.id)).second) merged.file.effects.push_back(effect);
		for (const particle::ParticleDef &definition : document.file.particles)
			if (definitions.insert(strutil::to_lower(definition.id)).second) merged.file.particles.push_back(definition);
		if (!tables) {
			merged.file.tables = document.file.tables;
			tables = true;
		}
	}
	if (!merged.file.effects.empty()) out.documents.push_back(std::move(merged));
	return out;
}

} // namespace opennova::editor
