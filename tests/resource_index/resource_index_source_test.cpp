// ResourceIndex::mount_source (ADR 0046 S14): an embedder's own file set mounted in place of an
// install (the editor's project files). The index has what the source resolves and reads what it
// reads, a lookup policy changing nothing; it misses what the source misses; nothing is indexed by
// kind, no file is preferred loose, no archive is mounted; root_dir() is the label, so what asks
// "is something mounted" answers yes; a mount replaces the one before (a directory's, and a
// source's) and a clear lets the source go; the revision moves with a mount, a clear, and when the
// embedder says the files changed (mark_changed). An AssetStore over the index parses a model the
// source holds, keeps it while the revision stands, and parses it again once it moved.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/file_source.h>
#include <runtime/assets/asset_store.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

namespace {

namespace fs = std::filesystem;

// Files by name, compared without case; each with a stamp the test moves. What was asked of it.
struct MapSource final : opennova::FileSource {
	struct File {
		std::vector<uint8_t> bytes;
		uint64_t stamp = 1;
	};
	std::map<std::string, File> files;
	mutable std::vector<std::string> reads;
	void put(const std::string &name, const std::string &text) {
		File &file = files[opennova::strutil::to_lower(name)];
		file.bytes.assign(text.begin(), text.end());
		++file.stamp;
	}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		reads.push_back(name);
		const auto found = files.find(opennova::strutil::to_lower(name));
		if (found == files.end()) return false;
		out = found->second.bytes;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = files.find(opennova::strutil::to_lower(name));
		return found == files.end() ? 0 : found->second.stamp;
	}
};

std::string text_of(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

} // namespace

int main() {
	auto source = std::make_shared<MapSource>();
	source->put("Terrain.TRN", "terrain");
	source->put("items.def", "items");

	opennova::ResourceIndex index;
	const uint64_t before = index.revision();
	TEST_EXPECT(!index.mount_source(nullptr) && !index.last_error().empty() && index.root_dir().empty());
	TEST_EXPECT(index.mount_source(source));
	TEST_EXPECT(index.revision() != before && index.last_error().empty());
	TEST_EXPECT(index.root_dir() == opennova::ResourceIndex::kSourceRootDir);

	// It has and reads what the source does, in any case, whatever the lookup policy.
	std::vector<uint8_t> bytes;
	TEST_EXPECT(index.has_file("terrain.trn") && index.has_file("ITEMS.DEF"));
	TEST_EXPECT(index.read_file("terrain.trn", bytes) && text_of(bytes) == "terrain");
	for (const opennova::VfsLookupPolicy policy :
			{ opennova::VfsLookupPolicy::SessionDefault, opennova::VfsLookupPolicy::ForceLooseFirst,
					opennova::VfsLookupPolicy::ForceArchiveOnly }) {
		TEST_EXPECT(index.has_file("items.def", policy));
		bytes.clear();
		TEST_EXPECT(index.read_file("items.def", bytes, policy) && text_of(bytes) == "items");
		TEST_EXPECT(!index.has_file("missing.til", policy) && !index.read_file("missing.til", bytes, policy));
	}
	TEST_EXPECT(!index.has_file("missing.til") && !index.read_file("missing.til", bytes));
	// Nothing indexed by kind, nothing preferred loose, no archive, no expansion.
	TEST_EXPECT(index.resource_files("*").empty() && index.resource_files("terrain").empty());
	TEST_EXPECT(!index.prefers_loose_file("terrain.trn") && !index.has_mounted_archive() &&
			index.mounted_expansion().empty());
	// The gore set follows the marker's presence in the source.
	TEST_EXPECT(index.particle_extension() == ".ptu");
	source->put("fgn2.bin", "marker");
	TEST_EXPECT(index.particle_extension() == ".ptg");
	// The source is read as it stands at each call: a file it gains or changes shows at once.
	source->put("items.def", "items two");
	bytes.clear();
	TEST_EXPECT(index.read_file("items.def", bytes) && text_of(bytes) == "items two");

	// The revision: the embedder's word that the files changed moves it; nothing else here does.
	const uint64_t mounted = index.revision();
	TEST_EXPECT(index.has_file("items.def") && index.revision() == mounted);
	index.mark_changed();
	TEST_EXPECT(index.revision() != mounted);

	// A directory scanned over a source replaces it; a source mounted over a directory replaces that.
	const fs::path dir = fs::temp_directory_path() / "opennova_resource_index_source_test";
	fs::remove_all(dir);
	fs::create_directories(dir);
	std::ofstream(dir / "loose.trn", std::ios::binary) << "loose";
	TEST_EXPECT(index.scan(dir.string(), std::string(), opennova::VfsMountMode::LooseOnly));
	TEST_EXPECT(index.has_file("loose.trn") && !index.has_file("terrain.trn") &&
			index.root_dir() != opennova::ResourceIndex::kSourceRootDir && index.resource_files("terrain").size() == 1);
	TEST_EXPECT(index.mount_source(source));
	TEST_EXPECT(!index.has_file("loose.trn") && index.has_file("terrain.trn") && index.resource_files("terrain").empty());
	// Cleared: nothing mounted, the source let go.
	std::weak_ptr<MapSource> held = source;
	const uint64_t at_clear = index.revision();
	index.clear();
	TEST_EXPECT(index.revision() != at_clear && index.root_dir().empty() && !index.has_file("terrain.trn") &&
			!index.read_file("terrain.trn", bytes));
	source.reset();
	TEST_EXPECT(held.expired());
	fs::remove_all(dir);

	// An AssetStore over a source index: a model the source holds parsed once and kept while the
	// index's revision stands, parsed again after it moved; none while nothing is mounted.
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<uint8_t> armory = test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di");
	TEST_EXPECT(!armory.empty());
	auto models = std::make_shared<MapSource>();
	models->files["armory.3di"].bytes = armory;
	opennova::ResourceIndex model_index;
	opennova::assets::AssetStore store(&model_index);
	TEST_EXPECT(!store.has_source() && !store.model("armory"));
	TEST_EXPECT(model_index.mount_source(models));
	TEST_EXPECT(store.has_source());
	const opennova::assets::Model first = store.model("armory");
	TEST_EXPECT(first && first->lod_count > 0);
	const size_t reads = models->reads.size();
	TEST_EXPECT(store.model("ARMORY.3DI") == first && models->reads.size() == reads);
	model_index.mark_changed();
	const opennova::assets::Model again = store.model("armory");
	TEST_EXPECT(again && again != first && models->reads.size() == reads + 1);
	TEST_EXPECT(!store.model("nosuch"));
	return 0;
}
