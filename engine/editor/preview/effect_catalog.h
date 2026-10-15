#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <formats/particle/parser.h>
#include <runtime/particle/effect_closure.h>
#include <runtime/particle/effect_scene.h>

namespace opennova::editor {

class ProjectAssetSource;
struct AssetScan;

// The effect catalog as the game loads it at a mission's start (ADR 0046 DI-14), of a project's files:
// every particle file in the effect system's order (opennova::effect_file_order: every .ptl, then the
// gore set's files, .ptg where the project holds fgn2.bin, else .ptu [orig: CEffectSystem_Init @
// 0x5F6070; Game_LoadConfig @ 0x5514E8..0x5514FA]), each read through the game's reader
// (formats/particle load_particles) from the project's files as the game would read them were they
// saved now (ProjectAssetSource: an open document stands in for its file), and read again only when its
// stamp moves. What an effect's picture spawns is its closure over these documents
// (particle::effect_closure: the definitions the engine's catalog registers for it). A preview of
// effects keeps one (the effect viewport's; later a definition's picture, a model's Shoot).
class PreviewEffectCatalog {
public:
	// One particle file of the catalog, in its order: its logical name and path, the stamp it was read
	// at, and whether the reader read it (where it stopped, else).
	struct File {
		std::string name;
		std::string path;
		uint64_t stamp = 0;
		bool read = false;
		particle::ParseError error;
	};

	// The project's files followed: the files of the kind in the effect system's order, each read
	// again where its stamp moved; nothing read where the source's generation and the scan did not
	// move since the last follow. True when what the catalog holds moved (a file read again, one added,
	// gone or moved in the order).
	bool follow(const std::shared_ptr<const AssetScan> &scan, const std::shared_ptr<const ProjectAssetSource> &files);
	// Nothing held (no project).
	void clear();

	const std::vector<File> &files() const { return files_; }
	// The documents the reader read, in the catalog's order: what the engine's effect scene opens.
	const std::vector<particle::EffectCatalogDocument> &documents() const { return documents_; }
	// The file at `path` (project-relative) among them; null for none.
	const File *file_at(const std::string &path) const;
	// The document read of the file at `path`; null for one the reader did not read, or none.
	const particle::ParticleFile *document_at(const std::string &path) const;
	// What a spawn of `name` reads of the catalog (particle::effect_closure).
	particle::EffectClosure closure(const std::string &name, const particle::EffectSceneConfig &limits) const;
	// What spawns of several names read of it together (a definition's picture, DI-21:
	// particle::effect_closures): each name's closure (`each`, in the order of `names`) and one config
	// holding every effect and definition they instantiate, each once, and every table, over which a scene
	// spawns each name as one opened over its own closure does (none: no name found an effect).
	particle::EffectSceneConfig closures(const std::vector<std::string> &names, const particle::EffectSceneConfig &limits,
			std::vector<particle::EffectClosure> &each) const;
	// Moves with every change of what it holds; how many files were read so far (a test's measure).
	uint64_t serial() const { return serial_; }
	uint64_t reads() const { return reads_; }

private:
	std::vector<File> files_;
	std::vector<particle::EffectCatalogDocument> documents_;
	std::vector<size_t> document_of_; // files_' index -> documents_' (npos: not read)
	std::shared_ptr<const AssetScan> scan_;
	std::shared_ptr<const ProjectAssetSource> source_;
	uint64_t generation_ = 0;
	bool followed_ = false;
	uint64_t serial_ = 0;
	uint64_t reads_ = 0;
};

} // namespace opennova::editor
