#include "model_preview_pane.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_document.h>
#include <editor/preview/model_overlay.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <runtime/anim/anim_event_bits.h>

namespace opennova::editor {

namespace {

constexpr float kWheelDolly = 0.85f;      // one wheel notch toward the target
constexpr int64_t kRegisterRange = 32767; // a slider's reach (the value is the register's word)
constexpr float kDragThreshold = 3.0f;    // pixels before a press becomes an orbit or a pan
constexpr float kPickSlop = 8.0f;         // how far from a marker a click still takes it
constexpr ImU32 kUserPointColor = IM_COL32(255, 220, 90, 255);
constexpr ImU32 kPivotColor = IM_COL32(110, 220, 255, 255);
constexpr ImU32 kSelectedColor = IM_COL32(255, 200, 60, 255);
constexpr ImU32 kHoverColor = IM_COL32(120, 190, 255, 220);

std::string fixed_name(const char *name, size_t size) {
	size_t length = 0;
	while (length < size && name[length]) ++length;
	return std::string(name, length);
}

ImU32 light_color(uint32_t rgb) {
	return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

// The model document the preview shows (open), or null.
const ModelDocument *previewed(const SessionView &view, const std::string &path) {
	for (const auto &open : view.documents)
		if (open && open->path() == path) return dynamic_cast<const ModelDocument *>(open.get());
	return nullptr;
}

} // namespace

void ModelPreviewPane::end_press_() {
	if (press_.handle && press_.sent) window_requests::end_edit(host_, press_.path);
	press_ = Press();
}

void ModelPreviewPane::draw() {
	if (!viewport_) {
		end_press_();
		ui_kit::empty_state(model_preview_status_message(ModelPreviewStatus::NoDevice, std::string()).c_str());
		return;
	}
	ModelPreviewModel &model = viewport_->model();
	if (model.status() != ModelPreviewStatus::Ready || !model.model()) {
		end_press_();
		ui_kit::empty_state(model_preview_status_message(model.status(), model.detail()).c_str());
		// An animation no item pairs: the author picks the model it plays on.
		if (model.status() == ModelPreviewStatus::NoRig) {
			ui_kit::WrapRow row;
			rig_chooser_(row, model);
		}
		return;
	}
	// An animation: the model it plays on (the Preview window's line names it), the clip the
	// selection plays (its slot in words, the table's key in the tooltip) and where the
	// pairing comes from, cut to the room left (whole in its tooltip) or on a line of its own
	// in a narrow window.
	if (model.animating()) {
		const PreviewRig &rig = model.rig();
		ui_kit::WrapRow row;
		rig_chooser_(row, model);
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
	toolbar_(model);
	const float timeline = model.animating() ? ImGui::GetFrameHeightWithSpacing() * 2.0f + 6.0f : 0.0f;
	canvas_(model, std::max(48.0f, ImGui::GetContentRegionAvail().y - timeline));
	if (model.animating()) timeline_(model);
}

// The model an animation plays on: Auto (the one an item pairs with the table) or a model
// of the project's.
void ModelPreviewPane::rig_chooser_(ui_kit::WrapRow &row, ModelPreviewModel &model) {
	const SessionView &view = host_.view();
	ModelPreviewOptions options = model.options();
	const float width = ImGui::GetFontSize() * 10.0f;
	row.next(ui_kit::field_width(width, "Plays on"));
	ImGui::SetNextItemWidth(width);
	if (ImGui::BeginCombo("Plays on", options.rig_model.empty() ? "Auto" : options.rig_model.c_str())) {
		if (ImGui::Selectable("Auto", options.rig_model.empty())) options.rig_model.clear();
		for (const AssetEntry &entry : view.scan.entries)
			if (entry.kind == AssetKind::Model &&
			    ImGui::Selectable(entry.logical_name.c_str(), entry.logical_name == options.rig_model))
				options.rig_model = entry.logical_name;
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The model the animation plays on. Auto takes the graphic of an item whose anim_def "
		                  "names the table.");
	if (options != model.options()) model.set_options(options);
}

// The clip the selection plays: run or hold it, step a tick, scrub; its trigger events
// under the track (a click on one seeks there and, in the clip's own document, selects it).
void ModelPreviewPane::timeline_(ModelPreviewModel &model) {
	const int32_t length = model.clip_length_ticks();
	if (model.clip_key().empty() || length <= 0) {
		ImGui::TextDisabled("%s", model.rig().table.empty() || model.clip_key().empty()
		                                  ? "Select a row of the table to play its clip."
		                                  : "The clip does not load in the rig.");
		return;
	}
	ModelPreviewOptions options = model.options();
	const int32_t ticks = model.clip_ticks();
	const int32_t shown = model.clip_loops() ? ticks % length : std::min(ticks, length);
	if (ImGui::Button(options.playing ? "Pause##clip" : "Run##clip")) options.playing = !options.playing;
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(options.playing ? "Hold the clip where it is." : "Run the clip.");
	ImGui::SameLine();
	if (ImGui::ArrowButton("##back", ImGuiDir_Left)) model.seek_ticks(std::max(shown - 1, 0));
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("A tick back.");
	ImGui::SameLine();
	if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) model.seek_ticks(shown + 1);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("A tick on.");
	ImGui::SameLine();
	// The frame the clip shows after the track, its width kept for it; in a window too
	// narrow for both, in the track's tooltip.
	const anim::SkeletalClips::LoadedClip *clip = model.skeleton()->find_clip_variant(model.clip_key(), model.clip_variant());
	char frame[48];
	std::snprintf(frame, sizeof(frame), "frame %.1f / %u", model.clip_frame(), clip ? clip->clip.frame_count : 0u);
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float track_room = ImGui::GetContentRegionAvail().x - ui_kit::text_width(frame) - spacing;
	const bool frame_beside = track_room >= ImGui::GetFontSize() * 5.0f;
	int scrub = shown;
	ImGui::SetNextItemWidth(frame_beside ? track_room : ImGui::GetContentRegionAvail().x);
	if (ImGui::SliderInt("##clip_ticks", &scrub, 0, length, "tick %d")) {
		model.seek_ticks(scrub);
		options.playing = false;
	}
	if (!frame_beside && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", frame);
	const ImVec2 track_min = ImGui::GetItemRectMin(), track_max = ImGui::GetItemRectMax();
	if (frame_beside) {
		ImGui::SameLine();
		ImGui::TextUnformatted(frame);
	}
	if (options != model.options()) model.set_options(options);

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
		ImU32 color = kUserPointColor;
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
	ImGui::SetTooltip("frame %d, tick %d: %s", under->frame, under->tick, bits.empty() ? "no named bit" : bits.c_str());
	if (!clicked) return;
	model.seek_ticks(under->tick);
	ModelPreviewOptions held = model.options();
	held.playing = false;
	model.set_options(held);
	// In the clip's own document the event is a record: select it.
	const SessionView &view = host_.view();
	for (const auto &open : view.documents) {
		const auto *clip = dynamic_cast<const AnimationDocument *>(open.get());
		if (!clip || clip->path() != model.shown_path() || clip->rows().empty()) continue;
		const Node &row = *clip->rows().front();
		if (size_t(under->frame) < row.collections[1].size())
			window_requests::select(host_, *clip,
			                        {row.id, node_kind(AnimationKind::Event), row.collections[1][size_t(under->frame)]});
	}
}

// The level (Auto or one held), what Auto picks and why, the clock, what the overlays
// mark, Frame, and the registers, on a row that wraps whole controls in a narrow window.
void ModelPreviewPane::toolbar_(ModelPreviewModel &model) {
	const threedi::Threedi3di3 &shown = *model.model();
	ModelPreviewOptions options = model.options();
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
			char row[48];
			std::snprintf(row, sizeof(row), "Level %d (%d px)", int(i), int(shown.lods[i].lod_threshold));
			if (ImGui::Selectable(row, options.lod == int(i))) options.lod = int(i);
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Auto draws the level the game picks at this distance: the model's projected radius "
		                  "against each level's threshold. A level held stays at any distance.");
	char radius[32];
	std::snprintf(radius, sizeof(radius), "%.1f px", projected / 65536.0);
	row.next(ui_kit::text_width(radius));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", radius);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("The model's projected radius, the size Auto measures.");
	// The clock: Run or Pause (Play is the game's).
	const char *clock = options.playing ? "Pause" : "Run";
	row.next(ui_kit::button_width(clock));
	if (ImGui::Button(clock)) options.playing = !options.playing;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Run or hold the model's clock: its part animations, flipbooks and colour generators.");
	row.next(ui_kit::button_width("Show"));
	if (ImGui::Button("Show")) ImGui::OpenPopup("marks");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the preview marks over the model.");
	if (ImGui::BeginPopup("marks")) {
		ImGui::Checkbox("User points", &options.overlays.user_points);
		ImGui::Checkbox("Lights", &options.overlays.lights);
		ImGui::Checkbox("Part pivots", &options.overlays.pivots);
		ImGui::EndPopup();
	}
	static const char *const kSnapNames[] = {"Free", "1/64 m", "1/16 m", "1/4 m", "1 m"};
	row.next(ui_kit::field_width(unit * 5.0f, "Snap"));
	ImGui::SetNextItemWidth(unit * 5.0f);
	ImGui::Combo("Snap", &snap_, kSnapNames, IM_ARRAYSIZE(kSnapNames));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("A dragged marker's place snaps to this grid on each of the file's axes. Hold Alt to "
		                  "place freely.");
	row.next(ui_kit::button_width("Frame"));
	if (ImGui::Button("Frame")) frame_(model);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look at the selected marker, or at the whole model (F).");
	row.next(ui_kit::button_width("Registers"));
	ImGui::BeginDisabled(shown.ctrl.count == 0);
	if (ImGui::Button("Registers")) ImGui::OpenPopup("registers");
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(shown.ctrl.count == 0 ? "The model declares no CTRL registers."
		                                        : "Hold the model's CTRL registers at a value, as the game's "
		                                          "entity would drive them.");
	if (options != model.options()) model.set_options(options);
	if (ImGui::BeginPopup("registers")) {
		registers_(model);
		ImGui::EndPopup();
	}
}

void ModelPreviewPane::registers_(ModelPreviewModel &model) {
	const threedi::Threedi3di3 &shown = *model.model();
	ModelPreviewOptions options = model.options();
	const float unit = ImGui::GetFontSize();
	for (uint32_t i = 0; i < shown.ctrl.count; ++i) {
		const std::string name = fixed_name(shown.ctrl.registers[i].name, sizeof(shown.ctrl.registers[i].name));
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
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Let every register go back to 0.");
	if (options != model.options()) model.set_options(options);
}

// The selected marker's place (the model's sphere when none is selected).
void ModelPreviewPane::frame_(ModelPreviewModel &model) {
	const SessionView &view = host_.view();
	const ModelDocument *document = previewed(view, model.shown_path());
	ModelOverlayKind kind = ModelOverlayKind::UserPoint;
	int index = -1;
	if (document && view.active_document == document->path() && model.shown_revision() == document->revision() &&
	    model_overlay_of(*document, view.selection, kind, index)) {
		for (const ModelOverlay &overlay : model.overlays())
			if (overlay.kind == kind && overlay.index == index) {
				PreviewVec3 center;
				float radius = 1.0f;
				model_preview_sphere(*model.model(), center, radius);
				const float around = overlay.kind == ModelOverlayKind::Light && overlay.radius > 0.0f
				                             ? overlay.radius
				                             : std::max(0.25f, radius * 0.15f);
				model.camera().frame(overlay.at, around, model.device_width(), model.device_height());
				return;
			}
	}
	model.frame();
}

void ModelPreviewPane::canvas_(ModelPreviewModel &model, float available_height) {
	const ImGuiIO &io = ImGui::GetIO();
	const ImVec2 region = ImGui::GetContentRegionAvail();
	const int width = std::max(64, int(region.x));
	const int height = std::max(48, int(available_height));
	// Every press on the canvas is the window's: the surface is the first item, so the
	// device's own item under it never takes the mouse.
	const ImVec2 base = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##surface", ImVec2(float(width), float(height)),
	                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();
	const bool activated = ImGui::IsItemActivated();
	const bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
	ImGui::SetCursorScreenPos(base);
	viewport_->draw(width, height);
	ImDrawList *paint = ImGui::GetWindowDrawList();
	paint->AddRect(base, ImVec2(base.x + float(width), base.y + float(height)), IM_COL32(90, 90, 90, 255));

	// The markers over the picture, the selected one ringed, and the one under the mouse.
	const SessionView &view = host_.view();
	const ModelDocument *document = previewed(view, model.shown_path());
	const bool current = document && model.shown_revision() == document->revision();
	ModelOverlayKind selected_kind = ModelOverlayKind::UserPoint;
	int selected = -1;
	if (current && view.active_document == document->path())
		model_overlay_of(*document, view.selection, selected_kind, selected);
	OrbitCamera &camera = model.camera();
	const std::vector<ModelOverlay> overlays = model.overlays();
	const float mx = io.MousePos.x - base.x, my = io.MousePos.y - base.y;
	const int under = hovered ? pick_model_overlay(overlays, camera, width, height, mx, my, kPickSlop) : -1;
	paint->PushClipRect(base, ImVec2(base.x + float(width), base.y + float(height)), true);
	for (size_t i = 0; i < overlays.size(); ++i) {
		const ModelOverlay &overlay = overlays[i];
		float x = 0.0f, y = 0.0f, depth = 0.0f;
		if (!camera.project(overlay.at, width, height, x, y, &depth)) continue;
		const ImVec2 at(base.x + x, base.y + y);
		const auto line_to = [&](const PreviewVec3 &tip, ImU32 color) {
			float tx = 0.0f, ty = 0.0f;
			if (camera.project(tip, width, height, tx, ty)) paint->AddLine(at, ImVec2(base.x + tx, base.y + ty), color, 1.5f);
		};
		const bool is_selected = overlay.kind == selected_kind && overlay.index == selected;
		switch (overlay.kind) {
		case ModelOverlayKind::UserPoint:
			if (overlay.has_direction) line_to(model.axis_tip(overlay), kUserPointColor);
			paint->AddQuadFilled(ImVec2(at.x, at.y - 4.0f), ImVec2(at.x + 4.0f, at.y), ImVec2(at.x, at.y + 4.0f),
			                     ImVec2(at.x - 4.0f, at.y), kUserPointColor);
			break;
		case ModelOverlayKind::Light: {
			const ImU32 color = light_color(overlay.color);
			if (overlay.radius > 0.0f && depth > 0.0f) {
				const float reach = overlay.radius * OrbitCamera::focal_pixels(width) / depth;
				if (reach > 2.0f && reach < 4.0f * float(width))
					paint->AddCircle(at, reach, (color & 0x00FFFFFFu) | 0x60000000u, 48);
			}
			if (overlay.has_direction) line_to(model.axis_tip(overlay), color);
			paint->AddCircleFilled(at, 4.5f, color);
			paint->AddCircle(at, 4.5f, IM_COL32(20, 20, 20, 255));
			break;
		}
		case ModelOverlayKind::Pivot:
			paint->AddLine(ImVec2(at.x - 5.0f, at.y), ImVec2(at.x + 5.0f, at.y), kPivotColor, 1.5f);
			paint->AddLine(ImVec2(at.x, at.y - 5.0f), ImVec2(at.x, at.y + 5.0f), kPivotColor, 1.5f);
			break;
		}
		if (int(i) == under) paint->AddCircle(at, 8.0f, kHoverColor, 16, 1.5f);
		if (is_selected) {
			paint->AddCircle(at, 9.0f, kSelectedColor, 16, 2.0f);
			// The selected marker's axis tip: the handle that turns it.
			float tx = 0.0f, ty = 0.0f;
			if (overlay.has_direction && camera.project(model.axis_tip(overlay), width, height, tx, ty))
				paint->AddCircleFilled(ImVec2(base.x + tx, base.y + ty), 4.0f, kSelectedColor);
		}
	}
	paint->PopClipRect();
	if (under >= 0 && !press_.dragging) ImGui::SetTooltip("%s", overlays[size_t(under)].name.c_str());

	// The left button: on the selected marker (or its axis tip) a drag moves (or turns) its
	// record; elsewhere a click selects the marker under it and a drag orbits (Shift: pans);
	// the middle button pans; the wheel dollies.
	if (activated) {
		end_press_();
		press_.active = true;
		press_.x = io.MousePos.x;
		press_.y = io.MousePos.y;
		press_.pan = ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || io.KeyShift;
		press_.pick = under;
		if (!press_.pan && current && selected >= 0 && !document->blocked()) {
			for (const ModelOverlay &overlay : overlays) {
				if (overlay.kind != selected_kind || overlay.index != selected) continue;
				float hx = 0.0f, hy = 0.0f;
				const auto near_mouse = [&](const PreviewVec3 &point) {
					return camera.project(point, width, height, hx, hy) && std::fabs(hx - mx) <= kPickSlop &&
					       std::fabs(hy - my) <= kPickSlop;
				};
				if (overlay.has_direction && near_mouse(model.axis_tip(overlay))) press_.which = ModelHandle::Axis;
				else if (overlay.kind != ModelOverlayKind::Pivot && near_mouse(overlay.at)) press_.which = ModelHandle::Place;
				else break;
				press_.handle = true;
				press_.marker = overlay;
				press_.offset_x = hx - mx;
				press_.offset_y = hy - my;
				press_.path = document->path();
				break;
			}
		}
	}
	if (press_.active) {
		if (!active) {
			// A click on a marker selects its record (while the picture is the document's).
			if (!press_.dragging && press_.pick >= 0 && size_t(press_.pick) < overlays.size() && current) {
				const NodeAddress record = model_overlay_record(*document, overlays[size_t(press_.pick)], model.lod());
				if (record.row) window_requests::select(host_, *document, record);
			}
			end_press_();
		} else {
			if (!press_.dragging && (std::fabs(io.MousePos.x - press_.x) > kDragThreshold ||
			                         std::fabs(io.MousePos.y - press_.y) > kDragThreshold)) {
				press_.dragging = true;
				if (press_.handle) press_.gesture = next_edit_gesture();
			}
			if (press_.dragging && press_.handle) {
				// The handle follows the mouse (kept where the press took it) in the plane that
				// faces the eye; each step is planned from the marker as it was pressed.
				std::vector<Edit> edits;
				const float snap = io.KeyAlt ? 0.0f : kModelHandleSnaps[std::clamp(snap_, 0, 4)];
				if (document && document->path() == press_.path &&
				    model.handle_edits(*document, press_.marker, press_.which, mx + press_.offset_x, my + press_.offset_y,
				                       snap, press_.gesture, edits) &&
				    !edits.empty()) {
					window_requests::edits(host_, *document, std::move(edits));
					press_.sent = true;
				}
			} else if (press_.dragging) {
				if (press_.pan) camera.pan(io.MouseDelta.x, io.MouseDelta.y, width);
				else camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
			}
		}
	}
	if (hovered && io.MouseWheel != 0.0f) camera.dolly(std::pow(kWheelDolly, io.MouseWheel));
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput;
	if (double_clicked || (focused && ImGui::IsKeyPressed(ImGuiKey_F, false))) frame_(model);
}

} // namespace opennova::editor
