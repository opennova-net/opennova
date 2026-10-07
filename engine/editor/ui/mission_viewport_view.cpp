#include <editor/ui/mission_viewport_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <imgui.h>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mission_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/preview/mission_canvas.h>
#include <editor/preview/mission_hint.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_place.h>
#include <editor/preview/mission_viewport.h>
#include <editor/session/play_controller.h>
#include <editor/session/request_factories.h>
#include <editor/session/script_assist.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/mission_palette_view.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>
#include <formats/trn/charmap_legend.h>

namespace opennova::editor {

namespace {

void set_options(Workspace &workspace, const MissionViewport &mission, const MissionViewportOptions &options) {
	workspace.request(request::set_viewport(
			mission.path(), viewport_change(ViewportKind::Mission, "options", mission_options_to_json(options))));
}

// A command (frame, top, ground, duplicate, select_same, paste, play_from_here) over the selection, as an
// EditInViewport the session plans over its own context (the viewport's device, the selection): a
// refusal is the request's outcome, which the editor reports (Output, the status line), never dropped
// here.
void viewport_command(Workspace &workspace, const MissionViewport &mission, const char *name,
		std::vector<double> by = {}, const CanvasPoint *at = nullptr) {
	ViewportCommand command;
	command.name = name;
	command.kind = ViewportKind::Mission;
	command.by = std::move(by);
	if (at) {
		command.has_at = true;
		command.at_x = at->x;
		command.at_y = at->y;
	}
	workspace.request(request::edit_in_viewport(mission.path(), std::move(command)));
}

// A drop of an item where the picture's point is (the Place tool's, the palette's drag, Place here).
void drop_item(Workspace &workspace, const MissionViewport &mission, int64_t item, CanvasPoint at, float snap) {
	ViewportDrop drop;
	drop.reference = "item";
	drop.name = std::to_string(item);
	drop.x = at.x;
	drop.y = at.y;
	drop.snap = snap;
	drop.kind = ViewportKind::Mission;
	workspace.request(request::edit_in_viewport(mission.path(), std::move(drop)));
}

// A tool's button on the toolbar's row, shown pressed while it is the tool.
bool tool_button(ui_kit::WrapRow &row, const char *label, bool active, bool enabled, const std::string &tip) {
	if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	const bool pressed = ui_kit::tool(row, label, enabled, tip);
	if (active) ImGui::PopStyleColor();
	return pressed;
}

// The viewport's tool set (its options' tool, the Place tool's item, the Path tool's path): one
// SetViewport, as the wire sets it.
void set_tool(Workspace &workspace, const MissionViewport &mission, MissionTool tool, int64_t item = -1, int path = -1) {
	MissionViewportOptions options = mission.options();
	options.tool = tool;
	if (item >= 0) options.item = item;
	if (path >= 0) options.path = path;
	if (options != mission.options()) set_options(workspace, mission, options);
}

// Whether the clipboard pastes at a point of the picture (S15, Paste here): copied entities or areas,
// whose middle goes there. Another clipboard (events, a nested kind's records) pastes as the session's
// rule puts it.
bool pastes_here(const SessionView &view) {
	double middle[2];
	return !view.documents.clipboard.empty() && mission_clip_middle(view.documents.clipboard, middle);
}

} // namespace

// What the view keeps of its own: the palette's list; where the right-click menu was opened. The tool,
// its item and its path, the snaps and the palette's search are the viewport's options (the wire sets
// them too).
struct MissionViewportView::Tools {
	float snap = 1.0f; // the options' snap, as the frame began
	MissionPaletteView palette;
	CanvasPoint menu_at;
	NodeAddress menu_record;
	// The last hint the canvas gave (what a click does now), and where the pointer was on the picture
	// (Ctrl+V pastes there).
	std::string hint;
	CanvasPoint mouse;
	bool mouse_on_picture = false;
	// The ground under the pointer in the game's words (DI-07), as the canvas last read it: its line and
	// its surface class (the char map legend's swatch; -1 none).
	std::string ground;
	int ground_surface = -1;
	// A model several items draw, let go over the picture (DI-12): the items, where it was let go, its file.
	std::vector<int64_t> choices;
	CanvasPoint choice_at;
	std::string choice_model;

