#include "native_asset_catalog.h"

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/project/project_manifest.h>
#include <base/title/title.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_view.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_index.h>
#include <editor/project/project_document.h>

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace opennova::editor {
namespace {

struct ExportFile {
	const InstallView *view = nullptr;
	const InstallFile *file = nullptr;
	bool own = false;
	AssetKind kind = AssetKind::Unknown;
};

// Each name has exactly one winner, the project's before the base's. Keeping every
// winner in one graph (rather than GraphLayer, which drops edges) follows references
// out of inherited files in the same context as references out of an own file.
class ExportFiles final : public FileSource {
public:
	bool open(const NativeAssetCatalogOptions &options, std::string &error) {
		InstallSpec own;
		own.root = options.project_export;
		own.game = options.game;
		own.expansion = own.project_expansion = options.expansion;
		if (!own_.open(own, error)) return false;
		if (!options.base_export.empty()) {
			InstallSpec base;
			base.root = options.base_export;
			base.game = options.game;
			if (!base_.open(base, error)) return false;
			add(base_, false, false);
		}
		add(own_, true, !options.expansion.empty());
		for (auto &[key, entry] : files) {
			std::string why;
			const std::string &name = entry.file->name;
			if (name.find('/') != std::string::npos || !project::is_project_path(name, why)) {
				error = "the export has an unsafe native file name: " + name;
				return false;
			}
			entry.kind = file_kind_for_name(name);
			if (entry.kind == AssetKind::Unknown || entry.kind == AssetKind::RawBin) {
				std::vector<uint8_t> bytes;
				if (!read(name, bytes)) {
					error = "cannot read " + name + " from the export";
					return false;
				}
				entry.kind = file_kind_for_file(name, &bytes);
			}
		}
		return true;
	}

	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const ExportFile *entry = find(name);
		return entry && entry->view->read(*entry->file, out);
	}
	uint64_t stamp(const std::string &name) const override { return find(name) ? 1 : 0; }
	bool stored(const std::string &name, std::vector<uint8_t> &out, std::string &error) const {
		const ExportFile *entry = find(name);
		if (!entry) return false;
		if (!entry->file->loose_path.empty())
			return io::read_file_bytes(entry->file->loose_path, out, error);
		if (entry->view->vfs().read_file_raw(entry->file->member, out)) return true;
		error = "cannot read " + name + " from the export";
		return false;
	}
	const ExportFile *find(const std::string &name) const {
		const auto at = files.find(pff::normalized_logical_name(name));
		return at == files.end() ? nullptr : &at->second;
	}
	std::map<std::string, ExportFile> files;

private:
	void add(const InstallView &view, bool own, bool expansion_only) {
		for (const InstallFile &file : view.files()) {
			if (expansion_only && file.layer != InstallFile::Layer::Expansion) continue;
			files[pff::normalized_logical_name(file.name)] = { &view, &file, own };
		}
	}
	InstallView own_;
	InstallView base_;
};

bool catalog_root(AssetKind kind) {
	return kind == AssetKind::Model || kind == AssetKind::Animation ||
			kind == AssetKind::AnimationMap || kind == AssetKind::Terrain || kind == AssetKind::Texture;
}

// Validate native leaves too: animations and textures need no graph extraction, but a
// file that does not parse is no usable download. Results are shared by every closure.
bool usable(const ExportFiles &files, const GraphSlot &slot, const std::string &game,
		std::map<std::string, std::string> &checked, std::string &error) {
	const auto found = checked.find(slot.path);
	if (found != checked.end()) {
		error = found->second;
		return error.empty();
	}
	std::string why;
	if (!slot.ok) why = "cannot read references in " + slot.path;
	else if (asset_kind_row(slot.kind).names_files && !graph_reads_kind(slot.kind))
		why = "references in " + slot.path + " are unsupported";
	else if (const DocumentType *type = document_type_for(slot.kind)) {
		std::vector<uint8_t> bytes;
		Diagnostic diagnostic;
		auto document = type->make();
		if (!files.read(slot.path, bytes) ||
				!document->load_bytes(bytes, slot.path, slot.kind, game, diagnostic))
			why = "cannot parse " + slot.path;
		else if (type->validate_file) {
			for (const Diagnostic &finding : type->validate_file(*document))
				if (finding.severity == DiagnosticSeverity::Error) {
					why = "invalid " + slot.path + ": " + finding.message;
					break;
				}
		}
	}
	checked.emplace(slot.path, why);
	error = std::move(why);
	return error.empty();
}

bool closure(const AssetGraph &graph, const ExportFiles &files, const std::string &root,
		const std::string &game, std::map<std::string, std::string> &checked,
		std::set<std::string> &out, std::string &error) {
	std::vector<std::string> pending{ root };
	for (size_t next = 0; next < pending.size(); ++next) {
		const std::string name = pending[next];
		if (!out.insert(name).second) continue;
		const uint32_t id = graph.index().find(name);
		if (id == GraphIndex::kNone) {
			error = "no native file for " + name;
			return false;
		}
		const GraphSlot &slot = graph.index().slot(id);
		if (!usable(files, slot, game, checked, error)) return false;
		for (const GraphEdge &edge : slot.edges) {
			std::string target;
			const ReferenceStatus status = graph.resolve(edge, &target);
			if (status == ReferenceStatus::NotAReference) continue;
			if (status != ReferenceStatus::Present || target.empty()) {
				if (edge.optional && status == ReferenceStatus::Missing) continue;
				error = name + " has an unresolved dependency: " + edge.value;
				return false;
			}
			pending.push_back(std::move(target));
		}
	}
	return true;
}

} // namespace

