#include <editor/session/base_layer_build.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include <editor/assets/asset_type_registry.h>

namespace opennova::editor {

BaseLayerBuild::BaseLayerBuild(std::string base, const ProjectDocument &document) :
		base_(std::move(base)), document_(document) {}

bool BaseLayerBuild::step(uint64_t bytes) {
	if (done_) return true;
	if (!opened_) {
		opened_ = true;
		if (document_.expansion.standalone() || base_.empty()) return done_ = true;
		view_ = std::make_unique<InstallView>();
		std::string error;
		if (!view_->open(base_install_spec(base_, document_), error)) {
			view_.reset();
			return done_ = true;
		}
		files_ = view_->files();
		total_ = files_.size();
		builder_ = std::make_unique<GraphLayerBuilder>(document_.target_game);
		return files_.empty() ? finish_now() : false;
	}
	uint64_t read = 0;
	while (next_ < files_.size() && (read < bytes || read == 0)) {
		const InstallFile file = files_[next_++];
		LayerFile layer;
		layer.name = file.name;
		const InstallView *view = view_.get();
		uint64_t peeked = 0;
		if (asset_classification_needs_bytes(file.name)) {
			// A `.bin` is what its content makes it (classify_asset: RTXT a string table, SCR0 a music script, else
			// raw), as the project scan types the project's own: by its name alone the base's menutxt.bin and
			// gametext.bin would be raw tables, so an expansion's menu naming menutxt.BIN would read as naming a
			// file of another kind and its string ids as defined nowhere, where the game reads the base's table
			// through the archives below the expansion's pair [orig: CUIStringTable_LookupString @ 0x6527c0;
			// PFF_OpenAllArchives @ 0x4a4310, slots 2..4]. Read once: the extractor takes the same bytes.
			auto content = std::make_shared<std::vector<uint8_t>>();
			const bool got = view->read(file, *content);
			layer.kind = classify_asset(file.name, got ? content.get() : nullptr);
			layer.read = [content, got](std::vector<uint8_t> &out, std::string &) {
				out = *content;
				return got;
			};
			peeked = content->size();
		} else {
			layer.kind = classify_asset(file.name, nullptr);
			layer.read = [view, file](std::vector<uint8_t> &out, std::string &) { return view->read(file, out); };
		}
		// A name-only file counts a byte, so a budget ends a step.
		read += std::max(builder_->add(layer), peeked) + 1;
	}
	return next_ < files_.size() ? false : finish_now();
}

bool BaseLayerBuild::finish_now() {
	layer_ = builder_ ? builder_->finish() : nullptr;
	builder_.reset();
	view_.reset();
	files_.clear();
	return done_ = true;
}

std::shared_ptr<const GraphLayer> BaseLayerBuild::take() { return std::move(layer_); }

std::shared_ptr<const GraphLayer> build_base_layer(const std::string &base, const ProjectDocument &document) {
	BaseLayerBuild build(base, document);
	while (!build.step(std::numeric_limits<uint64_t>::max())) {
	}
	return build.take();
}

} // namespace opennova::editor
