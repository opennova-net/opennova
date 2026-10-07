#include <editor/session/base_layer_build.h>

#include <limits>
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
		layer.kind = classify_asset(file.name, nullptr);
		const InstallView *view = view_.get();
		layer.read = [view, file](std::vector<uint8_t> &out, std::string &) { return view->read(file, out); };
		read += builder_->add(layer) + 1; // a name-only file counts a byte, so a budget ends a step
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
