#include <editor/ui/model_viewport_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_document.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_canvas.h>
#include <runtime/anim/anim_event_bits.h>

namespace opennova::editor {

namespace {

constexpr int64_t kRegisterRange = 32767; // a slider's reach (the value is the register's word)
// A clip event's tick on the timeline (a step's is green, a shot's red).
constexpr ImU32 kEventColor = IM_COL32(255, 220, 90, 255);

void set_options(Workspace &workspace, const ModelViewport &model, const ModelViewportOptions &options) {
	workspace.request(request::set_viewport(
			model.path(), viewport_change(ViewportKind::Model, "options", model_options_to_json(options))));
}

// The preview clock run or held, or sought to a clip tick (and held).
void set_playing(Workspace &workspace, const ModelViewport &model, bool playing) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set("playing", io::JsonValue::make_bool(playing));
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Model, "clock", std::move(clock))));
}
void seek_ticks(Workspace &workspace, const ModelViewport &model, int32_t ticks, bool hold) {
	io::JsonValue clock = io::JsonValue::make_object();
	clock.set("ticks", io::json_number(std::max(ticks, 0)));
	if (hold) clock.set("playing", io::JsonValue::make_bool(false));
	workspace.request(request::set_viewport(model.path(), viewport_change(ViewportKind::Model, "clock", std::move(clock))));
}

} // namespace

// What the view keeps of its own: the snap.
struct ModelViewportView::Tools {
	int snap = 2; // kModelHandleSnaps: 1/16 m

	void toolbar(Workspace &workspace, const ModelViewport &model, const ViewportContext &context);
	void registers(Workspace &workspace, const ModelViewport &model);
	void rig_chooser(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model);
	void timeline(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock);
};

ModelViewportView::ModelViewportView() : ViewportView(ViewportKind::Model), tools_(std::make_unique<Tools>()) {}

ModelViewportView::~ModelViewportView() = default;

void ModelViewportView::draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) {
	ViewportView::draw_empty(workspace, model, path);
	// An animation no item pairs: the author picks the model it plays on.
	const auto *shown = static_cast<const ModelViewport *>(model);
	if (shown && shown->view_status() == ModelViewStatus::NoRig) {
		ui_kit::WrapRow row;
		tools_->rig_chooser(workspace, row, *shown);
	}
}

void ModelViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const ModelViewport &>(viewport);
	if (!model.model()) {
		draw_empty(workspace, &viewport, viewport.path());
		return;
	}
	// An animation: the model it plays on (the Preview window's line names it), the clip the
	// selection plays (its slot in words, the table's key in the tooltip) and where the pairing
	// comes from, cut to the room left (whole in its tooltip) or on a line of its own in a narrow
	// window.
	if (model.animating()) {
		const PreviewRig &rig = model.rig();
		ui_kit::WrapRow row;
		tools_->rig_chooser(workspace, row, model);
		const std::string &key = model.clip_key();
		const std::string slot = animation_key_title(key);
		const std::string clip = key.empty() ? "(" + rig.source + ")"
		                                     : "plays " + slot + " #" + std::to_string(model.clip_variant()) + " (" + rig.source + ")";
		row.next(std::min(ui_kit::text_width(clip.c_str()), ImGui::GetFontSize() * 8.0f));
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text(clip, slot != key ? clip + "\n" + key : std::string());
		ImGui::PopStyleColor();
		if (!model.detail().empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
			ImGui::TextWrapped("%s", model.detail().c_str());
			ImGui::PopStyleColor();
		}
	}
	tools_->toolbar(workspace, model, context);
	snap = kModelHandleSnaps[std::clamp(tools_->snap, 0, 4)];
	context.snap = snap;
	const float timeline = model.animating() ? ImGui::GetFrameHeightWithSpacing() * 2.0f + 6.0f : 0.0f;
	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y - timeline));
	if (model.animating()) tools_->timeline(workspace, model, context.input.clock);
}

