#include <editor/ui/terrain_viewport_view.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include <imgui.h>

#include <base/io/json.h>
#include <base/io/os_path.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>
#include <editor/ui/workspace.h>
#include <formats/trn/charmap_legend.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

void set_options(Workspace &workspace, const TerrainViewport &model, JsonValue options) {
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Terrain, "options", std::move(options))));
}

void legend(ViewportCanvas &ui, const TerrainViewport &model) {
	const MissionOverlayImage &overlay = model.overlay();
	std::vector<ViewportCanvas::LegendRow> rows;
	for (const MissionOverlayRow &row : overlay.legend) {
		ViewportCanvas::LegendRow line;
		std::copy(std::begin(row.rgb), std::end(row.rgb), line.rgb);
		char share[16] = "";
		if (row.share > 0.0) std::snprintf(share, sizeof(share), " (%.1f%%)", row.share * 100.0);
		line.text = row.key + ": " + ui_kit::fit(row.words, ImGui::GetFontSize() * 30.0f) + share;
		rows.push_back(std::move(line));
	}
	std::string title = overlay.title;
	if (!overlay.words.empty()) title += (title.empty() ? "" : "\n") + overlay.words;
	ui.legend(title, rows);
}

} // namespace

TerrainViewportView::TerrainViewportView() : ViewportView(ViewportKind::Terrain) {}

void TerrainViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const TerrainViewport &>(viewport);
	const TerrainViewportOptions &options = model.options();
	ui_kit::WrapRow row;
	// The mission whose environment, tile set and tiles it is drawn under, or the engine's own environment.
	const TerrainUses &uses = model.uses();
	const TerrainMissionUse *drawn = model.mission();
	const float width = ImGui::GetFontSize() * 11.0f;
	row.next(width);
	ImGui::SetNextItemWidth(width);
	const std::string shown = drawn ? io::utf8_file_name(drawn->mission) : std::string("No mission");
	if (ImGui::BeginCombo("##mission", shown.c_str())) {
		for (const TerrainMissionUse &use : uses.missions) {
			const std::string label = io::utf8_file_name(use.mission) + (use.title.empty() ? "" : " (" + use.title + ")");
			if (ImGui::Selectable(label.c_str(), drawn && use.mission == drawn->mission)) {
				JsonValue change = JsonValue::make_object();
				change.set("mission", io::json_string(use.mission));
				set_options(workspace, model, std::move(change));
			}
		}
		if (ImGui::Selectable("No mission (the engine's own environment)", !drawn)) {
			JsonValue change = JsonValue::make_object();
			change.set("mission", io::json_string(kTerrainNeutral));
			set_options(workspace, model, std::move(change));
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip(drawn ? "Drawn under " + io::utf8_file_name(drawn->mission) + "'s environment (" +
	                                (drawn->environment.empty() ? std::string("none named") : drawn->environment) +
	                                "), with its tile set and its tiles, at its start time, as its load reads them."
	                      : std::string("Drawn under the engine's own environment, as a mission with no .env starts on "
	                                    "it, with the terrain's own tiles."));
	// The layers.
	row.next(ui_kit::button_width("Show"));
	if (ImGui::Button("Show")) ImGui::OpenPopup("show");
	ui_kit::tooltip("What the picture draws over the ground: the terrain's foliage, the water.");
	if (ImGui::BeginPopup("show")) {
		bool foliage = options.foliage, water = options.water;
		const bool changed = ImGui::Checkbox("Foliage", &foliage) | ImGui::Checkbox("Water", &water);
		if (changed) {
			JsonValue show = JsonValue::make_object();
			show.set("foliage", JsonValue::make_bool(foliage));
			show.set("water", JsonValue::make_bool(water));
			JsonValue change = JsonValue::make_object();
			change.set("show", std::move(show));
			set_options(workspace, model, std::move(change));
		}
		ImGui::EndPopup();
	}
	// DI-29: what the game reads at each point of the ground, tinted over the terrain with its legend.
	row.next(ui_kit::button_width("Over the terrain"));
	if (ImGui::Button("Over the terrain")) ImGui::OpenPopup("overlay");
	ui_kit::tooltip("Tint the terrain with what the game reads at each point: its surface classes, or where its foliage "
	                "grows.");
	if (ImGui::BeginPopup("overlay")) {
		const auto overlay = [&](const char *label, MissionGroundOverlay kind, const char *tip) {
			if (ImGui::RadioButton(label, options.overlay == kind)) {
				JsonValue change = JsonValue::make_object();
				change.set("overlay", io::json_string(mission_ground_overlay_token(kind)));
				set_options(workspace, model, std::move(change));
			}
			ui_kit::tooltip(tip);
		};
		overlay("Nothing", MissionGroundOverlay::None, "The terrain as the game draws it.");
		overlay("Surface classes", MissionGroundOverlay::Surfaces,
		        "Each point's surface class as the game reads it (its char map, the placed tiles): what footsteps play "
		        "and what a round's impact plays there. The legend names each class.");
		overlay("Foliage", MissionGroundOverlay::Foliage,
		        "Where the foliage map's codes grow each definition, and what the placed tiles keep off. The legend names "
		        "each definition's model.");
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Frame", true, "The camera on the terrain's sectors (F, or a double click)."))
		workspace.request(request::set_viewport(model.path(), terrain_camera_change(model.framed(context.width, context.height))));
	if (ui_kit::tool(row, "Top", true, "Straight down over the camera's target, north up.")) {
		OrbitCamera camera = model.camera();
		mission_camera_top(camera);
		workspace.request(request::set_viewport(model.path(), terrain_camera_change(camera)));
	}
	// What the picture says of itself: no mission, an environment the project lacks.
	const JsonValue notes = model.notes_json(context.input);
	for (const JsonValue &note : notes.array)
		if (note.get_string("code", "") != "file.missing") ui_kit::clipped_text(note.get_string("message", ""));
	// Under the canvas: the ground under the pointer (DI-07), then a line while the picture lacks a file.
	const float line = ImGui::GetFrameHeightWithSpacing();
	const float under = line + (model.missing().empty() ? 0.0f : line);
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y - under),
	       [&](const CanvasInput &in) {
		       if (options.overlay != MissionGroundOverlay::None) legend(canvas_ui(), model);
		       if (!in.hovered) {
			       asked_.valid = false;
			       ground_.clear();
			       ground_surface_ = -1;
			       return;
		       }
		       const OrbitCamera &camera = model.camera();
		       const float view[6] = { camera.target.x, camera.target.y, camera.target.z, camera.yaw, camera.pitch, camera.distance };
		       const bool same = asked_.valid && asked_.x == in.mouse.x && asked_.y == in.mouse.y &&
		                         asked_.width == context.width && asked_.height == context.height &&
		                         std::equal(std::begin(view), std::end(view), std::begin(asked_.camera)) &&
		                         asked_.surface == model.surface() && asked_.reads == uint64_t(model.ground().reads());
		       if (same) return;
		       asked_.valid = true;
		       asked_.x = in.mouse.x;
		       asked_.y = in.mouse.y;
		       asked_.width = context.width;
		       asked_.height = context.height;
		       std::copy(std::begin(view), std::end(view), std::begin(asked_.camera));
		       asked_.surface = model.surface();
		       asked_.reads = uint64_t(model.ground().reads());
		       const MissionGroundFacts facts = model.ground_under(context, in.mouse.x, in.mouse.y);
		       ground_ = mission_ground_line(facts);
		       ground_surface_ = facts.on == MissionGroundOn::Terrain && !facts.under_water ? facts.surface : -1;
	       });
	if (ground_.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text("Ground: point at the terrain to read its surface class, the footsteps a body plays there, the "
		                     "row a round plays and the foliage it grows.");
		ImGui::PopStyleColor();
	} else {
		// The class's colour in the char map legend (formats/trn/charmap_legend.h).
		if (ground_surface_ >= 0 && ground_surface_ < kCharmapLegendCount) {
			const CharmapLegendColour &c = kCharmapLegend[ground_surface_];
			const float side = ImGui::GetTextLineHeight();
			ImGui::ColorButton("##ground_class", ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f),
			                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
			                   ImVec2(side, side));
			ImGui::SameLine();
		}
		ui_kit::clipped_text("Ground: " + ground_);
	}
	if (!model.missing().empty()) {
		const std::vector<std::string> &missing = model.missing();
		std::string text = std::to_string(missing.size()) + (missing.size() == 1 ? " file missing: " : " files missing: ");
		std::string all;
		for (size_t i = 0; i < missing.size(); ++i) {
			if (i < 3) text += (i ? ", " : "") + missing[i];
			all += (i ? "\n" : "") + missing[i];
		}
		if (missing.size() > 3) text += ", ...";
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ui_kit::clipped_text(text, all + "\nImport them to see them (Problems).");
		ImGui::PopStyleColor();
	}
}

} // namespace opennova::editor
