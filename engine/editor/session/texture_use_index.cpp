#include <editor/session/texture_use_index.h>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

TextureModelSource TextureUseIndex::models(const SessionView &view) const {
	// A referring model: its open document, else the one read from its file while its stamp stands.
	return [this, &view](const std::string &path) -> std::shared_ptr<const Document> {
		for (const auto &open : view.documents.open)
			if (open && open->path() == path)
				if (const Document *records = records_of(*open))
					return std::shared_ptr<const Document>(open, records);
		const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path) : nullptr;
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
}

const std::vector<TextureUse> &TextureUseIndex::kept(const SessionView &view, const std::string &key,
                                                     const std::function<std::vector<TextureUse>()> &make) const {
	static const std::vector<TextureUse> kNone;
	if (!view.findings.graph || !view.project.scan) return kNone;
	const RevisionKey revisions = revision_key(view.revisions, {ViewConcern::Graph, ViewConcern::Files, ViewConcern::Documents,
	                                                            ViewConcern::DocumentSet});
	Made &made = made_by_file_[key];
	if (made.made && made.key == revisions) return made.uses;
	made.made = true;
	made.key = revisions;
	made.uses = make();
	++made_;
	return made.uses;
}

const std::vector<TextureUse> &TextureUseIndex::uses_of(const SessionView &view, const std::string &file) const {
	return kept(view, file, [&] {
		const AssetGraph &graph = *view.findings.graph;
		return texture_uses(graph, *view.project.scan, file, models(view),
		                    [&graph](const std::string &name) { return graph.has_file(name); });
	});
}

const std::vector<TextureUse> &TextureUseIndex::uses_named(const SessionView &view, const std::string &stem) const {
	return kept(view, "\x01" + strutil::to_lower(stem), [&] {
		const AssetGraph &graph = *view.findings.graph;
		return texture_uses_named(graph, *view.project.scan, stem, models(view),
		                          [&graph](const std::string &name) { return graph.has_file(name); });
	});
}

void TextureUseIndex::clear() {
	made_by_file_.clear();
	models_.clear();
}

} // namespace opennova::editor