// The model an animation plays on: Auto (the one an item pairs with the table) or a model
// of the project's.
void ModelViewportView::Tools::rig_chooser(Workspace &workspace, ui_kit::WrapRow &row, const ModelViewport &model) {
	const SessionView &view = workspace.view();
	ModelViewportOptions options = model.options();
	const float width = ImGui::GetFontSize() * 10.0f;
	row.next(ui_kit::field_width(width, "Plays on"));
	ImGui::SetNextItemWidth(width);
	if (ImGui::BeginCombo("Plays on", options.rig_model.empty() ? "Auto" : options.rig_model.c_str())) {
		if (ImGui::Selectable("Auto", options.rig_model.empty())) options.rig_model.clear();
		for (const AssetEntry &entry : view.project.scan->entries)
			if (entry.kind == AssetKind::Model &&
			    ImGui::Selectable(entry.logical_name.c_str(), entry.logical_name == options.rig_model))
				options.rig_model = entry.logical_name;
		ImGui::EndCombo();
	}
	ui_kit::tooltip("The model the animation plays on. Auto takes the graphic of an item whose "
	                "anim_def names the table.");
	if (options != model.options()) set_options(workspace, model, options);
}

// The clip the selection plays: run or hold it, step a tick, scrub; its trigger events
// under the track (a click on one seeks there and, in the clip's own document, selects it).
void ModelViewportView::Tools::timeline(Workspace &workspace, const ModelViewport &model, const PreviewClock &clock) {
	const int32_t length = model.clip_length_ticks();
	if (model.clip_key().empty() || length <= 0) {
		ImGui::TextDisabled("%s", model.rig().table.empty() || model.clip_key().empty()
		                                  ? "Select a row of the table to play its clip."
		                                  : "The clip does not load in the rig.");
		return;
	}
	const int32_t ticks = clock.ticks();
	const int32_t shown = model.clip_loops() ? ticks % length : std::min(ticks, length);
	if (ImGui::Button(clock.playing() ? "Pause##clip" : "Run##clip")) set_playing(workspace, model, !clock.playing());
	ui_kit::tooltip(clock.playing() ? "Hold the clip where it is." : "Run the clip.");
	ImGui::SameLine();
	if (ImGui::ArrowButton("##back", ImGuiDir_Left)) seek_ticks(workspace, model, std::max(shown - 1, 0), false);
	ui_kit::tooltip("A tick back.");
	ImGui::SameLine();
	if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) seek_ticks(workspace, model, shown + 1, false);
	ui_kit::tooltip("A tick on.");
	ImGui::SameLine();
	// The frame the clip shows after the track, its width kept for it; in a window too
	// narrow for both, in the track's tooltip.
	const anim::SkeletalClips::LoadedClip *clip = model.skeleton()->find_clip_variant(model.clip_key(), model.clip_variant());
	char frame[48];
	std::snprintf(frame, sizeof(frame), "frame %.1f / %u", model.clip_frame(clock), clip ? clip->clip.frame_count : 0u);
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float track_room = ImGui::GetContentRegionAvail().x - ui_kit::text_width(frame) - spacing;
	const bool frame_beside = track_room >= ImGui::GetFontSize() * 5.0f;
	int scrub = shown;
	ImGui::SetNextItemWidth(frame_beside ? track_room : ImGui::GetContentRegionAvail().x);
	if (ImGui::SliderInt("##clip_ticks", &scrub, 0, length, "tick %d")) seek_ticks(workspace, model, scrub, true);
	if (!frame_beside) ui_kit::tooltip(frame);
	const ImVec2 track_min = ImGui::GetItemRectMin(), track_max = ImGui::GetItemRectMax();
	if (frame_beside) {
		ImGui::SameLine();
		ImGui::TextUnformatted(frame);
	}

	// The events under the track.
	const float strip = 8.0f;
	const float grab = ImGui::GetStyle().GrabMinSize * 0.5f + ImGui::GetStyle().FramePadding.x;
	const float left = track_min.x + grab, right = track_max.x - grab;
	ImGui::SetCursorScreenPos(ImVec2(track_min.x, track_max.y + 2.0f));
	ImGui::InvisibleButton("##events", ImVec2(std::max(1.0f, track_max.x - track_min.x), strip));
	const bool hovered = ImGui::IsItemHovered();
	const bool clicked = ImGui::IsItemClicked();
	ImDrawList *paint = ImGui::GetWindowDrawList();
	const float mouse = ImGui::GetIO().MousePos.x;
	const PreviewClipEvent *under = nullptr;
	for (const PreviewClipEvent &event : model.clip_events()) {
		const float x = left + (right - left) * float(event.tick) / float(length);
		ImU32 color = kEventColor;
		if (event.trigger & (anim::kAnimEventFootLeft | anim::kAnimEventFootRight)) color = IM_COL32(120, 220, 120, 255);
		else if (event.trigger & (anim::kAnimEventFirePrimary | anim::kAnimEventFireSecondary | anim::kAnimEventFireMarker3))
			color = IM_COL32(240, 90, 80, 255);
		paint->AddRectFilled(ImVec2(x - 1.5f, track_max.y + 2.0f), ImVec2(x + 1.5f, track_max.y + 2.0f + strip), color);
		if (hovered && std::fabs(mouse - x) <= 4.0f) under = &event;
	}
	if (!under) return;
	std::string bits;
	for (const anim::AnimEventBit &bit : anim::kAnimEventBits)
		if (under->trigger & bit.mask) bits += (bits.empty() ? "" : ", ") + std::string(bit.name);
	ui_kit::tooltip("frame " + std::to_string(under->frame) + ", tick " +
	                std::to_string(under->tick) + ": " +
	                (bits.empty() ? std::string("no named bit") : bits));
	if (!clicked) return;
	seek_ticks(workspace, model, under->tick, true);
	// In the clip's own document the event is a record: select it.
	const SessionView &view = workspace.view();
	for (const auto &open : view.documents.open) {
		const auto *clip_document = dynamic_cast<const AnimationDocument *>(open.get());
		if (!clip_document || clip_document->path() != model.path() || clip_document->rows().empty()) continue;
		const Node &row = *clip_document->rows().front();
		if (size_t(under->frame) < row.collections[1].size())
			window_requests::select(workspace, *clip_document,
			                        {row.id, node_kind(AnimationKind::Event), row.collections[1][size_t(under->frame)]});
	}
}