	void toolbar(Workspace &workspace, const MissionViewport &mission, const ViewportContext &context);
	void show_popup(MissionViewportOptions &options);
	void time_popup(MissionViewportOptions &options, const MissionViewport &mission);
	void numbers(Workspace &workspace, const MissionViewport &mission, const ViewportContext &context);
	void path_list(Workspace &workspace, const MissionViewport &mission);
	void canvas_menu(Workspace &workspace, const MissionViewport &mission, const ViewportContext &context,
			const MissionCanvas &canvas);
	void events_using(Workspace &workspace, const MissionViewport &mission, const SessionView &view);
	void notes(const MissionViewport &mission);
	void ground_line();
	float snap_metres() const { return snap; }
};

MissionViewportView::MissionViewportView() : ViewportView(ViewportKind::Mission), tools_(std::make_unique<Tools>()) {}

MissionViewportView::~MissionViewportView() = default;

void MissionViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &mission = static_cast<const MissionViewport &>(viewport);
	Tools &tools = *tools_;
	const SessionView &view = workspace.view();
	// Esc under a tool goes back to Select once the canvas has read this frame (where it is not taken
	// as "select nothing").
	const MissionTool tool = mission.options().tool;
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
	auto *canvas = static_cast<MissionCanvas *>(half());
	// An Esc while a press is down is the canvas's (it cancels the press, the tool kept).
	const bool stop = tool != MissionTool::Select && focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
					  !(canvas && canvas->gesture().pressed());
	tools.snap = mission.options().snap;
	tools.toolbar(workspace, mission, context);
	snap = tools.snap_metres();
	context.snap = snap;
	tools.numbers(workspace, mission, context);
	if (canvas) canvas->set_turn(mission.options().turn);
	// The clipboard's keys while the view has the keyboard and no press is down: Copy, Cut, and Paste at
	// the pointer over the picture (else as the session's rule puts it).
	const bool pressed = canvas && canvas->gesture().pressed();
	if (focused && !pressed && mission.view_status() == MissionViewStatus::Ready) {
		const std::string &path = mission.path();
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) {
			EditorRequest copy = request::of(EditorRequestKind::Copy);
			copy.path = path;
			workspace.request(std::move(copy));
		} else if (context.editable() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) {
			EditorRequest cut = request::of(EditorRequestKind::Cut);
			cut.path = path;
			workspace.request(std::move(cut));
		} else if (context.editable() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V)) {
			// Over the picture, copied entities and areas with their middle where the pointer is; anything
			// else, or off the picture, as the session's rule puts it.
			if (tools.mouse_on_picture && pastes_here(view)) viewport_command(workspace, mission, "paste", {}, &tools.mouse);
			else workspace.request(request::paste(path));
		}
	}
	// Under the canvas: the hint (what a click does now), the ground under the pointer while the mission
	// names a terrain (DI-07), then a line while the picture lacks a file.
	const float line = ImGui::GetFrameHeightWithSpacing();
	const bool terrain = !mission.scene().header().terrain.empty();
	const float under = line + (terrain ? line : 0.0f) + (mission.missing().empty() ? 0.0f : line);
	const float height = std::max(48.0f, ImGui::GetContentRegionAvail().y - under);
	// Beside the picture while a tool picks from a list: the palette (Place) or the paths (Path).
	const bool side = tool == MissionTool::Place || tool == MissionTool::Path;
	const float avail = ImGui::GetContentRegionAvail().x;
	const float panel = side ? std::min(ImGui::GetFontSize() * 17.0f, avail * 0.45f) : 0.0f;
	if (side) ImGui::BeginChild("##picture", ImVec2(avail - panel - ImGui::GetStyle().ItemSpacing.x, height));
	const std::string path = mission.path();
	ViewportView::canvas(workspace, viewport, context, side ? ImGui::GetContentRegionAvail().y : height,
			[&](const CanvasInput &in) {
				tools.mouse = in.mouse;
				tools.mouse_on_picture = in.hovered;
				if (canvas) {
					tools.hint = canvas->hint(context, in);
					const MissionGroundFacts &ground = canvas->ground(context, in);
					tools.ground = mission_ground_line(ground);
					tools.ground_surface = ground.on == MissionGroundOn::Terrain && !ground.under_water ? ground.surface : -1;
				}
				// Let go over the picture: a palette's item placed there, or a Files row's model (whose item
				// the viewport finds, refusing another file and naming why).
				if (ImGui::BeginDragDropTarget()) {
					const ImGuiPayload *dragged = ImGui::GetDragDropPayload();
					if (dragged && dragged->IsDataType(kItemDragPayload) && dragged->Data &&
							ImGui::AcceptDragDropPayload(kItemDragPayload)) {
						const std::optional<int> item = strutil::parse_int(static_cast<const char *>(dragged->Data));
						if (item) drop_item(workspace, mission, *item, in.mouse, tools.snap_metres());
					} else {
						const AssetEntry *entry = nullptr;
						if (dragged && dragged->IsDataType(kFileDragPayload) && dragged->Data && view.project.scan) {
							const std::string file(static_cast<const char *>(dragged->Data));
							for (const AssetEntry &candidate : view.project.scan->entries)
								if (candidate.relative_path == file) entry = &candidate;
						}
						// Only a model is taken: another file is never accepted. Its item placed (made first where none
						// draws it, DI-12); where several draw it, a choice of them, placed where it was let go.
						if (entry && entry->kind == AssetKind::Model && ImGui::AcceptDragDropPayload(kFileDragPayload)) {
							tools.choices = mission_items_of_model(view, entry->relative_path);
							if (tools.choices.size() > 1) {
								tools.choice_at = in.mouse;
								tools.choice_model = entry->logical_name;
								ImGui::OpenPopup("mission_drop_choice");
							} else {
								ViewportDrop drop;
								drop.file = entry->logical_name;
								drop.x = in.mouse.x;
								drop.y = in.mouse.y;
								drop.snap = tools.snap_metres();
								drop.kind = ViewportKind::Mission;
								workspace.request(request::edit_in_viewport(path, std::move(drop)));
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
				// The right button: the mark under it selected unless it already is, and the menu of
				// what applies there.
				if (canvas_ui().right_clicked() && canvas && !canvas->gesture().pressed()) {
					tools.menu_at = in.mouse;
					tools.menu_record = NodeAddress();
					const int mark = mission_canvas_under(canvas->frame(), in, MissionPick::Click);
					if (mark >= 0) {
						tools.menu_record = canvas->frame().marks[size_t(mark)].record;
						if (!view.documents.selection.holds(tools.menu_record))
							workspace.request(request::select_record(path, tools.menu_record));
					}
					ImGui::OpenPopup("mission_canvas_menu");
				}
				if (ImGui::BeginPopup("mission_canvas_menu")) {
					if (canvas) tools.canvas_menu(workspace, mission, context, *canvas);
					ImGui::EndPopup();
				}
				// A model several items draw, let go over the picture: the item to place there.
				if (ImGui::BeginPopup("mission_drop_choice")) {
					ImGui::TextDisabled("Several items draw %s: place", tools.choice_model.c_str());
					for (const int64_t item : tools.choices) {
						MissionItemFacts facts;
						std::string ignored;
						mission_item_facts(view, item, facts, ignored);
						const std::string label = (facts.name.empty() ? std::string("Item") : facts.name) + " (" +
						                          std::to_string(item) + ")";
						if (ImGui::Selectable(label.c_str())) drop_item(workspace, mission, item, tools.choice_at, tools.snap_metres());
					}
					ImGui::EndPopup();
				}
			});
	if (side) {
		ImGui::EndChild();
		ImGui::SameLine();
		if (ImGui::BeginChild("##tool_list", ImVec2(panel, height))) {
			if (tool == MissionTool::Place) {
				const AssetGraph *graph = view.findings.graph.get();
				// The search typed is the viewport's (its options' palette).
				std::string typed = mission.options().palette;
				const int64_t picked = tools.palette.draw(graph, graph ? graph->generation() : 0, view.project.recent_items,
						mission.options().item, mission.options().palette, &typed);
				// The search alone (review X21): another option a client set in the same pump stays as it set it.
				if (typed != mission.options().palette) {
					io::JsonValue palette = io::JsonValue::make_object();
					palette.set("palette", io::JsonValue::make_string(typed));
					workspace.request(request::set_viewport(mission.path(), viewport_change(ViewportKind::Mission, "options",
					                                                                        std::move(palette))));
				}
				if (picked != 0) set_tool(workspace, mission, MissionTool::Place, picked);
			} else {
				tools.path_list(workspace, mission);
			}
		}
		ImGui::EndChild();
	}
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ui_kit::clipped_text(tools.hint);
	ImGui::PopStyleColor();
	if (terrain) tools.ground_line();
	if (!mission.missing().empty()) tools.notes(mission);
	if (stop) set_tool(workspace, mission, MissionTool::Select);
}

void MissionViewportView::Tools::toolbar(Workspace &workspace, const MissionViewport &mission,
		const ViewportContext &context) {
	MissionViewportOptions options = mission.options();
	const float unit = ImGui::GetFontSize();
	// The row wraps whole controls; a tab narrower than a control's full form (the Document window
	// beside the Preview in a narrow layout) gets its narrow one: a snap's combo as wide as the room
	// leaves beside its label, then with no label (its tooltip names it), never wider than the line.
	const float line = ImGui::GetContentRegionAvail().x;
	ui_kit::WrapRow row;
	const bool edits = context.editable();
	const std::string held = context.not_editable();
	// The tools: what a click on the picture does (a tool pressed again goes back to Select).
	const MissionTool tool = options.tool;
	const auto pick = [&](MissionTool chosen) { options.tool = tool == chosen ? MissionTool::Select : chosen; };
	if (tool_button(row, "Select", tool == MissionTool::Select, true,
				"Click a mark to select it (Shift adds, Ctrl toggles), drag it to move it (Alt-drag copies it), "
				"drag on nothing for a box. Esc."))
		options.tool = MissionTool::Select;
	if (tool_button(row, "Place", tool == MissionTool::Place, edits,
				edits ? "Pick an item in the palette, then click the ground to place one, facing the way the camera "
						"looks; or drag it from the palette onto the picture. Esc stops."
					  : held))
		pick(MissionTool::Place);
	if (tool_button(row, "Path stops", tool == MissionTool::Path, edits,
				edits ? "Pick a waypoint path, then click the ground to add its next stop (a marker the path visits). "
						"Esc stops."
					  : held))
		pick(MissionTool::Path);
	if (tool_button(row, "Area", tool == MissionTool::Area, edits,
				edits ? "Drag a box on the ground to make an area trigger over it. Esc stops." : held))
		pick(MissionTool::Area);
	// A step of the options (the snap, the turn) picked from its list; one the wire set off the list shown as
	// its value.
	const auto combo = [&](const char *label, float &value, const float *steps, const char *const *names, int count,
	                       const char *unit_word, const char *tip) {
		const float least = unit * 3.0f;
		const bool labelled = ui_kit::field_width(least, label) <= line;
		const float width = std::max(least, std::min(unit * 5.0f, labelled ? line - ui_kit::field_width(0.0f, label) : line));
		const std::string id = labelled ? std::string(label) : "##" + std::string(label);
		row.next(labelled ? ui_kit::field_width(width, label) : width);
		ImGui::SetNextItemWidth(width);
		int index = -1;
		for (int i = 0; i < count; ++i)
			if (steps[i] == value) index = i;
		char shown[32];
		std::snprintf(shown, sizeof(shown), "%g %s", double(value), unit_word);
		if (ImGui::BeginCombo(id.c_str(), index >= 0 ? names[index] : shown)) {
			for (int i = 0; i < count; ++i)
				if (ImGui::Selectable(names[i], i == index)) value = steps[i];
			ImGui::EndCombo();
		}
		ui_kit::tooltip(labelled ? std::string(tip) : std::string(label) + ": " + tip);
	};
	static const char *const kSnapNames[] = { "Free", "1/4 m", "1 m", "5 m", "10 m" };
	combo("Snap", options.snap, kMissionSnaps, kSnapNames, IM_ARRAYSIZE(kSnapNames), "m",
			"What a move, a placed record and an area's edge snap to on the file's axes, and how far the arrows "
			"nudge (Shift: a tenth of it) and Ctrl+D's copy goes. Hold Ctrl while dragging to move freely.");
	static const char *const kTurnNames[] = { "1 deg", "5 deg", "15 deg", "45 deg", "90 deg" };
	combo("Turn", options.turn, kMissionTurns, kTurnNames, IM_ARRAYSIZE(kTurnNames), "deg",
			"What a turned entity's heading snaps to. Hold Ctrl while turning to turn freely.");
	row.next(ui_kit::checkbox_width("Stick"));
	ImGui::Checkbox("Stick", &options.stick);
	ui_kit::tooltip("A move keeps each entity's height over the ground; off, its height stands.");
	if (ui_kit::tool(row, "Show", true, "What the picture draws (terrain, sky, water, models), what the viewport marks "
										"over it, the labels and how far a mark is drawn."))
		ImGui::OpenPopup("show");
	if (ImGui::BeginPopup("show")) {
		show_popup(options);
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Time", true, "The time of day the picture shows: the mission's start time, or an hour."))
		ImGui::OpenPopup("time");
	if (ImGui::BeginPopup("time")) {
		time_popup(options, mission);
		ImGui::EndPopup();
	}
	if (ui_kit::tool(row, "Frame", true, "Look at the selected records, or at every entity (F, or a double click on "
										 "the picture)."))
		viewport_command(workspace, mission, "frame");
	if (ui_kit::tool(row, "Top", true, "Look straight down over the camera's target, north up."))
		viewport_command(workspace, mission, "top");
	// What the ground under the selected entities is: each set down on it.
	if (ui_kit::tool(row, "Ground", edits && mission.ground(),
				!edits ? held
				: mission.ground() ? std::string("Set each selected entity down on the ground under it (the picture's "
												 "right-click menu: Drop to ground).")
								   : std::string("The picture has no ground yet (its terrain is not built).")))
		viewport_command(workspace, mission, "ground");
	// Play mission: the build, then the game started in this mission (the session's own rule for
	// the active document, play_mission_for: Ctrl+F5 is the same request).
	const SessionView &view = workspace.view();
	const std::string played = play_mission_for(view);
	const bool plays = view.project.open && view.activity.play_state == PlayState::Stopped &&
			view.allows(EditorRequestKind::Play) && !played.empty();
	if (ui_kit::tool(row, "Play mission", plays,
				played.empty() ? std::string("Make the mission the active document to start the game in it.")
							   : "Build, then start the game in " + played + " (Ctrl+F5)."))
		workspace.request(request::play(played));
	// Play from here (DI-26): the game started in this mission with its player on the ground under the camera,
	// facing the way it looks (the picture's right-click menu starts it where the pointer was).
	const bool from_here = plays && !play_mission_at(view, mission.path()).empty();
	if (ui_kit::tool(row, "Play from here", from_here,
				from_here ? "Build, then start the game in this mission with the player on the ground under the camera, "
							"facing the way it looks (Alt+F5). The build's copy of the mission gets the start; the "
							"mission's own file is left as it is."
						  : std::string("Make the mission the active document to start the game in it.")))
		viewport_command(workspace, mission, "play_from_here");
	// The mission's script (S15): the <stem>.wac the game compiles with it [orig: WacScript_InitAndLoad @
	// 0x4F91F0], opened, or made beside the mission where the project has none.
	const MissionScript script = mission_script(view, mission.path());
	if (!script.name.empty()) {
		const bool held = !script.path.empty();
		if (ui_kit::tool(row, "Script", view.project.open,
					held ? "Open " + script.path + ", the script the game runs with this mission."
						 : "The mission has no script (" + script.name + "): make one beside it.")) {
			if (held) workspace.request(request::open_document(script.path));
			else ImGui::OpenPopup("make_script");
		}
		if (ImGui::BeginPopup("make_script")) {
			ImGui::TextUnformatted(("Make " + script.create_at + "?").c_str());
			ImGui::TextDisabled("The game compiles it with the mission; an empty script does nothing.");
			if (ImGui::Button("Make it")) {
				workspace.request(request::create_file(script.create_at));
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
	if (options != mission.options()) set_options(workspace, mission, options);
}

// The selected entity's place and heading in plain units, typed exactly: each field a batch of the
// selection (several move as far, turn about their centre), one undo step.
void MissionViewportView::Tools::numbers(Workspace &workspace, const MissionViewport &mission,
		const ViewportContext &context) {
	const SessionView &view = workspace.view();
	const Document *document = context.input.document ? records_of(*context.input.document) : nullptr;
	if (!document || view.documents.active != mission.path() || !mission.current(context.input)) return;
	const Selection &selection = view.documents.selection;
	const MissionEntityMark *primary = mission.scene().entity(selection.primary.row);
	if (!primary) return;
	// The selection as a press finds it, the primary first.
	std::vector<MissionPressed> pressed;
	MissionPressed held;
	if (mission.pressed(selection.primary, held)) pressed.push_back(held);
	for (const NodeAddress &record : selection.records) {
		MissionPressed each;
		if (record != selection.primary && mission.pressed(record, each)) pressed.push_back(each);
	}
	const bool edits = context.editable();
	const float unit = ImGui::GetFontSize();
	ui_kit::WrapRow row;
	std::vector<Edit> batch;
	// Wide enough for the widest a mission's span reads (a map is some kilometres across) with its unit.
	const float width = std::max(unit * 4.5f, ImGui::CalcTextSize("-00000.00 m").x + ImGui::GetStyle().FramePadding.x * 2.0f);
	const auto number = [&](const char *label, const char *id, double value, const char *format, const char *tip) {
		row.next(ui_kit::field_width(width, label));
		ImGui::SetNextItemWidth(width);
		ImGui::BeginDisabled(!edits);
		ImGui::InputDouble((std::string(label) + "###" + id).c_str(), &value, 0.0, 0.0, format);
		ImGui::EndDisabled();
		ui_kit::tooltip(edits ? std::string(tip) : context.not_editable());
		return ImGui::IsItemDeactivatedAfterEdit() ? std::optional<double>(value) : std::nullopt;
	};
	// The file's x, y and z are mission units, which the game reads as metres (a trigger's whole-metre
	// distance shifts into the same 16.16 world units [orig: Entity_CompareDistancesToTarget @0x4F12E0;
	// world/world-wac-ai-re.md]); its yaw whole degrees clockwise from north, the engine's heading 90
	// minus it [orig: Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66].
	const char *metres = "Metres: the game reads a mission unit as a metre. Enter moves the selection there "
						 "(several selected move as far).";
	if (const auto x = number("East", "east", primary->x, "%.2f m", metres)) {
		const double to[2] = { *x, primary->y };
		mission_move_edits(*document, pressed, 0, to, 0.0f, mission.options().stick, context.device, 0, batch);
	}
	if (const auto y = number("North", "north", primary->y, "%.2f m", metres)) {
		const double to[2] = { primary->x, *y };
		mission_move_edits(*document, pressed, 0, to, 0.0f, mission.options().stick, context.device, 0, batch);
	}
	if (const auto z = number("Height", "height", primary->z, "%.2f m",
				"Metres above the mission's zero (the file's z is absolute, not over the ground)."))
		mission_height_edits(*document, pressed, 0, *z - primary->z, 0.0f, 0, batch);
	if (const auto heading = number("Heading", "heading", double(primary->yaw), "%.0f deg",
				"Degrees clockwise from north: 0 north, 90 east (the file's yaw). Several selected turn about "
				"their centre."))
		mission_yaw_edits(*document, pressed, 0, *heading - double(primary->yaw), 0.0f, 0, batch, mission.options().stick,
				context.device);
	if (!batch.empty()) workspace.request(request::edit_record(document->path(), std::move(batch)));
	if (pressed.size() > 1) {
		const std::string many = std::to_string(selection.records.size()) + " selected";
		row.next(ui_kit::text_width(many.c_str()));
		ImGui::TextDisabled("%s", many.c_str());
	}
}

// The Path tool's list: the mission's waypoint paths by number with their stops, the picked one shown.
void MissionViewportView::Tools::path_list(Workspace &workspace, const MissionViewport &mission) {
	const int path = mission.options().path;
	const auto choose = [&](int number) { set_tool(workspace, mission, MissionTool::Path, -1, number); };
	ImGui::TextDisabled("Waypoint paths");
	ui_kit::tooltip("Pick the path a click on the picture adds its next stop to. A stop is a marker the path "
					"visits in turn.");
	if (!ImGui::BeginChild("paths", ImVec2(0.0f, 0.0f))) {
		ImGui::EndChild();
		return;
	}
	// The used paths first (by number), then every free number to start a new one.
	std::vector<std::pair<int, size_t>> used;
	for (const MissionPathMark &each : mission.scene().paths()) used.emplace_back(each.index, each.stops.size());
	for (const auto &[number, stops] : used) {
		const std::string label = "Path " + std::to_string(number) + " (" + std::to_string(stops) +
				(stops == 1 ? " stop)" : " stops)");
		if (ImGui::Selectable(label.c_str(), path == number)) choose(number);
	}
	if (!used.empty()) ImGui::Separator();
	ImGui::TextDisabled("New path");
	for (int number = 1; number < 123; ++number) {
		if (std::any_of(used.begin(), used.end(), [&](const auto &each) { return each.first == number; })) continue;
		const std::string label = "Path " + std::to_string(number) + " (empty)";
		if (ImGui::Selectable(label.c_str(), path == number)) choose(number);
	}
	ImGui::EndChild();
}

// The picture's right-click menu: what applies where it was opened.
void MissionViewportView::Tools::canvas_menu(Workspace &workspace, const MissionViewport &mission,
		const ViewportContext &context, const MissionCanvas &canvas) {
	const SessionView &view = workspace.view();
	const bool edits = context.editable();
	const bool selected = view.documents.active == mission.path() && !view.documents.selection.empty();
	// Place here: the picked item, else the one placed last.
	const int64_t picked = mission.options().item;
	const int64_t item = picked != 0 ? picked : view.project.recent_items.empty() ? 0 : view.project.recent_items.front();
	std::string item_name = item == picked && !canvas.place_name().empty() ? canvas.place_name() : palette.name_of(item);
	if (item_name.empty() && item != 0) item_name = "item " + std::to_string(item);
	if (ImGui::MenuItem(item != 0 ? ("Place " + item_name + " here").c_str() : "Place here", nullptr, false, edits && item != 0))
		drop_item(workspace, mission, item, menu_at, snap_metres());
	if (item == 0) ui_kit::tooltip("Pick an item with Place first.");
	const bool clip = pastes_here(view);
	if (ImGui::MenuItem("Paste here", "Ctrl+V", false, edits && clip)) viewport_command(workspace, mission, "paste", {}, &menu_at);
	if (!clip)
		ui_kit::tooltip(view.documents.clipboard.empty()
								? "Copy entities or areas first (Ctrl+C)."
								: "The clipboard holds no entities or areas (events and nested records paste in the outline).");
	ImGui::Separator();
	// Play from here (DI-26): the game started in this mission with its player where the menu was opened.
	const bool plays = view.project.open && view.activity.play_state == PlayState::Stopped &&
			view.allows(EditorRequestKind::Play) && !play_mission_at(view, mission.path()).empty();
	if (ImGui::MenuItem("Play from here", nullptr, false, plays))
		viewport_command(workspace, mission, "play_from_here", {}, &menu_at);
	ui_kit::tooltip("Build, then start the game in this mission with the player on the ground here, facing the way the "
					"camera looks.");
	ImGui::Separator();
	if (ImGui::MenuItem("Frame", "F", false, true)) viewport_command(workspace, mission, "frame");
	if (ImGui::MenuItem("Drop to ground", nullptr, false, edits && selected && mission.ground()))
		viewport_command(workspace, mission, "ground");
	if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, edits && selected)) {
		// A step to the camera's right, as Ctrl+D goes.
		double east = 0.0, north = 0.0;
		canvas.step_right(east, north);
		const double step = snap_metres() > 0.0f ? double(snap_metres()) : 1.0;
		viewport_command(workspace, mission, "duplicate", { east * step, north * step });
	}
	if (ImGui::MenuItem("Delete", "Delete", false, edits && selected)) {
		std::vector<Edit> removes;
		for (const NodeAddress &record : view.documents.selection.records) {
			Edit edit;
			edit.operation = EditOperation::Remove;
			edit.address = record;
			removes.push_back(std::move(edit));
		}
		workspace.request(request::edit_record(mission.path(), std::move(removes)));
	}
	ImGui::Separator();
	const bool entity = menu_record.row && mission.scene().entity(menu_record.row);
	if (ImGui::MenuItem("Select same item", nullptr, false, entity || selected))
		viewport_command(workspace, mission, "select_same");
	ui_kit::tooltip("Select every entity of the mission placed from the same item.");
	if (ImGui::MenuItem("Go to in outline", nullptr, false, menu_record.row != 0)) {
		EditorRequest go = request::open_document(mission.path(), std::string(), entity ? "item" : "id");
		go.address = menu_record;
		workspace.request(std::move(go));
	}
	if (ImGui::BeginMenu("Show events using this", menu_record.row != 0)) {
		events_using(workspace, mission, view);
		ImGui::EndMenu();
	}
}

// The events (and any other record of the mission) that name the record the menu was opened on:
// an entity by its SSN, an area by its zone id; each opens at its field.
void MissionViewportView::Tools::events_using(Workspace &workspace, const MissionViewport &mission, const SessionView &view) {
	const AssetGraph *graph = view.findings.graph.get();
	const DocumentBase *base = nullptr;
	for (const auto &open : view.documents.open)
		if (open && open->path() == mission.path()) base = open.get();
	const Document *document = base ? records_of(*base) : nullptr;
	if (!graph || !document) {
		ImGui::TextDisabled("Not known yet (the project is still being read).");
		return;
	}
	ReferenceKind kind = ReferenceKind::MissionEntity;
	std::string name;
	if (const MissionEntityMark *entity = mission.scene().entity(menu_record.row)) {
		Value ssn;
		if (document->get(menu_record, "id", ssn) && std::holds_alternative<int64_t>(ssn))
			name = std::to_string(std::get<int64_t>(ssn));
		(void)entity;
	} else if (const MissionAreaMark *area = mission.scene().area(menu_record.row)) {
		kind = ReferenceKind::MissionZone;
		name = std::to_string(area->zone);
	}
	const std::vector<const GraphEdge *> users =
			name.empty() ? std::vector<const GraphEdge *>() : graph->referrers_of(kind, name, mission_scope(*document));
	if (users.empty()) {
		ImGui::TextDisabled("No event names it.");
		return;
	}
	const GraphNameSource names(*graph);
	for (size_t i = 0; i < users.size(); ++i) {
		const GraphEdge &edge = *users[i];
		std::string label = edge.record.empty() ? edge.source : edge.record;
		if (edge.source == mission.path() && edge.address.row) {
			// The event (its sentence) and the trigger or action, by the display names.
			const NodeAddress row{ edge.address.row, edge.address.kind, 0 };
			label = record_display(*document, row, &names);
			if (edge.address.child) label += ": " + record_display(*document, edge.address, &names);
		}
		ImGui::PushID(int(i));
		if (ImGui::MenuItem(ui_kit::fit(label, ImGui::GetFontSize() * 24.0f).c_str()))
			workspace.request(request::open_document(edge.source, edge.locator, edge.field));
		ImGui::PopID();
	}
}

void MissionViewportView::Tools::show_popup(MissionViewportOptions &options) {
	ImGui::TextDisabled("The picture");
	ImGui::Checkbox("Terrain", &options.terrain);
	ImGui::Checkbox("Sky", &options.sky);
	ImGui::Checkbox("Water", &options.water);
	ImGui::Checkbox("Models", &options.models);
	ImGui::Checkbox("Static shadows", &options.shadows);
	ImGui::Separator();
	ImGui::TextDisabled("The marks");
	ImGui::Checkbox("Items", &options.items);
	ImGui::Checkbox("Buildings", &options.buildings);
	ImGui::Checkbox("Markers", &options.markers);
	ImGui::Checkbox("Organics", &options.organics);
	ImGui::Checkbox("Areas", &options.areas);
	ImGui::Checkbox("Paths", &options.paths);
	ImGui::Checkbox("Labels", &options.labels);
	ui_kit::tooltip("A label beside the nearer marks too, not only the hovered and the selected ones (those that "
					"would overlap another are left out).");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	float range = options.mark_range;
	if (ImGui::SliderFloat("Mark range", &range, 0.0f, 2000.0f, range <= 0.0f ? "no limit" : "%.0f m",
				ImGuiSliderFlags_AlwaysClamp))
		options.mark_range = range;
	ui_kit::tooltip("How far from the eye a mark is still drawn and picked, metres; 0 for no limit.");
}

void MissionViewportView::Tools::time_popup(MissionViewportOptions &options, const MissionViewport &mission) {
	bool own = options.time < 0.0;
	// The header's start time is hours in 8.8 fixed point (the game shifts it into its 8.24 clock
	// [orig: Game_StartMission @ 0x525371]): 0x0C80 is 12:30.
	const int start = mission.scene().header().start_time;
	char label[64];
	std::snprintf(label, sizeof(label), "The mission's start time (%02d:%02d)", (start >> 8) & 0xFF, ((start & 0xFF) * 60) >> 8);
	if (ImGui::Checkbox(label, &own)) options.time = own ? -1.0 : 12.0;
	ImGui::BeginDisabled(own);
	float hour = options.time < 0.0 ? 12.0f : float(options.time);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	if (ImGui::SliderFloat("Hour", &hour, 0.0f, 24.0f, "%.1f h", ImGuiSliderFlags_AlwaysClamp) && !own)
		options.time = double(hour);
	ImGui::EndDisabled();
}

void MissionViewportView::Tools::ground_line() {
	if (ground.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text("Ground: point at the terrain to read its surface class, the footsteps a body plays there and "
							 "the row a round plays.");
		ImGui::PopStyleColor();
		return;
	}
	// The class's colour in the char map legend, the colour a surface map paints it (formats/trn/charmap_legend.h).
	if (ground_surface >= 0 && ground_surface < kCharmapLegendCount) {
		const CharmapLegendColour &c = kCharmapLegend[ground_surface];
		const float side = ImGui::GetTextLineHeight();
		ImGui::ColorButton("##surface", ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f),
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
				ImVec2(side, side));
		ImGui::SameLine();
	}
	ui_kit::clipped_text("Ground: " + ground);
}

void MissionViewportView::Tools::notes(const MissionViewport &mission) {
	const std::vector<std::string> &missing = mission.missing();
	std::string line = std::to_string(missing.size()) + (missing.size() == 1 ? " file missing: " : " files missing: ");
	std::string all;
	for (size_t i = 0; i < missing.size(); ++i) {
		if (i < 3) line += (i ? ", " : "") + missing[i];
		all += (i ? "\n" : "") + missing[i];
	}
	if (missing.size() > 3) line += ", ...";
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
	ui_kit::clipped_text(line, all + "\nImport them to see them (Problems).");
	ImGui::PopStyleColor();
}

} // namespace opennova::editor
