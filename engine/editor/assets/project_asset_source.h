#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/document.h>

namespace opennova::editor {

// The project's files as the engine looks them up (ADR 0046 d6: a flat logical name, the
// path is organization only), for what reads the project the way the game reads its
// mounted files (the menu preview). Every file the build packs is here under its name,
// the subfolders and the import outputs under `.opennova/` included; an import source
// (a `.png` with an `.import` record) and an archive are not, as the built game never
// sees them. A name the scan lists twice resolves to its first entry (the scan reports
// the duplicate). A payload
// decodes the way a document's does (the target game's SCR policy). An open document
// stands in for its file, with the bytes its Save would write (serialized once per
// revision); one that cannot serialize leaves the file in place.
class ProjectAssetSource : public FileSource {
public:
	void set_scan(const std::string &root, const AssetScan &scan, const std::string &target_game);
	void set_open(const std::vector<std::shared_ptr<const Document>> &open);
	void clear();

	bool read(const std::string &name, std::vector<uint8_t> &out) const override;
	// A file's stamp follows its size and last write as the scan saw them (a file changed
	// outside the editor moves on the next rescan); an open document's follows its
	// identity and revision.
	uint64_t stamp(const std::string &name) const override;
	// Moves whenever a stamp may have: a rescan, or an open document edited, undone,
	// opened or closed.
	uint64_t generation() const { return generation_; }
	// The project-relative path a name resolves to ("" when none).
	std::string path_of(const std::string &name) const;

private:
	struct Entry {
		std::string relative;
		uint64_t size = 0;
		int64_t modified = 0;
	};
	struct Open {
		std::shared_ptr<const Document> document;
		uint64_t identity = 0;
		uint64_t revision = 0;
	};
	struct Serialized {
		bool made = false;
		uint64_t identity = 0;
		uint64_t revision = 0;
		bool ok = false;
		std::vector<uint8_t> bytes;
	};
	const Entry *entry_(const std::string &name) const;
	const Serialized *serialized_(const Open &open, const std::string &relative) const;

	std::string root_;
	int scr_policy_ = 0;
	std::map<std::string, Entry> files_; // normalized logical name
	std::map<std::string, Open> open_;   // project-relative path
	mutable std::map<std::string, Serialized> serialized_by_path_;
	uint64_t generation_ = 0;
};

} // namespace opennova::editor
