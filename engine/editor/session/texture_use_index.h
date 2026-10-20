#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/graph/texture_uses.h>
#include <editor/session/view/view_revisions.h>

namespace opennova::editor {

class DocumentBase;
struct SessionView;

// What uses each texture of the project (ADR 0046 S18, graph/texture_uses), kept for the windows and the
// wire while what it reads stands (the graph, the files, the open documents: RevisionKey): a texture's
// uses made once, then answered again until one of those moves. A model's row reads its document (open,
// else read from its file, kept while the scan's stamp of it stands) for its material's words.
class TextureUseIndex {
public:
	// A closed model's document read from its file (DocumentSet::load: null when it does not read).
	using ModelReader = std::function<std::shared_ptr<DocumentBase>(const std::string &path)>;
	explicit TextureUseIndex(ModelReader reader) : reader_(std::move(reader)) {}

	// The uses of the project file `file` as the view stands (none without a graph).
	const std::vector<TextureUse> &uses_of(const SessionView &view, const std::string &file) const;
	// The uses of every name whose stem is `stem`, whatever file they find (texture_uses_named: what an
	// import of that stem is asked for), kept alike.
	const std::vector<TextureUse> &uses_named(const SessionView &view, const std::string &stem) const;
	// How many times a texture's uses were made (a test counts what the key saves).
	uint64_t made() const { return made_; }
	void clear();

private:
	struct Made {
		bool made = false;
		RevisionKey key;
		std::vector<TextureUse> uses;
	};
	struct Model {
		uint64_t size = 0;
		int64_t modified = 0;
		std::shared_ptr<const Document> document;
	};
	// The model source over the view: an open model's document, else one read from its file.
	TextureModelSource models(const SessionView &view) const;
	// The uses kept under `key`, made by `make` when what they read moved.
	const std::vector<TextureUse> &kept(const SessionView &view, const std::string &key,
	                                    const std::function<std::vector<TextureUse>()> &make) const;

	// The graph's texture edges by name (texture_edge_names), made again when the graph or what the uses read
	// moves: what every file's and stem's uses are found through.
	const TextureEdgeNames &edge_names(const SessionView &view) const;

	ModelReader reader_;
	mutable TextureEdgeNames names_;
	mutable RevisionKey names_key_;
	mutable const AssetGraph *names_graph_ = nullptr;
	mutable uint64_t names_generation_ = 0;
	mutable bool names_made_ = false;
	mutable std::map<std::string, Made> made_by_file_; // by a file's path, or "\x01" and a stem
	mutable std::map<std::string, Model> models_;
	mutable uint64_t made_ = 0;
};

} // namespace opennova::editor
