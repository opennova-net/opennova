#include <editor/ui/terrain_pane_parts.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/til_document.h>
#include <editor/model/document_base.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/texture_thumbnail_images.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/preview/tile_tools.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>
#include <formats/foliage/foliage.h>
#include <formats/trn/charmap_legend.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The open document of the tile placement at `path` (null: not open).
const TilDocument *open_tiles(const SessionView &view, const std::string &path) {
	for (const auto &open : view.documents.open)
		if (open->path() == path) return dynamic_cast<const TilDocument *>(open.get());
	return nullptr;
}

// A command of the tile tools over the view's selected tiles (the view's edit_in_viewport, as its keys raise them).
void tile_command(Workspace &workspace, const std::string &view, const char *name, std::vector<double> by,
                  const char *value = "") {
	ViewportCommand command;
	command.name = name;
	command.by = std::move(by);
	command.value = value;
	workspace.request(request::edit_in_viewport(view, command));
}

// The tile set's cells as the game cuts them, each its picture and how many of the placement's tiles draw it; a
// click takes the cell into the brush (Stamp's, as Nile's palette picks the tile its Insert stamps).
void draw_palette(TerrainPaneContext &context, const TilDocument *placement) {
	Workspace &workspace = context.workspace;
	const SessionView &view = workspace.view();
	const TerrainFamily &family = context.family;
	if (family.tile_set.empty()) {
		ImGui::TextDisabled("The terrain names no tile set.");
		return;
	}
	const AssetEntry *entry = view.project.scan ? view.project.scan->find(family.tile_set) : nullptr;
	if (!entry) {
		ImGui::TextWrapped("The tile set %s is not in the project or its base: its tiles draw as the game's missing "
		                   "texture.",
		                   family.tile_set.c_str());
		return;
	}
	const std::shared_ptr<const TextureThumbnail> picture =
	        view.documents.thumbnails ? view.documents.thumbnails->get(view, entry->relative_path, TextureLoadTransform::None)
	                                  : nullptr;
	ImGui::Text("Tile set %s", basename_of(entry->relative_path).c_str());
	if (!picture) {
		ImGui::TextDisabled("Reading the tile set...");
		return;
	}
	if (picture->state != TextureThumbnail::State::Ready || !picture->source_width || !picture->source_height) {
		ImGui::TextWrapped("%s", picture->refusal.empty() ? "The tile set does not read." : picture->refusal.c_str());
		return;
	}
	const TilFile file = placement ? placement->file() : TilFile();
	const std::vector<TileCell> cells =
	        tile_palette(int(picture->source_width), int(picture->source_height), placement ? &file : nullptr);
	if (cells.empty()) {
		ImGui::TextWrapped("The tile set is %u texels high: the game cuts no row of cells from a set under 64 (D-TIL-8).",
		                   picture->source_height);
		return;
	}
	TextureThumbnailImages *images = workspace.thumbnail_images();
	const uint64_t texture = images ? images->texture_id(*picture) : 0;
	const float side = ImGui::GetFontSize() * 2.5f, gap = ImGui::GetStyle().ItemSpacing.x;
	const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (side + gap)));
	ImDrawList &paint = *ImGui::GetWindowDrawList();
	const float w = float(picture->source_width), h = float(picture->source_height);
	for (size_t i = 0; i < cells.size(); ++i) {
		const TileCell &cell = cells[i];
		if (i % size_t(columns) != 0) ImGui::SameLine(0.0f, gap);
		ImGui::PushID(int(i));
		const bool pressed = ImGui::InvisibleButton("cell", ImVec2(side, side));
		const ImVec2 from = ImGui::GetItemRectMin(), to = ImGui::GetItemRectMax();
		if (texture)
			paint.AddImage(ImTextureID(texture), from, to, ImVec2(float(cell.x) / w, float(cell.y) / h),
			               ImVec2(float(cell.x + cell.w) / w, float(cell.y + cell.h) / h));
		else
			paint.AddRectFilled(from, to,
			                    IM_COL32(picture->average[0], picture->average[1], picture->average[2], picture->average[3]));
		const bool chosen = context.brush.cell == cell.index;
		paint.AddRect(from, to, ImGui::GetColorU32(chosen ? ImGuiCol_ButtonActive : ImGuiCol_Border), 0.0f, 0,
		              chosen ? 3.0f : 1.0f);
		std::string tip = "Cell " + std::to_string(cell.index) + ": texels " + std::to_string(cell.x) + ", " +
		                  std::to_string(cell.y) + ", " + std::to_string(cell.w) + " x " + std::to_string(cell.h);
		if (placement) tip += "; " + std::to_string(cell.uses) + " of the placement's tiles draw it";
		ui_kit::tooltip(tip + ". Click to stamp it.");
		if (pressed) {
			io::JsonValue members = io::JsonValue::make_object();
			members.set("cell", io::json_number(double(cell.index)));
			members.set("tool", io::json_string(terrain_tool_token(TerrainTool::Stamp)));
			window_requests::set_workspace(workspace, "terrain_brush", std::move(members));
		}
		ImGui::PopID();
	}
}