// The level (Auto or one held), what Auto picks and why, the clock, what the overlays
// mark, Frame, and the registers, on a row that wraps whole controls in a narrow window.
void ModelViewportView::Tools::toolbar(Workspace &workspace, const ModelViewport &model, const ViewportContext &context) {
	const threedi::Threedi3di3 &shown = *model.model();
	ModelViewportOptions options = model.options();
	const float unit = ImGui::GetFontSize();
	int32_t projected = 0;
	const int automatic = model.auto_lod(&projected);
	char label[48];
	if (options.lod < 0) std::snprintf(label, sizeof(label), "Auto (level %d)", automatic);
	else std::snprintf(label, sizeof(label), "Level %d", std::min(options.lod, int(shown.lod_count) - 1));
	ui_kit::WrapRow row;
	row.next(ui_kit::field_width(unit * 11.0f, "Level"));
	ImGui::SetNextItemWidth(unit * 11.0f);
	if (ImGui::BeginCombo("Level", label)) {
		if (ImGui::Selectable("Auto", options.lod < 0)) options.lod = -1;
		for (size_t i = 0; i < shown.lod_count; ++i) {
			char text[48];
			std::snprintf(text, sizeof(text), "Level %d (%d px)", int(i), int(shown.lods[i].lod_threshold));
			if (ImGui::Selectable(text, options.lod == int(i))) options.lod = int(i);
		}
		ImGui::EndCombo();
	}
	ui_kit::tooltip("Auto draws the level the game picks at this distance: the model's projected "
	                "radius against each level's threshold. A level held stays at any distance.");
	char radius[32];
	std::snprintf(radius, sizeof(radius), "%.1f px", projected / 65536.0);
	row.next(ui_kit::text_width(radius));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", radius);
	ui_kit::tooltip("The model's projected radius, the size Auto measures.");
	// The preview clock: Run or Pause (Play is the game's).
	const bool playing = context.input.clock.playing();
	const char *clock = playing ? "Pause" : "Run";
	row.next(ui_kit::button_width(clock));
	if (ImGui::Button(clock)) set_playing(workspace, model, !playing);
	ui_kit::tooltip("Run or hold the preview clock: the model's part animations, flipbooks and colour "
	                "generators.");
	row.next(ui_kit::button_width("Show"));
	if (ImGui::Button("Show")) ImGui::OpenPopup("marks");
	ui_kit::tooltip("What the viewport marks over the model.");
	if (ImGui::BeginPopup("marks")) {
		ImGui::Checkbox("User points", &options.overlays.user_points);
		ImGui::Checkbox("Lights", &options.overlays.lights);
		ImGui::Checkbox("Part pivots", &options.overlays.pivots);
		ImGui::EndPopup();
	}
	static const char *const kSnapNames[] = {"Free", "1/64 m", "1/16 m", "1/4 m", "1 m"};
	row.next(ui_kit::field_width(unit * 5.0f, "Snap"));
	ImGui::SetNextItemWidth(unit * 5.0f);
	ImGui::Combo("Snap", &snap, kSnapNames, IM_ARRAYSIZE(kSnapNames));
	ui_kit::tooltip("A dragged marker's place snaps to this grid on each of the file's axes. Hold "
	                "Alt to place freely.");
	row.next(ui_kit::button_width("Frame"));
	if (ImGui::Button("Frame")) {
		// The selected record's marker, else the whole model.
		const SessionView &view = workspace.view();
		std::vector<NodeId> ids;
		if (view.documents.active == model.path() && view.documents.selection.primary.child)
			ids.push_back(view.documents.selection.primary.child);
		CanvasWindowRequests requests(workspace);
		std::string error;
		model.command(context, "frame", ids, requests, error);
	}
	ui_kit::tooltip("Look at the selected marker, or at the whole model (F).");
	row.next(ui_kit::button_width("Registers"));
	ImGui::BeginDisabled(shown.ctrl.count == 0);
	if (ImGui::Button("Registers")) ImGui::OpenPopup("registers");
	ImGui::EndDisabled();
	ui_kit::tooltip(shown.ctrl.count == 0 ? "The model declares no CTRL registers."
	                                      : "Hold the model's CTRL registers at a value, as the game's "
	                                        "entity would drive them.");
	if (options != model.options()) set_options(workspace, model, options);
	if (ImGui::BeginPopup("registers")) {
		registers(workspace, model);
		ImGui::EndPopup();
	}
}

void ModelViewportView::Tools::registers(Workspace &workspace, const ModelViewport &model) {
	const threedi::Threedi3di3 &shown = *model.model();
	ModelViewportOptions options = model.options();
	const float unit = ImGui::GetFontSize();
	for (uint32_t i = 0; i < shown.ctrl.count; ++i) {
		const std::string name = strutil::fixed_string(shown.ctrl.registers[i].name, sizeof(shown.ctrl.registers[i].name));
		if (name.empty()) continue;
		const auto held = options.ctrl.find(name);
		int64_t value = held == options.ctrl.end() ? 0 : held->second;
		const int64_t low = -kRegisterRange, high = kRegisterRange;
		ImGui::SetNextItemWidth(unit * 12.0f);
		if (ImGui::SliderScalar(name.c_str(), ImGuiDataType_S64, &value, &low, &high)) {
			if (value == 0) options.ctrl.erase(name);
			else options.ctrl[name] = value;
		}
	}
	if (ImGui::Button("Reset all")) options.ctrl.clear();
	ui_kit::tooltip("Let every register go back to 0.");
	if (options != model.options()) set_options(workspace, model, options);
}

} // namespace opennova::editor
