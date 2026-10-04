#include <editor/session/texture_show_use.h>

#include <memory>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_edge.h>
#include <editor/graph/texture_uses.h>
#include <editor/model/finding_code_row.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

// The mission whose header names `file` (a terrain or an environment) as its own: one open first, else the
// first the graph's referrers list (the scan's order); "" for none.
std::string mission_drawing(const SessionView &view, const std::string &file) {
	std::string first;
	for (const GraphEdge *edge : view.findings.graph->referrers_of_file(file)) {
		if (edge->kind != ReferenceKind::Terrain && edge->kind != ReferenceKind::Environment) continue;
		const AssetEntry *source = view.project.scan->at_path(edge->source);
		if (!source || source->kind != AssetKind::Mission) continue;
		for (const std::shared_ptr<const DocumentBase> &open : view.documents.open)
			if (open && open->path() == edge->source) return edge->source;
		if (first.empty()) first = edge->source;
	}
	return first;
}

} // namespace

bool texture_use_place(const SessionView &view, const std::string &texture, const TextureUse &use, int index,
                       UsePlace &out, std::string &why) {
	out = UsePlace();
	out.use = index;
	if (use.fixed || use.referrer.empty()) {
		why = "The game opens " + basename_of(texture) + " by its name itself (" + use.fixed_for +
		      "): its view shows it as that use draws it (As used).";
		return false;
	}
	const AssetEntry *referrer = view.project.scan ? view.project.scan->at_path(use.referrer) : nullptr;
	const AssetKind kind = referrer ? referrer->kind : AssetKind::Unknown;
	if (kind == AssetKind::Model) {
		out.picture = UsePicture::Model;
		out.open = use.referrer;
		out.label = "Show on " + basename_of(use.referrer);
		return true;
	}
	if (kind == AssetKind::Menu) {
		out.picture = UsePicture::Menu;
		out.open = use.referrer;
		out.label = "Show in " + basename_of(use.referrer);
		return true;
	}
	if (kind == AssetKind::Terrain || kind == AssetKind::Environment) {
		const std::string mission = view.findings.graph ? mission_drawing(view, use.referrer) : std::string();
		if (mission.empty()) {
			why = "No mission of the project names " + basename_of(use.referrer) +
			      " as its own, and a mission's view is what draws it: name it in a mission's header first.";
			return false;
		}
		out.picture = UsePicture::Mission;
		out.open = mission;
		out.label = (kind == AssetKind::Terrain ? "Show on " : "Show with ") + basename_of(use.referrer) + " in " +
		            basename_of(mission);
		return true;
	}
	// A definition's, the HUD layout's, a particle's, a mission's loading screen: no picture of the referrer
	// draws it here, so the texture's own shows it as the use's loader makes it.
	out.picture = UsePicture::AsUsed;
	out.open = texture;
	out.label = "Show as " + basename_of(use.referrer) + " draws it";
	return true;
}

void show_texture_use(SessionCore &core, const EditorRequest &request) {
	SessionView &view = core.view();
	if (!view.project.open) return;
	const auto refuse = [&](const std::string &message) { core.refuse_now(CoreFinding::TextureShowUse, message, request.path); };
	if (!view.findings.graph || !view.documents.texture_uses || !view.project.scan)
		return refuse("The project's references are not read yet: show the use once they are.");
	if (request.paths.size() != 1) return refuse("A use is shown by the one file that uses the texture.");
	const AssetEntry *entry = view.project.scan->at_path(request.path);
	if (!entry) entry = view.project.scan->find(basename_of(request.path));
	if (!entry) return refuse("The project has no file " + request.path + ".");
	const std::string texture = entry->relative_path;
	const std::string &referrer = request.paths.front();
	// The use, copied: opening a document moves what the uses were made from.
	TextureUse use;
	int index = -1;
	const std::vector<TextureUse> &uses = view.documents.texture_uses->uses_of(view, texture);
	for (size_t i = 0; i < uses.size() && index < 0; ++i) {
		const TextureUse &each = uses[i];
		if (each.fixed || each.referrer != referrer) continue;
		if (!request.locator.empty() && each.locator != request.locator) continue;
		if (!request.field.empty() && each.field != request.field) continue;
		use = each;
		index = int(i);
	}
	if (index < 0) return refuse(referrer + " does not use " + basename_of(texture) + " there.");
	UsePlace place;
	std::string why;
	if (!texture_use_place(view, texture, use, index, place, why)) return refuse(why);
	DocumentSet &documents = core.documents();
	switch (place.picture) {
	case UsePicture::Model:
	case UsePicture::Menu: {
		documents.open_document(request::open_document(place.open, use.locator, use.field));
		if (!documents.document_for(place.open)) return; // it did not read: the open said why
		ViewEvent reveal;
		reveal.kind = ViewEventKind::RevealPreview;
		reveal.path = place.open;
		view.events.post(std::move(reveal));
		core.touch(ViewConcern::Selection);
		break;
	}
	case UsePicture::Mission:
		documents.open_document(request::open_document(place.open));
		if (!documents.document_for(place.open)) return;
		break;
	case UsePicture::AsUsed:
		documents.open_document(request::open_document(place.open));
		if (!documents.document_for(place.open)) return;
		core.set_viewport(place.open, "{\"kind\":\"texture\",\"options\":{\"as_used\":" + std::to_string(index) + "}}");
		break;
	}
	view.activity.status = "Showing " + basename_of(texture) + " " +
	                       (place.picture == UsePicture::Model     ? "on " + basename_of(place.open)
	                        : place.picture == UsePicture::Menu    ? "in " + basename_of(place.open)
	                        : place.picture == UsePicture::Mission ? "in " + basename_of(place.open) + "'s view"
	                                                               : "as " + basename_of(referrer) + " draws it") +
	                       ".";
	core.touch(ViewConcern::Output);
}

} // namespace opennova::editor