bool write_native_asset_catalog(const NativeAssetCatalogOptions &options,
		NativeAssetCatalogResult &result, std::string &error) {
	result = {};
	if (!options.base_project.empty()) {
		if (!options.base_export.empty()) {
			error = "choose a base export or a base project";
			return false;
		}
		const ProjectPaths paths = ProjectPaths::for_root(options.base_project);
		ProjectDocument document;
		Diagnostic diagnostic;
		if (!load_project_document(paths.project_file, document, diagnostic)) {
			error = diagnostic.message;
			return false;
		}
		if (!document.expansion.standalone() || document.target_game != options.game) {
			error = "the base project must be a standalone project of the same game";
			return false;
		}
		NativeAssetCatalogOptions resolved = options;
		resolved.base_project.clear();
		resolved.base_export = paths.export_dir(document);
		return write_native_asset_catalog(resolved, result, error);
	}
	if (!title_by_code(options.game)) {
		error = "unknown game: " + options.game;
		return false;
	}
	if (options.output.empty() || options.project_export.empty() ||
			(options.expansion.empty() && !options.base_export.empty())) {
		error = "an output and project export are required; only an expansion takes a base export";
		return false;
	}
	std::error_code ec;
	const auto output = io::os_path(options.output);
	if (std::filesystem::exists(output, ec) && !std::filesystem::is_empty(output, ec)) {
		error = "the catalog output must be empty: " + options.output;
		return false;
	}
	if (ec) { error = ec.message(); return false; }
	auto files = std::make_shared<ExportFiles>();
	if (!files->open(options, error)) return false;
	AssetScan scan;
	for (const auto &[key, entry] : files->files) {
		AssetEntry asset;
		asset.logical_name = asset.relative_path = entry.file->name;
		asset.kind = entry.kind;
		asset.size_bytes = entry.view->size(*entry.file);
		scan.entries.push_back(std::move(asset));
	}
	scan.index();
	ProjectPaths paths;
	paths.files = files;
	ProjectDocument project;
	project.target_game = options.game;
	project.expansion.name = options.expansion;
	AssetGraph graph;
	graph.update(paths, project, scan, {});
	std::map<std::string, std::string> checked;
	std::map<std::string, io::JsonValue> recorded;
	std::set<std::string> written;
	io::JsonValue assets = io::JsonValue::make_array();
	const std::string blobs = io::utf8_join(options.output, "blobs");
	if (!io::ensure_directory(blobs, error)) return false;
	for (const auto &[key, entry] : files->files) {
		if (!entry.own || !catalog_root(entry.kind)) continue;
		const std::string &name = entry.file->name;
		std::set<std::string> selected;
		std::string why;
		if (!closure(graph, *files, name, options.game, checked, selected, why)) {
			result.omitted.push_back(name + ": " + why);
			continue;
		}
		io::JsonValue members = io::JsonValue::make_array();
		for (const std::string &member : selected) {
			auto found = recorded.find(member);
			if (found == recorded.end()) {
				std::vector<uint8_t> bytes;
				if (!files->stored(member, bytes, error)) return false;
				const std::string sha = io::sha256_hex(bytes.data(), bytes.size());
				if (written.insert(sha).second &&
						!io::write_file_atomic(io::utf8_join(blobs, sha), bytes.data(), bytes.size(), error)) return false;
				io::JsonValue file = io::JsonValue::make_object();
				file.set("name", io::json_string(member));
				file.set("sha256", io::json_string(sha));
				file.set("size", io::json_number(static_cast<double>(bytes.size())));
				file.set("origin", io::json_string(files->find(member)->own ? "project" : "base"));
				found = recorded.emplace(member, std::move(file)).first;
			}
			members.push(found->second);
		}
		const io::JsonValue &root = recorded.at(name);
		io::JsonValue asset = io::JsonValue::make_object();
		asset.set("name", *root.get("name"));
		asset.set("sha256", *root.get("sha256"));
		asset.set("size", *root.get("size"));
		asset.set("kind", io::json_string(asset_kind_token(entry.kind)));
		asset.set("files", std::move(members));
		assets.push(std::move(asset));
		++result.assets;
	}
	io::JsonValue manifest = io::JsonValue::make_object();
	manifest.set("schema_version", io::json_number(1));
	manifest.set("assets", std::move(assets));
	return io::write_file_atomic(io::utf8_join(options.output, "manifest.json"), io::json_write(manifest), error);
}

} // namespace opennova::editor
