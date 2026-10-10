#include <editor/session/texture_show_use.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/model_document.h>
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
#include <base/io/strutil.h>
#include <formats/til/til_io.h>
#include <runtime/renderer/material_eval.h>

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

// A field's whole number.
int64_t whole(const Value &value) {
	if (const int64_t *i = std::get_if<int64_t>(&value)) return *i;
	if (const double *d = std::get_if<double>(&value)) return int64_t(*d);
	return 0;
}

// The frame a flipbook shows at `time_ms` with every register at `ctrl`: the engine's frame law.
int frame_at(const threedi::ThreediMaterial &material, const std::vector<std::string> &names, uint32_t time_ms,
		int32_t ctrl) {
	renderer::ControlRegisterValues values{};
	values.fill(ctrl);
	return renderer::compute_anim_frame(material, 0, time_ms, names, values);
}

} // namespace

bool flipbook_frame_change(const threedi::ThreediMaterial &material,
		const std::vector<threedi::ThreediControlRegister> &registers, int frame, FlipbookFrameChange &out,
		std::string &why) {
	out = FlipbookFrameChange();
	const int frames = int(material.animation.num_frames);
	if (frames <= 1 || frame < 0 || frame >= frames) {
		why = "Frame " + std::to_string(frame) + " is past the flipbook's " + std::to_string(frames) +
		      " frames: the game never shows it.";
		return false;
	}
	std::vector<std::string> names;
	for (const threedi::ThreediControlRegister &each : registers)
		names.push_back(strutil::fixed_string(each.name, sizeof(each.name)));
	if (material.animation.animation_type == 0) {
		// The clock's first millisecond on the frame (a frame time of 0 is 1, the loader's [orig:
		// Material_ConvertDefinition @ 0x5B06F6..0x5B070A]).
		const uint32_t step = uint16_t(material.animation.cycle_frame_time) == 0 ? 1u : uint16_t(material.animation.cycle_frame_time);
		const uint32_t time = uint32_t(frame) * step;
		if (frame_at(material, names, time, 0) != frame) {
			why = "The flipbook's clock never shows frame " + std::to_string(frame) + ".";
			return false;
		}
		out.clock = true;
		out.change = "{\"clock\":{\"playing\":false,\"time_ms\":" + std::to_string(time) + "}}";
		out.words = "the clock held at " + std::to_string(time) + " ms";
		return true;
	}
	if (material.animation.animation_type == 1) {
		const int index = int(material.animation.cycle_frame_time);
		if (index < 0 || size_t(index) >= names.size() || names[size_t(index)].empty()) {
			why = "The flipbook reads register " + std::to_string(index) + ", which the model does not name.";
			return false;
		}
		// The least value the frame law shows the frame at: a texture selector's the frame itself, a 16.16
		// fraction's the first past the frames before it.
		const int32_t fraction = int32_t((int64_t(frame) * 65536 + frames - 1) / frames);
		for (const int32_t value : { int32_t(frame), fraction }) {
			if (frame_at(material, names, 0, value) != frame) continue;
			out.clock = false;
			out.change = "{\"kind\":\"model\",\"options\":{\"ctrl\":{\"" + names[size_t(index)] + "\":" +
			             std::to_string(value) + "}}}";
			out.words = names[size_t(index)] + " held at " + std::to_string(value);
			return true;
		}
		why = "No value of " + names[size_t(index)] + " shows frame " + std::to_string(frame) + ".";
		return false;
	}
	why = "The flipbook's clock (type " + std::to_string(material.animation.animation_type) +
	      ") holds frame 0: the game never shows frame " + std::to_string(frame) + ".";
	return false;
}

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
		// No referrer named: a name the game opens itself (texture_use_place says where it shows).
		if (each.fixed != referrer.empty() || each.referrer != referrer) continue;
		// A use in a native text has no locator: its record tells it from another of the same field (two
		// particles' graphic1).
		if (!request.locator.empty() && (each.locator.empty() ? each.record : each.locator) != request.locator) continue;
		if (!request.field.empty() && each.field != request.field) continue;
		use = each;
		index = int(i);
	}
	if (index < 0) return refuse(referrer + " does not use " + basename_of(texture) + " there.");
	UsePlace place;
	std::string why;
	if (!texture_use_place(view, texture, use, index, place, why)) return refuse(why);
	DocumentSet &documents = core.documents();
	std::string flipbook; // a flipbook frame's words, where the use is one
	switch (place.picture) {
	case UsePicture::Model:
	case UsePicture::Menu: {
		documents.open_document(request::open_document(place.open, use.locator, use.field));
		const DocumentBase *opened = documents.document_for(place.open);
		if (!opened) return; // it did not read: the open said why
		ViewEvent reveal;
		reveal.kind = ViewEventKind::RevealPreview;
		reveal.path = place.open;
		view.events.post(std::move(reveal));
		core.touch(ViewConcern::Selection);
		// A flipbook's frame shown at its frame (S23 C): its row's frame, its material's clock.
		const ModelDocument *model = dynamic_cast<const ModelDocument *>(opened);
		const ModelRow *row = model ? model->model_row() : nullptr;
		if (place.picture == UsePicture::Model && use.role == renderer::TextureRoleId::ModelFlipFrame && row &&
				use.context.material >= 0 && size_t(use.context.material) < row->materials.size()) {
			const NodeAddress at = model->address_at(use.locator);
			Value value;
			const int frame = at.row && model->get(at, "frame", value) ? int(whole(value)) : -1;
			FlipbookFrameChange step;
			std::string why;
			if (flipbook_frame_change(row->materials[size_t(use.context.material)].material, row->registers, frame, step, why)) {
				core.set_viewport(step.clock ? std::string() : place.open, step.change);
				flipbook = ", frame " + std::to_string(frame) + " (" + step.words + ")";
			} else {
				flipbook = ": " + why;
			}
		}
		break;
	}
	case UsePicture::Mission:
		documents.open_document(request::open_document(place.open));
		if (!documents.document_for(place.open)) return;
		// A tile atlas's cells framed (S23 C): the squares the mission's .til places them on, as the mission's
		// ground draws them (each 16 units, at x, -z of its entry).
		if (use.role == renderer::TextureRoleId::TerrainTileAtlas && view.findings.assets) {
			std::vector<uint8_t> bytes;
			TilFile placed;
			std::string error;
			const std::string name = basename_of(place.open);
			const std::string til = name.substr(0, name.find_last_of('.')) + ".til";
			if (view.findings.assets->read(til, bytes) && load_til(bytes.data(), bytes.size(), placed, error) &&
					!placed.entries.empty()) {
				double box[4] = { 1e30, 1e30, -1e30, -1e30 };
				for (const TilOverlayEntry &entry : placed.entries) {
					const double x = double(entry.x_fixed) / 65536.0, y = -double(entry.z_fixed) / 65536.0;
					box[0] = std::min(box[0], x);
					box[1] = std::min(box[1], y);
					box[2] = std::max(box[2], x + TIL_CELL_WORLD_UNITS);
					box[3] = std::max(box[3], y + TIL_CELL_WORLD_UNITS);
				}
				char change[192];
				std::snprintf(change, sizeof(change), "{\"kind\":\"mission\",\"frame_ground\":[%.3f,%.3f,%.3f,%.3f]}",
						box[0], box[1], box[2], box[3]);
				core.set_viewport(place.open, change);
				flipbook = ", framed on the " + std::to_string(placed.entries.size()) + " squares " + til + " places its cells on";
			} else {
				flipbook = ": " + til + " places none of its cells";
			}
		}
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
	                       flipbook + ".";
	core.touch(ViewConcern::Output);
}

} // namespace opennova::editor