// The stamp's own turns and flips (the brush's: R, H and V in the view).
void draw_stamp(TerrainPaneContext &context) {
	Workspace &workspace = context.workspace;
	const TerrainBrush &brush = context.brush;
	ImGui::Text("Cell %d, turned %d degrees clockwise%s%s", brush.cell, brush.rotation * 90, brush.flip_h ? ", flipped across" : "",
	            brush.flip_v ? ", flipped down" : "");
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Turn", true, "Turn the stamp a quarter clockwise (R in the view; Shift+R the other way)."))
		window_requests::set_workspace(workspace, "terrain_brush", "rotation", io::json_number(double((brush.rotation + 1) % 4)));
	if (ui_kit::tool(row, "Flip H", true, "Mirror the stamp east to west (H in the view)."))
		window_requests::set_workspace(workspace, "terrain_brush", "flip_h", io::JsonValue::make_bool(!brush.flip_h));
	if (ui_kit::tool(row, "Flip V", true, "Mirror the stamp north to south (V in the view)."))
		window_requests::set_workspace(workspace, "terrain_brush", "flip_v", io::JsonValue::make_bool(!brush.flip_v));
	ImGui::TextDisabled("Click the ground to lay it, its corner under the pointer (Shift: on the 16-unit grid).");
}

// The selected tiles' operations, in the original tools' words.
void draw_selected(TerrainPaneContext &context, const TilDocument *placement) {
	Workspace &workspace = context.workspace;
	const std::string &at = context.document.path();
	const size_t selected = placement ? workspace.view().workspace.document(at).tiles.size() : 0;
	ImGui::Text("%zu tile%s selected", selected, selected == 1 ? "" : "s");
	const bool any = selected > 0;
	const std::string none = "Select tiles first: click one in the view (Shift adds, Ctrl toggles).";
	const auto tip = [&](const char *what) { return any ? std::string(what) : none; };
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Rotate CW", any, tip("Turn each selected tile a quarter clockwise in place (R in the view).")))
		tile_command(workspace, at, "tile_rotate", {1.0});
	if (ui_kit::tool(row, "Rotate CCW", any, tip("Turn each selected tile a quarter counter-clockwise in place (Shift+R).")))
		tile_command(workspace, at, "tile_rotate", {-1.0});
	if (ui_kit::tool(row, "Flip H", any, tip("Mirror each selected tile east to west (H).")))
		tile_command(workspace, at, "tile_flip", {}, "h");
	if (ui_kit::tool(row, "Flip V", any, tip("Mirror each selected tile north to south (V).")))
		tile_command(workspace, at, "tile_flip", {}, "v");
	ui_kit::WrapRow order;
	if (ui_kit::tool(order, "Layer up", any, tip("Draw the selected tiles over every other: to the end of the file (PgUp).")))
		tile_command(workspace, at, "tile_layer", {1.0});
	if (ui_kit::tool(order, "Layer down", any, tip("Draw the selected tiles under every other: to its start (PgDn).")))
		tile_command(workspace, at, "tile_layer", {-1.0});
	if (ui_kit::tool(order, "Delete", any, tip("Remove the selected tiles (Delete).")))
		tile_command(workspace, at, "tile_delete", {});
	const bool group = selected > 1;
	const std::string alone = "Select two tiles or more: the group turns about their middle.";
	ui_kit::WrapRow turn;
	if (ui_kit::tool(turn, "Turn group CW", group,
	                 group ? std::string("Turn the selected tiles together a quarter clockwise about their middle, as MED's group turn.")
	                       : alone))
		tile_command(workspace, at, "tile_group_rotate", {1.0});
	if (ui_kit::tool(turn, "Turn group CCW", group,
	                 group ? std::string("Turn the selected tiles together a quarter counter-clockwise about their middle.") : alone))
		tile_command(workspace, at, "tile_group_rotate", {-1.0});
}

} // namespace

