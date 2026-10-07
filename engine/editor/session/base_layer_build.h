#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/install_view.h>
#include <editor/graph/graph_layer.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// The base layer under an expansion project's graph (ADR 0046 d10, S13 D3; wired since T5's follow-up): the
// base game's files as the game serves them under the project (base_install_spec over SessionCore::
// base_game(): the install's base game, or the base game's project's export), each name the graph reads
// read through its extractor (GraphLayerBuilder), so a reference to a name the base game defines (a
// weapon of its weapon.def, a sound profile of its SndProf.def, a texture or model it ships, a model's
// user point) resolves as the game resolves it, through the archives below the expansion's pair [orig:
// PFF_OpenAllArchives @ 0x4a4310, slots 2..4]. Stepped by bytes, so an Open builds it within its polls'
// budget (the whole JO:CA install: 9,290 names, 1,510 read, some 3 s in one call, S13 A3). Nothing for a
// standalone project, or where the base does not mount (the build's gate says so).
class BaseLayerBuild {
public:
	BaseLayerBuild(std::string base, const ProjectDocument &document);

	// One step: the base mounted and listed (the first), then files read until `bytes` of them; true
	// once every file is in (the layer then taken by take()).
	bool step(uint64_t bytes);
	bool done() const { return done_; }
	// The layer built (null for a standalone project, a base that does not mount, or before the end).
	std::shared_ptr<const GraphLayer> take();
	size_t files_done() const { return next_; }
	size_t files_total() const { return total_; }

private:
	bool finish_now();

	std::string base_;
	ProjectDocument document_;
	bool opened_ = false;
	bool done_ = false;
	std::unique_ptr<InstallView> view_;
	std::vector<InstallFile> files_;
	size_t next_ = 0;
	size_t total_ = 0;
	std::unique_ptr<GraphLayerBuilder> builder_;
	std::shared_ptr<const GraphLayer> layer_;
};

// The layer built in one call (BaseLayerBuild stepped to its end): a settings change that moves the base.
std::shared_ptr<const GraphLayer> build_base_layer(const std::string &base, const ProjectDocument &document);

} // namespace opennova::editor
