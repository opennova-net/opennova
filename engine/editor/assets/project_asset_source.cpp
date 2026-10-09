#include <editor/assets/project_asset_source.h>

#include <filesystem>

#include <base/gameprofile/gameprofile.h>
#include <base/io/hash.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_kinds.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

uint64_t text_hash(const std::string &text) {
	return io::fnv1a64_bytes(io::kFnv1a64Offset, text.data(), text.size());
}

// The flat name a lookup names, however the caller spelled a path.
std::string flat_name(const std::string &name) {
	const size_t slash = name.find_last_of("/\\");
	return pff::normalized_logical_name(slash == std::string::npos ? name : name.substr(slash + 1));
}

} // namespace

void ProjectAssetSource::set_scan(const std::string &root, const AssetScan &scan, const std::string &target_game) {
	root_ = root;
	scr_policy_ = gameprofile::gameprofile_scr_policy_for_code(target_game.c_str());
	files_.clear();
	for (const AssetEntry &asset : scan.entries) {
		// What the build packs, and nothing else: an import source stays out (its outputs
		// are in the scan), and so do an archive (the build refuses one) and a file of no kind the
		// game knows (S13 A8), as in plan_build.
		if (!asset_kind_packed(asset.kind)) continue;
		files_.emplace(pff::normalized_logical_name(asset.logical_name), Entry{asset.relative_path, asset.size_bytes, asset.modified_ticks});
	}
	++generation_;
}

void ProjectAssetSource::set_open(const std::vector<std::shared_ptr<const DocumentBase>> &open) {
	std::map<std::string, Open> next;
	for (const auto &document : open)
		if (document)
			next[document->path()] =
					Open{document, document->identity(), document->load_generation(), document->revision()};
	bool moved = next.size() != open_.size();
	for (auto a = next.begin(), b = open_.begin(); !moved && a != next.end(); ++a, ++b)
		moved = a->first != b->first || a->second.identity != b->second.identity ||
				a->second.load_generation != b->second.load_generation ||
				a->second.revision != b->second.revision;
	open_ = std::move(next);
	for (auto it = serialized_by_path_.begin(); it != serialized_by_path_.end();) {
		if (open_.count(it->first)) ++it;
		else it = serialized_by_path_.erase(it);
	}
	if (moved) ++generation_;
}

void ProjectAssetSource::clear() {
	root_.clear();
	files_.clear();
	open_.clear();
	serialized_by_path_.clear();
	++generation_;
}

const ProjectAssetSource::Entry *ProjectAssetSource::entry_(const std::string &name) const {
	const auto found = files_.find(flat_name(name));
	return found != files_.end() ? &found->second : nullptr;
}

const ProjectAssetSource::Serialized *ProjectAssetSource::serialized_(const Open &open, const std::string &relative) const {
	// The document as it is now (its identity, load and revision are live, not the ones set_open
	// saw). A load in place keeps the identity and starts the revision again, so the load is kept
	// too.
	Serialized &memo = serialized_by_path_[relative];
	const uint64_t identity = open.document->identity(), load = open.document->load_generation(),
	               revision = open.document->revision();
	if (!memo.made || memo.identity != identity || memo.load_generation != load ||
			memo.revision != revision) {
		memo.made = true;
		const SerializeResult result = open.document->serialize();
		memo.identity = identity;
		memo.load_generation = load;
		memo.revision = revision;
		memo.ok = result.ok();
		memo.bytes.assign(result.text.begin(), result.text.end());
	}
	return memo.ok ? &memo : nullptr;
}

bool ProjectAssetSource::read(const std::string &name, std::vector<uint8_t> &out) const {
	const Entry *entry = entry_(name);
	if (!entry) return false;
	const auto open = open_.find(entry->relative);
	if (open != open_.end()) {
		if (const Serialized *serialized = serialized_(open->second, entry->relative)) {
			out = serialized->bytes;
			return true;
		}
	}
	std::string error;
	if (!io::read_file_bytes(join_path(root_, entry->relative), out, error)) return false;
	return vfs_decode_payload(out, scr_policy_);
}

uint64_t ProjectAssetSource::stamp(const std::string &name) const {
	const Entry *entry = entry_(name);
	if (!entry) return 0;
	const auto open = open_.find(entry->relative);
	if (open != open_.end()) {
		const DocumentBase &document = *open->second.document;
		return io::splitmix64(io::splitmix64(io::splitmix64(document.identity()) ^ document.load_generation()) ^ document.revision()) | 1;
	}
	return io::splitmix64(io::splitmix64(entry->size ^ text_hash(entry->relative)) ^ static_cast<uint64_t>(entry->modified)) | 1;
}

std::string ProjectAssetSource::path_of(const std::string &name) const {
	const Entry *entry = entry_(name);
	return entry ? entry->relative : std::string();
}

} // namespace opennova::editor
