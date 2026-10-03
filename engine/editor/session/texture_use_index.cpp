#include <editor/session/texture_use_index.h>

#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

const std::vector<TextureUse> &TextureUseIndex::uses_of(const SessionView &view, const std::string &file) const {
	static const std::vector<TextureUse> kNone;
	if (!view.findings.graph || !view.project.scan) return kNone;
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::Graph, ViewConcern::Files, ViewConcern::Documents,
	                                                      ViewConcern::DocumentSet});
	Made &made = made_by_file_[file];
	if (made.made && made.key == key) return made.uses;
	made.made = true;
	made.key = key;
	const AssetScan &scan = *view.project.scan;
	// A referring model: its open document, else the one read from its file while its stamp stands.
	const TextureModelSource models = [&](const std::string &path) -> std::shared_ptr<const Document> {
		for (const auto &open : view.documents.open)
			if (open && open->path() == path)
				if (const Document *records = records_of(*open))
					return std::shared_ptr<const Document>(open, records);
		const AssetEntry *entry = scan.at_path(path);
		if (!entry || !reader_) return nullptr;
		Model &model = models_[path];
		if (!model.document || model.size != entry->size_bytes || model.modified != entry->modified_ticks) {
			const std::shared_ptr<DocumentBase> read = reader_(path);
			const Document *records = read ? records_of(*read) : nullptr;
			model.document = records ? std::shared_ptr<const Document>(read, records) : nullptr;
			model.size = entry->size_bytes;
			model.modified = entry->modified_ticks;
		}
		return model.document;
	};
	const AssetGraph &graph = *view.findings.graph;
	made.uses = texture_uses(graph, scan, file, models, [&graph](const std::string &name) { return graph.has_file(name); });
	++made_;
	return made.uses;
}

void TextureUseIndex::clear() {
	made_by_file_.clear();
	models_.clear();
}

} // namespace opennova::editor