// The Paint tools' values (S25 B4): the surface class from the char map's legend, in the game's words; the foliage
// code from the terrain's definitions (0 grows nothing); the colour; the detail blend map's layer. Each a member of
// the session's brush (set_workspace's terrain_brush).
void draw_paint_part(TerrainPaneContext &context) {
	Workspace &workspace = context.workspace;
	const TerrainBrush &brush = context.brush;
	const auto set = [&](const char *member, io::JsonValue value) {
		window_requests::set_workspace(workspace, "terrain_brush", member, std::move(value));
	};
	// A map the game reads from its own files (a shipped terrain's): copied into the project first, through the
	// import dialog on the game install's files, then painted.
	TerrainPlane plane = TerrainPlane::Surface;
	if (terrain_tool_plane(brush.tool, plane)) {
		const TerrainFamilyFile &file = context.family.plane(plane);
		if (file.missing && !file.name.empty()) {
			ui_kit::WrapRow row;
			if (ui_kit::tool(row, ("Copy " + file.name + " into the project...").c_str(), true,
			                 "Open the import dialog on the game install's " + file.name +
			                         ": once the project holds it, the brush paints it in place."))
				workspace.request(request::preview_install_import({file.name}));
		}
	}
	switch (brush.tool) {
	case TerrainTool::Surface: {
		ImGui::TextUnformatted("Surface class");
		ui_kit::tooltip("The class the game reads under a body or a round there: its footsteps, its impacts, its tyres.");
		for (int surface = 0; surface < kCharmapLegendCount; ++surface) {
			const CharmapLegendColour &c = kCharmapLegend[surface];
			ImGui::PushID(surface);
			ImGui::ColorButton("##legend", ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip,
			                   ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
			ImGui::SameLine();
			const std::string label = std::to_string(surface) + "  " + mission_surface_words(surface);
			if (ImGui::Selectable(label.c_str(), brush.surface == surface)) set("surface", io::json_number(surface));
			if (const char *name = mission_surface_name(surface); name && *name) ui_kit::tooltip(name);
			ImGui::PopID();
		}
		int held = brush.surface;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
		ImGui::InputInt("Class", &held);
		ui_kit::tooltip("Any class, 0 to 255: the game's table names the first 20 (one past them reads as its last).");
		if (ImGui::IsItemDeactivatedAfterEdit()) set("surface", io::json_number(std::clamp(held, 0, 255)));
		break;
	}
	case TerrainTool::Foliage: {
		ImGui::TextUnformatted("Foliage code");
		if (ImGui::Selectable("0  grows nothing", brush.foliage == 0)) set("foliage", io::json_number(0));
		if (context.family.trn)
			for (const FoliageDef &def : context.family.trn->foliage_defs)
				for (const int code : def.match) {
					if (code == FOLIAGE_MATCH_UNSET || code < 0 || code > 255) continue;
					const std::string label = std::to_string(code) + "  " + (def.graphic.empty() ? std::string("(no model)") : def.graphic);
					ImGui::PushID(code);
					if (ImGui::Selectable(label.c_str(), brush.foliage == code)) set("foliage", io::json_number(code));
					ImGui::PopID();
				}
		int held = brush.foliage;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
		ImGui::InputInt("Code", &held);
		ui_kit::tooltip("Any code, 0 to 255: a definition whose match lists it grows there.");
		if (ImGui::IsItemDeactivatedAfterEdit()) set("foliage", io::json_number(std::clamp(held, 0, 255)));
		break;
	}
	case TerrainTool::Colour: {
		float rgb[3] = {((brush.colour >> 16) & 0xFF) / 255.0f, ((brush.colour >> 8) & 0xFF) / 255.0f, (brush.colour & 0xFF) / 255.0f};
		ImGui::ColorEdit3("Colour", rgb);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			char hex[8];
			std::snprintf(hex, sizeof hex, "#%02X%02X%02X", int(std::lround(rgb[0] * 255.0f)), int(std::lround(rgb[1] * 255.0f)),
			              int(std::lround(rgb[2] * 255.0f)));
			set("colour", io::json_string(hex));
		}
		ui_kit::tooltip("The colour the brush lays, its strength the share it covers (Ctrl+click the ground: the colour there).");
		break;
	}
	case TerrainTool::Detail: {
		ImGui::TextUnformatted("Detail layer");
		static const char *const kLayers[] = {"1 (red)", "2 (green)", "3 (blue)", "4 (alpha)"};
		for (int layer = 0; layer < 4; ++layer) {
			ImGui::SameLine();
			if (ImGui::RadioButton(kLayers[layer], brush.layer == layer)) set("layer", io::json_number(layer));
		}
		ui_kit::tooltip("The detail texture whose weight the brush raises, the others' falling: the blend map's channel.");
		break;
	}
	default: ImGui::TextDisabled("Pick a Paint tool: Surface, Foliage, Colour or Detail."); break;
	}
}

void draw_tiles_part(TerrainPaneContext &context) {
	Workspace &workspace = context.workspace;
	const TerrainFamilyFile &tiles = context.family.tiles;
	if (!tiles.refusal.empty()) {
		ImGui::TextWrapped("%s", tiles.refusal.c_str());
		return;
	}
	if (tiles.path.empty()) {
		ImGui::TextDisabled("No tile placement: the terrain names none.");
		return;
	}
	const TilDocument *placement = open_tiles(workspace.view(), tiles.path);
	if (!placement && !tiles.missing) {
		ui_kit::WrapRow row;
		if (ui_kit::tool(row, ("Open " + basename_of(tiles.path)).c_str(), true,
		                 "Open the tile placement beside this view: its tiles are selected here, its edits undone and "
		                 "saved with the view."))
			workspace.request(request::tile_edit(context.document.path(), tiles.path));
	}
	draw_palette(context, placement);
	ImGui::Separator();
	if (context.brush.tool == TerrainTool::Stamp) draw_stamp(context);
	else if (context.brush.tool == TerrainTool::Tiles) draw_selected(context, placement);
	else ImGui::TextDisabled("Stamp lays the palette's cell; Select picks, turns, flips and layers the laid tiles.");
}

} // namespace opennova::editor
